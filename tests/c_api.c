#include "topenapi.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    cJSON *value = cJSON_Parse("4");
    cJSON *schema = cJSON_Parse("{\"type\":\"integer\",\"minimum\":3}");
    char error[128] = "stale";
    if (!topenapi_validate_schema(value, schema, error, sizeof(error)) || error[0]) return 1;
    cJSON_SetNumberValue(value, 2);
    if (topenapi_validate_schema(value, schema, error, sizeof(error)) || !error[0]) return 2;
    char small[2] = {0, 'x'};
    if (topenapi_validate_schema(NULL, schema, small, sizeof(small)) || small[1]) return 3;
    if (topenapi_validate_request(NULL, schema, NULL, 0)) return 4;
    cJSON_Delete(value);
    cJSON_Delete(schema);
    cJSON *request = cJSON_Parse("{\"query\":{\"count\":\"007\"}}");
    cJSON *operation = cJSON_Parse("{\"parameters\":[{\"in\":\"query\",\"name\":\"count\",\"schema\":{\"type\":\"integer\"}}]}");
    cJSON *prepared = NULL;
    if (!topenapi_prepare_request(request, operation, &prepared, error, sizeof(error)) || error[0]) return 5;
    cJSON *count = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(prepared, "query"), "count");
    if (!cJSON_IsNumber(count) || count->valuedouble != 7) return 6;
    cJSON *original = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(request, "query"), "count");
    if (!cJSON_IsString(original) || strcmp(original->valuestring, "007")) return 7;
    cJSON_Delete(prepared);
    prepared = request; // Failure must clear, not free, this borrowed pointer.
    if (topenapi_prepare_request(NULL, operation, &prepared, error, sizeof(error)) || prepared || !error[0]) return 8;
    if (topenapi_prepare_request(request, operation, NULL, NULL, 0)) return 9;
    cJSON_Delete(request);
    cJSON_Delete(operation);
    puts("C API passed");
    return 0;
}
