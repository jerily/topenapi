#include "topenapiInt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Topenapi_Error(Tcl_Interp *interp, const char *code, const char *message) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, -1));
    Tcl_SetErrorCode(interp, "TOPENAPI", code, NULL);
    return TCL_ERROR;
}

int Topenapi_Get(Tcl_Interp *interp, Tcl_Obj *dict, const char *key, Tcl_Obj **value) {
    *value = NULL;
    if (!dict) return TCL_OK;
    Tcl_Obj *name = Tcl_NewStringObj(key, -1);
    Tcl_IncrRefCount(name);
    int status = Tcl_DictObjGet(interp, dict, name, value);
    Tcl_DecrRefCount(name);
    return status;
}

static int utf8_text(Tcl_Interp *interp, Tcl_Obj *value, Tcl_DString *buffer) {
    Tcl_Size length;
    const Tcl_UniChar *chars = Tcl_GetUnicodeFromObj(value, &length);
    for (Tcl_Size i = 0; i < length; ++i) {
        if (chars[i] == 0) return Topenapi_Error(interp, "NUL", "text must not contain NUL characters");
    }
    const char *text = Tcl_GetStringFromObj(value, &length);
    Tcl_Encoding encoding = Tcl_GetEncoding(interp, "utf-8");
    if (!encoding) return TCL_ERROR;
    Tcl_UtfToExternalDString(encoding, text, length, buffer);
    Tcl_FreeEncoding(encoding);
    return TCL_OK;
}

cJSON *Topenapi_String(Tcl_Interp *interp, Tcl_Obj *value) {
    Tcl_DString buffer;
    Tcl_DStringInit(&buffer);
    if (utf8_text(interp, value, &buffer) != TCL_OK) {
        Tcl_DStringFree(&buffer);
        return NULL;
    }
    cJSON *result = cJSON_CreateString(Tcl_DStringValue(&buffer));
    Tcl_DStringFree(&buffer);
    if (!result) Topenapi_Error(interp, "MEMORY", "unable to allocate JSON string");
    return result;
}

Tcl_Obj *Topenapi_Utf8(const char *text) {
    Tcl_DString buffer;
    Tcl_DStringInit(&buffer);
    Tcl_Encoding encoding = Tcl_GetEncoding(NULL, "utf-8");
    Tcl_ExternalToUtfDString(encoding, text, -1, &buffer);
    Tcl_Obj *result = Tcl_NewStringObj(Tcl_DStringValue(&buffer), Tcl_DStringLength(&buffer));
    Tcl_DStringFree(&buffer);
    Tcl_FreeEncoding(encoding);
    return result;
}

static cJSON *parse_json(Tcl_Interp *interp, Tcl_Obj *obj) {
    Tcl_DString buffer;
    Tcl_DStringInit(&buffer);
    if (utf8_text(interp, obj, &buffer) != TCL_OK) {
        Tcl_DStringFree(&buffer);
        return NULL;
    }
    const char *text = Tcl_DStringValue(&buffer);
    /* cJSON strings cannot represent embedded NUL. Reject the escape instead
     * of silently validating a truncated value. Escaped backslashes are skipped. */
    for (const char *p = text; *p; ++p) {
        if (*p == '\\') {
            if (strncmp(p + 1, "u0000", 5) == 0) {
                Tcl_DStringFree(&buffer);
                Topenapi_Error(interp, "NUL", "JSON strings must not contain NUL characters");
                return NULL;
            }
            if (p[1]) ++p;
        }
    }
    cJSON *value = cJSON_ParseWithLengthOpts(text, (size_t)Tcl_DStringLength(&buffer) + 1, NULL, 1);
    Tcl_DStringFree(&buffer);
    if (!value) Topenapi_Error(interp, "JSON", "invalid JSON (expected one complete value)");
    return value;
}

static int validation_result(Tcl_Interp *interp, cJSON *value, cJSON *schema, int request) {
    char error[2048];
    int valid = request ? topenapi_validate_request(value, schema, error, sizeof(error))
                        : topenapi_validate_schema(value, schema, error, sizeof(error));
    Tcl_Obj *result = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, result, Tcl_NewStringObj("valid", -1), Tcl_NewBooleanObj(valid));
    Tcl_DictObjPut(interp, result, Tcl_NewStringObj("error", -1), Topenapi_Utf8(error));
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

static int validate_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "valueJson schemaJson");
        return TCL_ERROR;
    }
    cJSON *value = parse_json(interp, objv[1]);
    if (!value) return TCL_ERROR;
    cJSON *schema = parse_json(interp, objv[2]);
    int status = schema ? validation_result(interp, value, schema, data != NULL) : TCL_ERROR;
    cJSON_Delete(value);
    cJSON_Delete(schema);
    return status;
}

static int from_twebserver_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    (void)data;
    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "requestDict ?parsedFormDict?");
        return TCL_ERROR;
    }
    cJSON *request = NULL;
    if (Topenapi_FromTwebserver(interp, objv[1], objc == 3 ? objv[2] : NULL, &request) != TCL_OK)
        return TCL_ERROR;
    char *json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!json) return Topenapi_Error(interp, "MEMORY", "unable to serialize request");
    Tcl_SetObjResult(interp, Topenapi_Utf8(json));
    cJSON_free(json);
    return TCL_OK;
}

