#include "openapi_request_validator.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

JsonPtr parse_json(const char *json) {
    return JsonPtr(cJSON_Parse(json), cJSON_Delete);
}

void require_true(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

bool validate(const char *request_json, const char *schema_json, std::string &error_out) {
    JsonPtr request = parse_json(request_json);
    JsonPtr schema = parse_json(schema_json);
    require_true(request != nullptr, "request fixture must parse");
    require_true(schema != nullptr, "schema fixture must parse");
    return topenapi::validate_request(request.get(), schema.get(), error_out);
}

bool validate(const char *request_json, const char *schema_json) {
    std::string error;
    return validate(request_json, schema_json, error);
}

bool validate_value(const char *value_json, const char *schema_json, std::string &error_out) {
    JsonPtr value = parse_json(value_json);
    JsonPtr schema = parse_json(schema_json);
    require_true(value != nullptr, "value fixture must parse");
    require_true(schema != nullptr, "schema fixture must parse");
    return topenapi::validate_schema(value.get(), schema.get(), error_out);
}

bool validate_value(const char *value_json, const char *schema_json) {
    std::string error;
    return validate_value(value_json, schema_json, error);
}

void test_required_path_and_query_parameters() {
    const char *request = R"json({
        "params": {"item_id": "42"},
        "query": {"page": "3", "mode": "compact"},
        "form": {},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "parameters": [
            {"name":"item_id","in":"path","required":true,"schema":{"type":"integer","minimum":1}},
            {"name":"page","in":"query","schema":{"type":"integer","minimum":1,"maximum":10}},
            {"name":"mode","in":"query","schema":{"type":"string","enum":["compact","full"]}}
        ]
    })json";
    require_true(validate(request, schema), "valid path and query parameters should pass");
}

void test_missing_required_parameter_fails() {
    const char *request = R"json({"params":{},"query":{},"form":{},"raw_body":""})json";
    const char *schema = R"json({
        "parameters": [
            {"name":"item_id","in":"path","required":true,"schema":{"type":"string"}}
        ]
    })json";
    std::string error;
    require_true(!validate(request, schema, error), "missing required parameter should fail");
    require_true(error.find("item_id") != std::string::npos, "missing parameter error should name the field");
}

void test_get_request_ignores_request_body_fragment() {
    const char *request = R"json({
        "method": "GET",
        "params": {"class_id": "cls_3ElBpazUqq3z6TxUrardxUwnAW8"},
        "query": {},
        "form": {},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "parameters": [
            {"name":"class_id","in":"path","required":true,"schema":{"type":"string"}}
        ],
        "requestBody": false
    })json";
    require_true(validate(request, schema), "GET request should ignore requestBody fragments");
}

