#include "topenapiInt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void lowercase(char *text) {
    for (; *text; ++text) if (*text >= 'A' && *text <= 'Z') *text += 'a' - 'A';
}

static int add_item(Tcl_Interp *interp, cJSON *object, const char *key, cJSON *value) {
    if (value && cJSON_AddItemToObject(object, key, value)) return TCL_OK;
    cJSON_Delete(value);
    return Topenapi_Error(interp, "MEMORY", "unable to allocate request field");
}

/* Only explicitly multi-valued dictionaries become arrays. Tcl's internal
 * representation is never used to guess the type of an ordinary string. */
static int copy_fields(Tcl_Interp *interp, Tcl_Obj *dict, cJSON *target, int multi, int headers) {
    if (!dict) return TCL_OK;
    Tcl_DictSearch search;
    Tcl_Obj *key, *value;
    int done;
    if (Tcl_DictObjFirst(interp, dict, &search, &key, &value, &done) != TCL_OK) return TCL_ERROR;
    int status = TCL_OK;
    for (; !done; Tcl_DictObjNext(&search, &key, &value, &done)) {
        cJSON *name = Topenapi_String(interp, key);
        cJSON *item = NULL;
        if (!name) { status = TCL_ERROR; break; }
        if (headers) lowercase(name->valuestring);
        if (multi) {
            Tcl_Size count;
            Tcl_Obj **values;
            if (Tcl_ListObjGetElements(interp, value, &count, &values) != TCL_OK) {
                cJSON_Delete(name);
                status = TCL_ERROR;
                break;
            }
            item = cJSON_CreateArray();
            for (Tcl_Size i = 0; item && i < count; ++i) {
                cJSON *entry = Topenapi_String(interp, values[i]);
                if (!entry || !cJSON_AddItemToArray(item, entry)) {
                    cJSON_Delete(entry);
                    cJSON_Delete(item);
                    item = NULL;
                }
            }
        } else item = Topenapi_String(interp, value);
        if (!item) {
            cJSON_Delete(name);
            status = TCL_ERROR;
            break;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(target, name->valuestring);
        status = add_item(interp, target, name->valuestring, item);
        cJSON_Delete(name);
        if (status != TCL_OK) break;
    }
    Tcl_DictObjDone(&search);
    return status;
}

static int source_fields(Tcl_Interp *interp, Tcl_Obj *source, const char *single,
                         const char *multi, cJSON *target, int headers) {
    Tcl_Obj *value;
    if (Topenapi_Get(interp, source, single, &value) != TCL_OK ||
        copy_fields(interp, value, target, 0, headers) != TCL_OK) return TCL_ERROR;
    if (multi && (Topenapi_Get(interp, source, multi, &value) != TCL_OK ||
                  copy_fields(interp, value, target, 1, headers) != TCL_OK)) return TCL_ERROR;
    return TCL_OK;
}

static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* With output == NULL this measures/validates in place, allocating no payload. */
static int base64(Tcl_Interp *interp, const char *text, size_t length, char *output, size_t *size) {
    *size = 0;
    if (length % 4) return Topenapi_Error(interp, "BASE64", "invalid base64 length");
    for (size_t i = 0; i < length; i += 4) {
        int a = base64_digit((unsigned char)text[i]);
        int b = base64_digit((unsigned char)text[i + 1]);
        int c = text[i + 2] == '=' ? 0 : base64_digit((unsigned char)text[i + 2]);
        int d = text[i + 3] == '=' ? 0 : base64_digit((unsigned char)text[i + 3]);
        int padding = (text[i + 2] == '=') + (text[i + 3] == '=');
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (padding && i + 4 != length) ||
            (text[i + 2] == '=' && text[i + 3] != '=') ||
            (padding == 2 && (b & 15)) || (padding == 1 && (c & 3)))
            return Topenapi_Error(interp, "BASE64", "invalid base64 content");
        unsigned int block = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6) | (unsigned)d;
        if (output) {
            output[*size] = (char)(block >> 16);
            if (padding < 2) output[*size + 1] = (char)(block >> 8);
            if (padding < 1) output[*size + 2] = (char)block;
        }
        *size += (size_t)(3 - padding);
    }
    return TCL_OK;
}