static Tcl_Obj *json_to_tcl(cJSON *value) {
    if (cJSON_IsObject(value)) {
        Tcl_Obj *dict = Tcl_NewDictObj();
        cJSON *item;
        cJSON_ArrayForEach(item, value)
            Tcl_DictObjPut(NULL, dict, Topenapi_Utf8(item->string), json_to_tcl(item));
        return dict;
    }
    if (cJSON_IsArray(value)) {
        Tcl_Obj *list = Tcl_NewListObj(0, NULL);
        cJSON *item;
        cJSON_ArrayForEach(item, value) Tcl_ListObjAppendElement(NULL, list, json_to_tcl(item));
        return list;
    }
    if (cJSON_IsString(value)) return Topenapi_Utf8(value->valuestring);
    if (cJSON_IsNumber(value)) return Tcl_NewDoubleObj(value->valuedouble);
    if (cJSON_IsBool(value)) return Tcl_NewBooleanObj(cJSON_IsTrue(value));
    return Tcl_NewObj();
}

static int files_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    (void)data;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "requestJson");
        return TCL_ERROR;
    }
    cJSON *request = parse_json(interp, objv[1]);
    if (!request) return TCL_ERROR;
    cJSON *files = cJSON_GetObjectItemCaseSensitive(request, "files");
    int status = TCL_OK;
    if (!cJSON_IsObject(request) || (files && !cJSON_IsObject(files)))
        status = Topenapi_Error(interp, "REQUEST", "expected a request object with a files object");
    else Tcl_SetObjResult(interp, files ? json_to_tcl(files) : Tcl_NewDictObj());
    cJSON_Delete(request);
    return status;
}

typedef struct {
    cJSON *operation;
    Tcl_Command token;
} Validator;

static void delete_validator(ClientData data) {
    Validator *validator = data;
    cJSON_Delete(validator->operation);
    free(validator);
}

static int validator_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    static const char *const commands[] = {"validate", "destroy", NULL};
    int index;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "validate requestJson | destroy");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], commands, "subcommand", TCL_EXACT, &index) != TCL_OK)
        return TCL_ERROR;
    if (objc != (index == 0 ? 3 : 2)) {
        Tcl_WrongNumArgs(interp, 2, objv, index == 0 ? "requestJson" : "");
        return TCL_ERROR;
    }
    Validator *validator = data;
    if (index == 1) return Tcl_DeleteCommandFromToken(interp, validator->token);
    cJSON *request = parse_json(interp, objv[2]);
    if (!request) return TCL_ERROR;
    int status = validation_result(interp, request, validator->operation, 1);
    cJSON_Delete(request);
    return status;
}

static int compile_command(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "operationJson");
        return TCL_ERROR;
    }
    cJSON *operation = parse_json(interp, objv[1]);
    if (!operation) return TCL_ERROR;
    if (!cJSON_IsObject(operation)) {
        cJSON_Delete(operation);
        return Topenapi_Error(interp, "SCHEMA", "operation must be a JSON object");
    }
    Validator *validator = calloc(1, sizeof(*validator));
    if (!validator) {
        cJSON_Delete(operation);
        return Topenapi_Error(interp, "MEMORY", "unable to allocate validator");
    }
    validator->operation = operation;
    unsigned long long *counter = data;
    char name[80];
    do {
        snprintf(name, sizeof(name), "::topenapi::validator%llu", ++*counter);
    } while (Tcl_FindCommand(interp, name, NULL, TCL_GLOBAL_ONLY));
    validator->token = Tcl_CreateObjCommand(interp, name, validator_command, validator, delete_validator);
    if (!validator->token) {
        delete_validator(validator);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(name, -1));
    return TCL_OK;
}

static void delete_counter(ClientData data) { free(data); }

int Topenapi_Init(Tcl_Interp *interp) {
#if TCL_MAJOR_VERSION >= 9
    if (!Tcl_InitStubs(interp, "9.0", 0)) return TCL_ERROR;
#else
    if (!Tcl_InitStubs(interp, "8.6", 0)) return TCL_ERROR;
#endif
    if (!Tcl_FindNamespace(interp, "::topenapi", NULL, TCL_GLOBAL_ONLY) &&
        !Tcl_CreateNamespace(interp, "::topenapi", NULL, NULL)) return TCL_ERROR;
    unsigned long long *counter = calloc(1, sizeof(*counter));
    if (!counter) return Topenapi_Error(interp, "MEMORY", "unable to initialize package");
    if (!Tcl_CreateObjCommand(interp, "::topenapi::compile", compile_command, counter, delete_counter)) {
        free(counter);
        return TCL_ERROR;
    }
    Tcl_CreateObjCommand(interp, "::topenapi::validate_schema", validate_command, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::topenapi::validate_request", validate_command, (ClientData)1, NULL);
    Tcl_CreateObjCommand(interp, "::topenapi::from_twebserver", from_twebserver_command, NULL, NULL);
    Tcl_CreateObjCommand(interp, "::topenapi::files", files_command, NULL, NULL);
    return Tcl_PkgProvide(interp, "topenapi", TOPENAPI_VERSION);
}
