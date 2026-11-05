#include "topenapiInt.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static int local_name(const char *name) {
    if (!((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') || *name == '_')) return 0;
    for (++name; *name; ++name) {
        if (!((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') ||
              (*name >= '0' && *name <= '9') || *name == '_')) return 0;
    }
    return 1;
}

static int binding_error(Tcl_Interp *interp, const char *code, Tcl_Obj *name, const char *message) {
    Topenapi_Error(interp, code, message);
    Tcl_SetObjResult(interp, Tcl_ObjPrintf("binding \"%s\": %s", Tcl_GetString(name), message));
    return TCL_ERROR;
}

static int array_index(const char *text, int *index) {
    if (!*text) return 0;
    *index = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || *index > (INT_MAX - (*text - '0')) / 10) return 0;
        *index = *index * 10 + (*text - '0');
    }
    return 1;
}

static cJSON *select_value(Tcl_Interp *interp, cJSON *prepared, Tcl_Obj *name, Tcl_Obj *path) {
    Tcl_Size count;
    Tcl_Obj **parts;
    if (Tcl_ListObjGetElements(interp, path, &count, &parts) != TCL_OK) return NULL;
    if (!count) {
        binding_error(interp, "BIND_MAPPING", name, "field path must not be empty");
        return NULL;
    }
    static const char *const sources[] = {"params", "query", "headers", "cookies", "body", "form", "files", NULL};
    int source;
    if (Tcl_GetIndexFromObj(interp, parts[0], sources, "binding source", TCL_EXACT, &source) != TCL_OK) return NULL;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(prepared, sources[source]);
    for (Tcl_Size i = 1; value && i < count; ++i) {
        cJSON *key = Topenapi_String(interp, parts[i]);
        if (!key) return NULL;
        if (cJSON_IsObject(value)) {
            value = source == 2 && i == 1 ? cJSON_GetObjectItem(value, key->valuestring)
                : cJSON_GetObjectItemCaseSensitive(value, key->valuestring);
        } else if (cJSON_IsArray(value)) {
            int index;
            if (!array_index(key->valuestring, &index)) {
                cJSON_Delete(key);
                binding_error(interp, "BIND_PATH", name, "array index must be a nonnegative decimal integer");
                return NULL;
            }
            value = cJSON_GetArrayItem(value, index);
        } else value = NULL;
        cJSON_Delete(key);
    }
    if (!value) binding_error(interp, "BIND_PATH", name, "field path is missing or cannot be traversed");
    return value;
}

static Tcl_Obj *owned(Tcl_Obj *value) {
    Tcl_IncrRefCount(value);
    return value;
}

static Tcl_Obj *binding_value(Tcl_Interp *interp, cJSON *value, int depth) {
    if (depth > 64) {
        Topenapi_Error(interp, "BIND_VALUE", "binding value nesting exceeds depth limit");
        return NULL;
    }
    if (cJSON_IsObject(value) || cJSON_IsArray(value)) {
        int object = cJSON_IsObject(value);
        Tcl_Obj *result = object ? Tcl_NewDictObj() : Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(result);
        cJSON *item;
        cJSON_ArrayForEach(item, value) {
            Tcl_Obj *child = binding_value(interp, item, depth + 1);
            if (!child) { Tcl_DecrRefCount(result); return NULL; }
            if (object) Tcl_DictObjPut(interp, result, Topenapi_Utf8(item->string), child);
            else Tcl_ListObjAppendElement(interp, result, child);
            Tcl_DecrRefCount(child);
        }
        return result;
    }
    if (cJSON_IsString(value)) return owned(Topenapi_Utf8(value->valuestring));
    if (cJSON_IsBool(value)) return owned(Tcl_NewBooleanObj(cJSON_IsTrue(value)));
    if (cJSON_IsNumber(value)) {
        double number = value->valuedouble;
        if (!isfinite(number)) {
            Topenapi_Error(interp, "BIND_VALUE", "binding number must be finite");
            return NULL;
        }
        // The upper bound is exclusive: converting 2^63 to a signed wide int is undefined.
        if (floor(number) == number && number >= -9223372036854775808.0 && number < 9223372036854775808.0)
            return owned(Tcl_NewWideIntObj((Tcl_WideInt)number));
        return owned(Tcl_NewDoubleObj(number));
    }
    return owned(Tcl_NewObj()); // JSON null is an empty Tcl value; absence is a path error.
}

int Topenapi_Bind(Tcl_Interp *interp, cJSON *request, cJSON *operation, Tcl_Obj *mapping) {
    Tcl_Size count;
    if (Tcl_DictObjSize(interp, mapping, &count) != TCL_OK) return TCL_ERROR;
    cJSON *prepared = NULL;
    char error[2048];
    if (!topenapi_prepare_request(request, operation, &prepared, error, sizeof(error)))
        return Topenapi_Error(interp, "VALIDATION", error);

    Tcl_Obj *pending = Tcl_NewDictObj();
    Tcl_IncrRefCount(pending);
    Tcl_DictSearch search;
    Tcl_Obj *name, *path;
    int done, status = Tcl_DictObjFirst(interp, mapping, &search, &name, &path, &done);
    if (status == TCL_OK) {
        for (; !done; Tcl_DictObjNext(&search, &name, &path, &done)) {
            cJSON *text = Topenapi_String(interp, name);
            if (!text) { status = TCL_ERROR; break; }
            int valid_name = local_name(text->valuestring);
            cJSON_Delete(text);
            if (!valid_name) {
                status = binding_error(interp, "BIND_MAPPING", name, "use a local scalar name matching [A-Za-z_][A-Za-z0-9_]*");
                break;
            }
            cJSON *value = select_value(interp, prepared, name, path);
            if (!value) { status = TCL_ERROR; break; }
            Tcl_Obj *converted = binding_value(interp, value, 0);
            if (!converted) { status = TCL_ERROR; break; }
            Tcl_DictObjPut(interp, pending, name, converted);
            Tcl_DecrRefCount(converted);
        }
        Tcl_DictObjDone(&search);
    }
    cJSON_Delete(prepared);
    // The schema and input are no longer used. A variable write trace may delete
    // the validator or recursively call bind without invalidating staged values.
    if (status == TCL_OK) {
        Tcl_Obj *value;
        Tcl_DictObjFirst(interp, pending, &search, &name, &value, &done);
        for (; !done; Tcl_DictObjNext(&search, &name, &value, &done)) {
            if (!Tcl_ObjSetVar2(interp, name, NULL, value, TCL_LEAVE_ERR_MSG)) { status = TCL_ERROR; break; }
        }
        Tcl_DictObjDone(&search);
    }
    Tcl_DecrRefCount(pending);
    if (status == TCL_OK) Tcl_ResetResult(interp);
    return status;
}
