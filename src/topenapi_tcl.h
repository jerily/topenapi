#ifndef TOPENAPI_TCL_H
#define TOPENAPI_TCL_H
#include <tcl.h>
#include "topenapi.h"

#ifdef __cplusplus
extern "C" {
#endif
DLLEXPORT int Topenapi_Init(Tcl_Interp *interp);
/* Return TCL_OK or TCL_ERROR. On success *out is owned by the caller, who must
 * cJSON_Delete it. No Tcl objects are retained. parsed_form may be NULL except
 * for a nonempty form body. It is the result of twebserver::get_form. */
int Topenapi_FromTwebserver(Tcl_Interp *interp, Tcl_Obj *request,
                          Tcl_Obj *parsed_form, struct cJSON **out);
#ifdef __cplusplus
}
#endif
#endif
