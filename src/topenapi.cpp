#include "topenapi.h"
#include "openapi_request_validator.h"
#include <cstdio>
#include <exception>

static int validate(const cJSON *value, const cJSON *schema, char *error,
                    size_t capacity, bool request) noexcept {
    if (error && capacity) error[0] = '\0';
    try {
        std::string message;
        bool valid = request
            ? topenapi::validate_request(const_cast<cJSON *>(value), const_cast<cJSON *>(schema), message)
            : topenapi::validate_schema(const_cast<cJSON *>(value), const_cast<cJSON *>(schema), message);
        if (error && capacity) std::snprintf(error, capacity, "%s", message.c_str());
        return valid ? 1 : 0;
    } catch (const std::exception &e) {
        if (error && capacity) std::snprintf(error, capacity, "validation failed: %s", e.what());
    } catch (...) {
        if (error && capacity) std::snprintf(error, capacity, "validation failed");
    }
    return 0;
}

extern "C" int topenapi_validate_schema(const cJSON *value, const cJSON *schema,
                                      char *error, size_t capacity) {
    return validate(value, schema, error, capacity, false);
}
extern "C" int topenapi_validate_request(const cJSON *request, const cJSON *operation,
                                       char *error, size_t capacity) {
    return validate(request, operation, error, capacity, true);
}
