#include "topenapi_tcl.h"
static int app_init(Tcl_Interp *interp) { return Tcl_Init(interp); }
int main(int argc, char **argv) {
    Tcl_FindExecutable(argv[0]);
    Tcl_StaticPackage(NULL, "Topenapi", Topenapi_Init, NULL);
    Tcl_Main(argc, argv, app_init);
    return 0;
}