static int add_upload(Tcl_Interp *interp, cJSON *files, const char *field,
                      const char *filename, Tcl_Obj *encoded) {
    Tcl_Size length;
    const char *text = Tcl_GetStringFromObj(encoded, &length);
    size_t size;
    if (base64(interp, text, (size_t)length, NULL, &size) != TCL_OK) return TCL_ERROR;
    cJSON *list = cJSON_GetObjectItemCaseSensitive(files, field);
    if (!list) {
        list = cJSON_CreateArray();
        if (add_item(interp, files, field, list) != TCL_OK) return TCL_ERROR;
    }
    cJSON *entry = cJSON_CreateObject();
    if (!entry) return Topenapi_Error(interp, "MEMORY", "unable to allocate file metadata");
    if (!cJSON_AddStringToObject(entry, "filename", filename) ||
        !cJSON_AddStringToObject(entry, "source_key", filename) ||
        !cJSON_AddNumberToObject(entry, "size", (double)size) ||
        !cJSON_AddStringToObject(entry, "encoding", "base64") ||
        !cJSON_AddItemToArray(list, entry)) {
        cJSON_Delete(entry);
        return Topenapi_Error(interp, "MEMORY", "unable to allocate file metadata");
    }
    return TCL_OK;
}

static int extract_uploads(Tcl_Interp *interp, Tcl_Obj *parsed, cJSON *form, cJSON *files) {
    Tcl_Obj *uploads;
    if (Topenapi_Get(interp, parsed, "files", &uploads) != TCL_OK) return TCL_ERROR;
    if (!uploads) return TCL_OK;
    Tcl_Size unused;
    if (Tcl_DictObjSize(interp, uploads, &unused) != TCL_OK) return TCL_ERROR;
    cJSON *field = form->child;
    while (field) {
        cJSON *next = field->next;
        cJSON *value = cJSON_IsArray(field) ? field->child : field;
        int file_count = 0, text_count = 0;
        for (; value; value = cJSON_IsArray(field) ? value->next : NULL) {
            Tcl_Obj *name = Topenapi_Utf8(value->valuestring);
            Tcl_IncrRefCount(name);
            Tcl_Obj *encoded = NULL;
            int status = Tcl_DictObjGet(interp, uploads, name, &encoded);
            Tcl_DecrRefCount(name);
            if (status != TCL_OK) return status;
            if (encoded) {
                if (add_upload(interp, files, field->string, value->valuestring, encoded) != TCL_OK)
                    return TCL_ERROR;
                ++file_count;
            } else ++text_count;
        }
        if (file_count && text_count)
            return Topenapi_Error(interp, "FORM", "a field cannot mix uploaded files and ordinary values");
        if (file_count) cJSON_DeleteItemFromObjectCaseSensitive(form, field->string);
        field = next;
    }
    return TCL_OK;
}

