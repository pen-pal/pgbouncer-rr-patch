/*
Copyright 2015-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.

Licensed under the Amazon Software License (the "License"). 
You may not use this file except in compliance with the License. A copy of the License is located at

    http://aws.amazon.com/asl/

or in the "license" file accompanying this file. 
This file is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, express or implied. See the License for the specific language governing permissions and limitations under the License.
*/

/*
 * pgbouncer-rr extension: call external python function
 */

#include <Python.h>
#include "bouncer.h"
#include <usual/pgutil.h>

char *pycall(PgSocket *client, char *username, char *query_str, char *py_file,
		char* py_function) {
	PyObject *pName = NULL, *pModule = NULL, *pFunc = NULL;
	PyObject *pArgs = NULL, *pValue = NULL, *pArgsCnt = NULL, *pCode = NULL;
	PyObject *ptype, *perror, *ptraceback, *bytes_obj, *string_obj;
	char *py_pathtmp, *py_filetmp, *py_path, *py_module, *ext;
	char *res = NULL;
	int agCount = 0;
	const char *dbname = NULL;

        /* setup python search path */
	py_pathtmp = strdup(py_file);
	if (py_pathtmp == NULL) {
		slog_error(client, "out of memory");
		return NULL;
	}
	py_path = malloc(strlen(py_file) + 20) ;
	if (py_path == NULL) {
		slog_error(client, "out of memory");
		free(py_pathtmp);
		return NULL;
	}
        sprintf(py_path,"PYTHONPATH=%s",dirname(py_pathtmp)) ;
	putenv(py_path) ;

	/* setup python module name, function name */
	py_filetmp = strdup(py_file);
	if (py_filetmp == NULL) {
		slog_error(client, "out of memory");
		free(py_pathtmp);
		free(py_path);
		return NULL;
	}
	py_module = (char *) basename(py_filetmp);
	ext = strrchr(py_module, '.');
	if (ext)
		ext[0] = '\0';

        /* Initialize the Python interpreter
         * NOTE: This call is a no-op on subsequent calls, as we do not 
         * call PyFinalize(). This 
         * a) avoids the overhead of repeatedly reloading the interpreter
         * b) allows the use of global variables for persisting data in the
         *    routing / rewriting functions between calls.
         */
	Py_Initialize();

	/* Load python module */
	pName = PyUnicode_FromString(py_module);
	if (pName == NULL) {
		slog_error(client, "Python module <%s> did not load", py_module);
		goto finish;
	}
	pModule = PyImport_Import(pName);
	if (pModule == NULL) {
		slog_error(client, "Python module <%s> did not load", py_module);
		goto finish;
	}

	/* Prepare to call python function */
	pFunc = PyObject_GetAttrString(pModule, py_function);
	if (!pFunc) {
		slog_error(client, "Python Function <%s> not found in module <%s>",
				py_function, py_module);
		goto finish;
	}
	if (!PyCallable_Check(pFunc)) {
		slog_error(client,
				"Python Function <%s> in module <%s> is not callable!",
				py_function, py_module);
		goto finish;
	}

#if PY_MAJOR_VERSION >= 3
	pCode = PyObject_GetAttrString(pFunc, "__code__");
#else
	pCode = PyObject_GetAttrString(pFunc, "code");
#endif
	if (pCode) {
		/* get the number of arguments */
		pArgsCnt = PyObject_GetAttrString(pCode, "co_argcount");
	
		if (pArgsCnt == NULL) {
			slog_error(client, "Could not obtain arg count of Python Function <%s>", 
					py_function);
			goto finish;
		}

		agCount = PyLong_AsLong(pArgsCnt);
		dbname = client->db->dbname;
		Py_DECREF(pArgsCnt);
		Py_DECREF(pCode);
	} else {
		agCount = 2;
	}

	/* Call function with two arguments - username, query_str and optionally with dbname */
	pArgs = PyTuple_New(agCount);
	if (pArgs == NULL) {
		slog_error(client, "Python module <%s>: out of memory", py_module);
		goto finish;
	}
	pValue = PyUnicode_FromString(username);
	if (pValue == NULL) {
		slog_error(client, "Python module <%s>: out of memory", py_module);
		goto finish;
	}
	PyTuple_SetItem(pArgs, 0, pValue);
	pValue = PyUnicode_FromString(query_str);
	if (pValue == NULL) {
		slog_error(client, "Python module <%s>: out of memory", py_module);
		goto finish;
	}
	PyTuple_SetItem(pArgs, 1, pValue);

	/* If there is a dbname argument, add it to the tuple */
	if (agCount == 3) {
        pValue = PyUnicode_FromString(dbname);
		PyTuple_SetItem(pArgs, 2, pValue);
		if (pValue == NULL) {
			slog_error(client, "Python module <%s>: out of memory", py_module);
			goto finish;
		}
    }
	pValue = PyObject_CallObject(pFunc, pArgs);
	if (pValue == NULL) {
		slog_error(client, "Python Function <%s> failed to return a value",
				py_function);
		goto finish;
	}
	if (PyUnicode_Check(pValue)) {
            bytes_obj = PyUnicode_AsUTF8String(pValue);
            res = strdup(PyBytes_AsString(bytes_obj));
            Py_DECREF(bytes_obj);
    } else {
            res = NULL;
    }

    finish:
    if (PyErr_Occurred()) {
            PyErr_Fetch(&ptype, &perror, &ptraceback);
            PyErr_NormalizeException(&ptype, &perror, &ptraceback);
            string_obj = PyObject_Repr(perror);
            bytes_obj = PyUnicode_AsUTF8String(string_obj);
            slog_error(client, "Python error: %s", PyBytes_AsString(bytes_obj));
            Py_DECREF(string_obj);
            Py_DECREF(bytes_obj);
    }
	free(py_pathtmp);
	free(py_filetmp);
	free(py_path);
	Py_XDECREF(pName);
	Py_XDECREF(pModule);
	Py_XDECREF(pFunc);
	Py_XDECREF(pArgs);
	Py_XDECREF(pValue);
	return res;
}

