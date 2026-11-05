#ifndef TOPENAPI_INT_H
#define TOPENAPI_INT_H
#include "topenapi_tcl.h"
#include <cJSON.h>
#if TCL_MAJOR_VERSION < 9
typedef int Tcl_Size;
#endif
int Topenapi_Error(Tcl_Interp *interp, const char *code, const char *message);
int Topenapi_Get(Tcl_Interp *interp, Tcl_Obj *dict, const char *key, Tcl_Obj **value);
cJSON *Topenapi_String(Tcl_Interp *interp, Tcl_Obj *value);
Tcl_Obj *Topenapi_Utf8(const char *text);
#endif
