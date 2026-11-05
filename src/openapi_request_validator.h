#pragma once

extern "C" {
#include <cJSON.h>
}

#include <string>

namespace topenapi {

bool validate_schema(cJSON *value, cJSON *schema);

bool validate_schema(cJSON *value, cJSON *schema, std::string &error_out);

bool validate_request(cJSON *request, cJSON *operation_schema);

bool validate_request(cJSON *request, cJSON *operation_schema, std::string &error_out);

cJSON *prepare_request(cJSON *request, cJSON *operation_schema, std::string &error_out);

}  // namespace topenapi
