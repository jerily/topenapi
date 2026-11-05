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
    puts("C API passed");
    return 0;
}