static int cookie_header(Tcl_Interp *interp, cJSON *cookies, const char *header) {
    size_t length = strlen(header);
    char *copy = malloc(length + 1);
    if (!copy) return Topenapi_Error(interp, "MEMORY", "unable to allocate cookie metadata");
    memcpy(copy, header, length + 1);
    char *part = copy;
    int status = TCL_OK;
    while (part && *part) {
        char *next = strchr(part, ';');
        if (next) *next++ = '\0';
        while (*part == ' ' || *part == '\t') ++part;
        char *equal = strchr(part, '=');
        if (equal && equal != part) {
            *equal++ = '\0';
            char *end = part + strlen(part);
            while (end > part && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            /* Keep the first cookie with a name, matching common HTTP parsers. */
            if (!cJSON_GetObjectItemCaseSensitive(cookies, part) &&
                add_item(interp, cookies, part, cJSON_CreateString(equal)) != TCL_OK) {
                status = TCL_ERROR;
                break;
            }
        }
        part = next;
    }
    free(copy);
    return status;
}

static int add_body(Tcl_Interp *interp, Tcl_Obj *request, Tcl_Obj *body, cJSON *result) {
    if (!body) return TCL_OK;
    Tcl_Obj *flag;
    int encoded = 0;
    if (Topenapi_Get(interp, request, "isBase64Encoded", &flag) != TCL_OK ||
        (flag && Tcl_GetBooleanFromObj(interp, flag, &encoded) != TCL_OK)) return TCL_ERROR;
    if (!encoded) return add_item(interp, result, "raw_body", Topenapi_String(interp, body));
    Tcl_Size length;
    const char *text = Tcl_GetStringFromObj(body, &length);
    char *decoded = malloc((size_t)length / 4 * 3 + 1);
    if (!decoded) return Topenapi_Error(interp, "MEMORY", "unable to allocate request body");
    size_t size;
    int status = base64(interp, text, (size_t)length, decoded, &size);
    if (status == TCL_OK) {
        decoded[size] = '\0';
        if (memchr(decoded, '\0', size))
            status = Topenapi_Error(interp, "NUL", "decoded text body must not contain NUL bytes");
        else status = add_item(interp, result, "raw_body", cJSON_CreateString(decoded));
    }
    free(decoded);
    return status;
}

int Topenapi_FromTwebserver(Tcl_Interp *interp, Tcl_Obj *request,
                          Tcl_Obj *parsed_form, cJSON **out) {
    if (!out) return Topenapi_Error(interp, "ARGUMENT", "output pointer is required");
    *out = NULL;
    Tcl_Size unused;
    if (!request || Tcl_DictObjSize(interp, request, &unused) != TCL_OK) return TCL_ERROR;
    cJSON *result = cJSON_CreateObject();
    if (!result) return Topenapi_Error(interp, "MEMORY", "unable to allocate request");
    const char *names[] = {"params", "query", "headers", "cookies", "form", "files", "context"};
    cJSON *parts[7];
    for (int i = 0; i < 7; ++i) {
        parts[i] = cJSON_CreateObject();
        if (add_item(interp, result, names[i], parts[i]) != TCL_OK) goto failure;
    }
    cJSON *params = parts[0], *query = parts[1], *headers = parts[2], *cookies = parts[3];
    cJSON *form = parts[4], *files = parts[5], *context = parts[6];
    Tcl_Obj *method, *body;
    if (Topenapi_Get(interp, request, "httpMethod", &method) != TCL_OK ||
        Topenapi_Get(interp, request, "body", &body) != TCL_OK) goto failure;
    if (method && add_item(interp, result, "method", Topenapi_String(interp, method)) != TCL_OK) goto failure;
    if (source_fields(interp, request, "pathParameters", NULL, params, 0) != TCL_OK ||
        source_fields(interp, request, "queryStringParameters", "multiValueQueryStringParameters", query, 0) != TCL_OK ||
        source_fields(interp, request, "headers", "multiValueHeaders", headers, 1) != TCL_OK ||
        source_fields(interp, request, "cookies", NULL, cookies, 0) != TCL_OK) goto failure;
    cJSON *cookie = cJSON_GetObjectItemCaseSensitive(headers, "cookie");
    if (cookie) {
        cJSON *entry = cJSON_IsArray(cookie) ? cookie->child : cookie;
        for (; entry; entry = cJSON_IsArray(cookie) ? entry->next : NULL)
            if (cookie_header(interp, cookies, entry->valuestring) != TCL_OK) goto failure;
    }
    cJSON *content_type = cJSON_GetObjectItemCaseSensitive(headers, "content-type");
    if (content_type && !cJSON_IsString(content_type)) {
        Topenapi_Error(interp, "REQUEST", "Content-Type must have exactly one value");
        goto failure;
    }
    char *type = NULL;
    if (content_type) {
        cJSON *copy = cJSON_Duplicate(content_type, 1);
        if (add_item(interp, context, "content_type", copy) != TCL_OK) goto failure;
        type = copy->valuestring;
        lowercase(type);
        char *semicolon = strchr(type, ';');
        if (semicolon) *semicolon = '\0';
        size_t n = strlen(type);
        while (n && (type[n - 1] == ' ' || type[n - 1] == '\t')) type[--n] = '\0';
        while (*type == ' ' || *type == '\t') ++type;
    }
    int is_form = type && (!strcmp(type, "multipart/form-data") ||
                          !strcmp(type, "application/x-www-form-urlencoded"));
    if (is_form) {
        Tcl_Size body_length = 0;
        if (body) Tcl_GetStringFromObj(body, &body_length);
        if (body_length && !parsed_form) {
            Topenapi_Error(interp, "FORM", "pass the result of twebserver::get_form as parsedFormDict");
            goto failure;
        }
        if (!cJSON_AddBoolToObject(result, "body_present", body_length > 0)) goto memory_failure;
        if (source_fields(interp, parsed_form, "fields", "multiValueFields", form, 0) != TCL_OK ||
            extract_uploads(interp, parsed_form, form, files) != TCL_OK) goto failure;
    } else if (parsed_form) {
        Topenapi_Error(interp, "FORM", "parsedFormDict requires a form Content-Type");
        goto failure;
    } else if (add_body(interp, request, body, result) != TCL_OK) goto failure;
    *out = result;
    return TCL_OK;

memory_failure:
    Topenapi_Error(interp, "MEMORY", "unable to allocate request metadata");
failure:
    cJSON_Delete(result);
    return TCL_ERROR;
}