void test_json_request_body() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "context": {"content_type": "application/json; charset=utf-8"},
        "raw_body": "{\"name\":\"Nova\",\"count\":2}"
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "application/json": {
                    "schema": {
                        "type": "object",
                        "required": ["name", "count"],
                        "properties": {
                            "name": {"type":"string","minLength":2},
                            "count": {"type":"integer","minimum":1}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    require_true(validate(request, schema), "valid JSON request body should pass");
}

void test_invalid_json_request_body_fails() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "context": {"content_type": "application/json"},
        "raw_body": "{\"name\":\"Nova\",\"extra\":true}"
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "application/json": {
                    "schema": {
                        "type": "object",
                        "required": ["name"],
                        "properties": {"name": {"type":"string"}},
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    require_true(!validate(request, schema), "JSON body with unsupported property should fail");
}

void test_form_request_body_coerces_scalars() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {"email":"a@example.test","quantity":"4","gift":"false"},
        "context": {"content_type": "application/x-www-form-urlencoded"},
        "raw_body": "email=a%40example.test&quantity=4&gift=false"
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "application/x-www-form-urlencoded": {
                    "schema": {
                        "type": "object",
                        "required": ["email", "quantity", "gift"],
                        "properties": {
                            "email": {"type":"string","minLength":3},
                            "quantity": {"type":"integer","minimum":1},
                            "gift": {"type":"boolean"}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    require_true(validate(request, schema), "form request body should coerce scalar strings");
}

void test_multipart_required_file_can_come_from_files() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {"title":"Hero"},
        "files": {
            "image": [
                {
                    "filename": "hero.png",
                    "content_type": "image/png",
                    "size": 3,
                    "encoding": "base64"
                }
            ]
        },
        "context": {"content_type": "multipart/form-data; boundary=test"},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "multipart/form-data": {
                    "schema": {
                        "type": "object",
                        "required": ["title", "image"],
                        "properties": {
                            "title": {"type":"string","minLength":1},
                            "image": {"type":"object","required":["filename","size"],"properties":{"size":{"type":"integer","minimum":1}}}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    require_true(validate(request, schema), "multipart required file should validate from request.files");
}

void test_multipart_missing_required_file_fails() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {"title":"Hero"},
        "files": {},
        "context": {"content_type": "multipart/form-data"},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "multipart/form-data": {
                    "schema": {
                        "type": "object",
                        "required": ["title", "image"],
                        "properties": {
                            "title": {"type":"string"},
                            "image": {"type":"object"}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    std::string error;
    require_true(!validate(request, schema, error), "multipart missing required file should fail");
    require_true(error.find("image") != std::string::npos, "missing file error should name the field");
}

void test_multipart_empty_upload_fails() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "files": {
            "image": [
                {
                    "filename": "empty.png",
                    "content_type": "image/png",
                    "size": 0,
                    "encoding": "base64"
                }
            ]
        },
        "context": {"content_type": "multipart/form-data"},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "multipart/form-data": {
                    "schema": {
                        "type": "object",
                        "required": ["image"],
                        "properties": {
                            "image": {"type":"object"}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    std::string error;
    require_true(!validate(request, schema, error), "multipart empty upload should fail");
    require_true(error.find("non-empty") != std::string::npos, "empty upload error should be explicit");
}

void test_multipart_multiple_files_require_array_schema() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "files": {
            "gallery": [
                {
                    "filename": "a.png",
                    "content_type": "image/png",
                    "size": 3,
                    "encoding": "base64"
                },
                {
                    "filename": "b.png",
                    "content_type": "image/png",
                    "size": 3,
                    "encoding": "base64"
                }
            ]
        },
        "context": {"content_type": "multipart/form-data"},
        "raw_body": ""
    })json";
    const char *array_schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "multipart/form-data": {
                    "schema": {
                        "type": "object",
                        "required": ["gallery"],
                        "properties": {
                            "gallery": {
                                "type":"array",
                                "minItems":2,
                                "maxItems":2,
                                "items":{"type":"object","required":["filename","size"],"properties":{"size":{"type":"integer","minimum":1}}}
                            }
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    const char *single_schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "multipart/form-data": {
                    "schema": {
                        "type": "object",
                        "required": ["gallery"],
                        "properties": {
                            "gallery": {"type":"object"}
                        },
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    require_true(validate(request, array_schema), "multipart multiple files should validate with array schema");
    std::string error;
    require_true(!validate(request, single_schema, error),
                 "multipart multiple files should fail with single-file schema");
    require_true(error.find("multiple files") != std::string::npos,
                 "multiple file error should describe the schema mismatch");
}

void test_request_body_content_type_mismatch_fails() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "context": {"content_type": "text/plain"},
        "raw_body": "{\"name\":\"Nova\"}"
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "application/json": {
                    "schema": {
                        "type": "object",
                        "required": ["name"],
                        "properties": {"name": {"type":"string"}},
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    std::string error;
    require_true(!validate(request, schema, error), "request body content type mismatch should fail");
    require_true(error.find("text/plain") != std::string::npos, "content type error should name actual type");
}

void test_request_body_missing_content_type_fails() {
    const char *request = R"json({
        "params": {},
        "query": {},
        "form": {},
        "raw_body": "{\"name\":\"Nova\"}"
    })json";
    const char *schema = R"json({
        "requestBody": {
            "required": true,
            "content": {
                "application/json": {
                    "schema": {
                        "type": "object",
                        "required": ["name"],
                        "properties": {"name": {"type":"string"}},
                        "additionalProperties": false
                    }
                }
            }
        }
    })json";
    std::string error;
    require_true(!validate(request, schema, error), "request body without content type should fail");
    require_true(error.find("content_type") != std::string::npos, "missing content type error should be explicit");
}

void test_array_query_parameter_from_comma_list() {
    const char *request = R"json({
        "params": {},
        "query": {"tags":"new,sale"},
        "form": {},
        "raw_body": ""
    })json";
    const char *schema = R"json({
        "parameters": [
            {"name":"tags","in":"query","schema":{"type":"array","minItems":2,"items":{"type":"string","enum":["new","sale","clearance"]}}}
        ]
    })json";
    require_true(validate(request, schema), "comma-separated query array should validate");
}

void test_inline_refs_are_rejected() {
    const char *request = R"json({"params":{},"query":{"id":"1"},"form":{},"raw_body":""})json";
    const char *schema = R"json({
        "parameters": [
            {"name":"id","in":"query","schema":{"$ref":"#/components/schemas/Id"}}
        ]
    })json";
    require_true(!validate(request, schema), "schema $ref should fail");
}

void test_direct_schema_validation() {
    const char *schema = R"json({
        "type": "object",
        "required": ["name", "price"],
        "properties": {
            "name": {"type": "string", "minLength": 2},
            "price": {"type": "integer", "minimum": 1}
        },
        "additionalProperties": false
    })json";
    require_true(validate_value(R"json({"name":"Desk","price":120})json", schema),
                 "direct schema validation should accept valid values");
    std::string error;
    require_true(!validate_value(R"json({"name":"D","price":0})json", schema, error),
                 "direct schema validation should reject invalid values");
    require_true(!error.empty(), "direct schema validation should provide errors");
}

void test_optional_null_object_property_is_treated_as_missing() {
    const char *schema = R"json({
        "type": "object",
        "required": ["name"],
        "properties": {
            "name": {"type": "string", "minLength": 1},
            "lot_size": {"type": "number", "minimum": 0}
        },
        "additionalProperties": false,
        "maxProperties": 1
    })json";

    require_true(validate_value(R"json({"name":"Desk","lot_size":null})json", schema),
                 "optional null object properties should validate as missing");
    require_true(!validate_value(R"json({"name":null})json", schema),
                 "required null object properties should still fail");
    require_true(!validate_value(R"json({"name":"Desk","unknown":null})json", schema),
                 "undeclared null object properties should still fail");
}

}  // namespace

int main() {
    test_required_path_and_query_parameters();
    test_missing_required_parameter_fails();
    test_get_request_ignores_request_body_fragment();
    test_json_request_body();
    test_invalid_json_request_body_fails();
    test_form_request_body_coerces_scalars();
    test_multipart_required_file_can_come_from_files();
    test_multipart_missing_required_file_fails();
    test_multipart_empty_upload_fails();
    test_multipart_multiple_files_require_array_schema();
    test_request_body_content_type_mismatch_fails();
    test_request_body_missing_content_type_fails();
    test_array_query_parameter_from_comma_list();
    test_inline_refs_are_rejected();
    test_direct_schema_validation();
    test_optional_null_object_property_is_treated_as_missing();
    return 0;
}
