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

extern "C" int topenapi_prepare_request(const cJSON *request, const cJSON *operation,
                                       cJSON **output, char *error, size_t capacity) {
    if (error && capacity) error[0] = '\0';
    if (!output) {
        if (error && capacity) std::snprintf(error, capacity, "output pointer is required");
        return 0;
    }
    *output = nullptr;
    try {
        std::string message;
        *output = topenapi::prepare_request(const_cast<cJSON *>(request), const_cast<cJSON *>(operation), message);
        if (error && capacity) std::snprintf(error, capacity, "%s", message.c_str());
        return *output != nullptr;
    } catch (const std::exception &e) {
        if (error && capacity) std::snprintf(error, capacity, "binding preparation failed: %s", e.what());
    } catch (...) {
        if (error && capacity) std::snprintf(error, capacity, "binding preparation failed");
    }
    return 0;
}
