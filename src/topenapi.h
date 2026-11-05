#ifndef TOPENAPI_H
#define TOPENAPI_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
struct cJSON;

/* Return 1 on success, 0 on validation failure. Inputs are borrowed and never
 * modified. Optional error storage is always NUL terminated when capacity > 0.
 * No C++ exception crosses this interface. Link the C++ runtime and cJSON. */
int topenapi_validate_schema(const struct cJSON *value, const struct cJSON *schema,
                            char *error, size_t capacity);
int topenapi_validate_request(const struct cJSON *request, const struct cJSON *operation,
                             char *error, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
