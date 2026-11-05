#include "openapi_request_validator.h"
#include "string_utils.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace topenapi {
namespace {

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

struct ValidationContext {
    cJSON *prepared = nullptr;
    bool coerce_strings = false;
    int depth = 0;
    std::string *error = nullptr;
    const std::set<const cJSON *> *uploads = nullptr;
};

void set_error(ValidationContext &context, const std::string &message) {
    if (context.error != nullptr && context.error->empty()) {
        *context.error = message;
    }
}

bool has_ref(cJSON *object) {
    return cJSON_IsObject(object) &&
           cJSON_GetObjectItemCaseSensitive(object, "$ref") != nullptr;
}

bool json_bool(cJSON *object, const char *key, bool fallback = false) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsTrue(item)) {
        return true;
    }
    if (cJSON_IsFalse(item)) {
        return false;
    }
    return fallback;
}

std::string json_string(cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool required_property_names_contains(cJSON *required, const char *property_name) {
    if (!cJSON_IsArray(required) || property_name == nullptr) {
        return false;
    }
    cJSON *name = nullptr;
    cJSON_ArrayForEach(name, required) {
        if (cJSON_IsString(name) && name->valuestring != nullptr &&
            std::strcmp(name->valuestring, property_name) == 0) {
            return true;
        }
    }
    return false;
}

bool should_treat_object_property_as_missing(cJSON *property, cJSON *properties, cJSON *required) {
    if (!cJSON_IsNull(property) || property->string == nullptr || !cJSON_IsObject(properties)) {
        return false;
    }
    cJSON *property_schema = cJSON_GetObjectItemCaseSensitive(properties, property->string);
    return property_schema != nullptr && !required_property_names_contains(required, property->string);
}

size_t effective_object_property_count(cJSON *value, cJSON *properties, cJSON *required) {
    size_t count = 0;
    cJSON *property = nullptr;
    cJSON_ArrayForEach(property, value) {
        if (!should_treat_object_property_as_missing(property, properties, required)) {
            ++count;
        }
    }
    return count;
}

bool parse_number_text(const std::string &text, double &number_out) {
    const std::string trimmed = trim_ascii_copy(text);
    if (trimmed.empty()) {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(trimmed.c_str(), &end);
    if (errno == ERANGE || end == trimmed.c_str() || end == nullptr || *end != '\0' ||
        !std::isfinite(value)) {
        return false;
    }
    number_out = value;
    return true;
}

bool value_as_number(cJSON *value, const ValidationContext &context, double &number_out) {
    if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
        number_out = value->valuedouble;
        return true;
    }
    if (context.coerce_strings && cJSON_IsString(value) && value->valuestring != nullptr) {
        return parse_number_text(value->valuestring, number_out);
    }
    return false;
}

bool value_as_bool(cJSON *value, const ValidationContext &context, bool &bool_out) {
    if (cJSON_IsTrue(value)) {
        bool_out = true;
        return true;
    }
    if (cJSON_IsFalse(value)) {
        bool_out = false;
        return true;
    }
    if (context.coerce_strings && cJSON_IsString(value) && value->valuestring != nullptr) {
        const std::string normalized = lowercase_ascii_copy(trim_ascii_copy(value->valuestring));
        if (normalized == "true") {
            bool_out = true;
            return true;
        }
        if (normalized == "false") {
            bool_out = false;
            return true;
        }
    }
    return false;
}

std::string json_fingerprint(cJSON *value) {
    char *printed = cJSON_PrintUnformatted(value);
    if (printed == nullptr) {
        return "";
    }
    std::string result = printed;
    cJSON_free(printed);
    return result;
}

bool json_values_equal(cJSON *left, cJSON *right) {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    if (cJSON_IsNull(left) || cJSON_IsNull(right)) {
        return cJSON_IsNull(left) && cJSON_IsNull(right);
    }
    if (cJSON_IsBool(left) || cJSON_IsBool(right)) {
        return (cJSON_IsTrue(left) && cJSON_IsTrue(right)) ||
               (cJSON_IsFalse(left) && cJSON_IsFalse(right));
    }
    if (cJSON_IsNumber(left) || cJSON_IsNumber(right)) {
        return cJSON_IsNumber(left) && cJSON_IsNumber(right) &&
               std::fabs(left->valuedouble - right->valuedouble) < 0.000000001;
    }
    if (cJSON_IsString(left) || cJSON_IsString(right)) {
        return cJSON_IsString(left) && cJSON_IsString(right) &&
               left->valuestring != nullptr && right->valuestring != nullptr &&
               std::strcmp(left->valuestring, right->valuestring) == 0;
    }
    return json_fingerprint(left) == json_fingerprint(right);
}

bool value_matches_enum_item(cJSON *value, cJSON *item, const ValidationContext &context) {
    if (json_values_equal(value, item)) {
        return true;
    }
    if (!context.coerce_strings || !cJSON_IsString(value) || value->valuestring == nullptr) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        double number = 0.0;
        return parse_number_text(value->valuestring, number) &&
               std::fabs(number - item->valuedouble) < 0.000000001;
    }
    if (cJSON_IsBool(item)) {
        bool boolean = false;
        return value_as_bool(value, context, boolean) &&
               ((boolean && cJSON_IsTrue(item)) || (!boolean && cJSON_IsFalse(item)));
    }
    return false;
}

bool validate_schema(cJSON *value, cJSON *schema, ValidationContext context);

bool validate_all_of(cJSON *value, cJSON *items, ValidationContext context) {
    if (items == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(items)) {
        set_error(context, "allOf must be an array");
        return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        if (!validate_schema(value, item, context)) {
            return false;
        }
    }
    return true;
}

bool validate_any_of(cJSON *value, cJSON *items, ValidationContext context) {
    if (items == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(items)) {
        set_error(context, "anyOf must be an array");
        return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        std::string ignored_error;
        ValidationContext nested = context;
        nested.error = &ignored_error;
        if (validate_schema(value, item, nested)) {
            return true;
        }
    }
    set_error(context, "value does not match any anyOf schema");
    return false;
}

bool validate_one_of(cJSON *value, cJSON *items, ValidationContext context) {
    if (items == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(items)) {
        set_error(context, "oneOf must be an array");
        return false;
    }
    int match_count = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, items) {
        std::string ignored_error;
        ValidationContext nested = context;
        nested.error = &ignored_error;
        if (validate_schema(value, item, nested)) {
            ++match_count;
        }
    }
    if (match_count == 1) {
        return true;
    }
    set_error(context, "value does not match exactly one oneOf schema");
    return false;
}

bool type_is_declared(cJSON *schema, const char *type_name) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && type->valuestring != nullptr) {
        return std::strcmp(type->valuestring, type_name) == 0;
    }
    if (cJSON_IsArray(type)) {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, type) {
            if (cJSON_IsString(item) && item->valuestring != nullptr &&
                std::strcmp(item->valuestring, type_name) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool has_declared_type(cJSON *schema) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    return cJSON_IsString(type) || cJSON_IsArray(type);
}

bool value_matches_type(cJSON *value, const std::string &type_name, const ValidationContext &context) {
    if (type_name == "null") {
        return cJSON_IsNull(value);
    }
    if (type_name == "boolean") {
        bool ignored = false;
        return value_as_bool(value, context, ignored);
    }
    if (type_name == "number") {
        double ignored = 0.0;
        return value_as_number(value, context, ignored);
    }
    if (type_name == "integer") {
        double number = 0.0;
        return value_as_number(value, context, number) && std::floor(number) == number;
    }
    if (type_name == "string") {
        return cJSON_IsString(value);
    }
    if (type_name == "array") {
        return cJSON_IsArray(value) ||
               (context.coerce_strings && cJSON_IsString(value) && value->valuestring != nullptr);
    }
    if (type_name == "object") {
        return cJSON_IsObject(value);
    }
    return false;
}

bool validate_declared_type(cJSON *value, cJSON *schema, ValidationContext context) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && type->valuestring != nullptr) {
        if (value_matches_type(value, type->valuestring, context)) {
            return true;
        }
        set_error(context, "value does not match schema type " + std::string(type->valuestring));
        return false;
    }
    if (cJSON_IsArray(type)) {
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, type) {
            if (cJSON_IsString(item) && item->valuestring != nullptr &&
                value_matches_type(value, item->valuestring, context)) {
                return true;
            }
        }
        set_error(context, "value does not match any schema type");
        return false;
    }
    return true;
}

bool validate_enum(cJSON *value, cJSON *schema, ValidationContext context) {
    cJSON *enum_values = cJSON_GetObjectItemCaseSensitive(schema, "enum");
    if (enum_values == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(enum_values)) {
        set_error(context, "enum must be an array");
        return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, enum_values) {
        if (value_matches_enum_item(value, item, context)) {
            return true;
        }
    }
    set_error(context, "value is not in enum");
    return false;
}

bool validate_string_constraints(cJSON *value, cJSON *schema, ValidationContext context) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        return true;
    }
    const std::string text = value->valuestring;
    size_t length = 0;
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++length;
    cJSON *min_length = cJSON_GetObjectItemCaseSensitive(schema, "minLength");
    if (cJSON_IsNumber(min_length) && length < static_cast<size_t>(std::max(0.0, min_length->valuedouble))) {
        set_error(context, "string is shorter than minLength");
        return false;
    }
    cJSON *max_length = cJSON_GetObjectItemCaseSensitive(schema, "maxLength");
    if (cJSON_IsNumber(max_length) && length > static_cast<size_t>(std::max(0.0, max_length->valuedouble))) {
        set_error(context, "string is longer than maxLength");
        return false;
    }
    cJSON *pattern = cJSON_GetObjectItemCaseSensitive(schema, "pattern");
    if (cJSON_IsString(pattern) && pattern->valuestring != nullptr) {
        try {
            if (!std::regex_search(text, std::regex(pattern->valuestring))) {
                set_error(context, "string does not match pattern");
                return false;
            }
        } catch (const std::regex_error &) {
            set_error(context, "pattern is invalid");
            return false;
        }
    }
    return true;
}

bool validate_number_constraints(cJSON *value, cJSON *schema, ValidationContext context) {
    double number = 0.0;
    if (!value_as_number(value, context, number)) {
        return true;
    }
    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(schema, "minimum");
    cJSON *exclusive_minimum = cJSON_GetObjectItemCaseSensitive(schema, "exclusiveMinimum");
    if (cJSON_IsNumber(exclusive_minimum)) {
        if (!(number > exclusive_minimum->valuedouble)) {
            set_error(context, "number is not greater than exclusiveMinimum");
            return false;
        }
    } else if (cJSON_IsNumber(minimum)) {
        const bool exclusive = cJSON_IsTrue(exclusive_minimum);
        if (exclusive ? !(number > minimum->valuedouble) : !(number >= minimum->valuedouble)) {
            set_error(context, exclusive ? "number is not greater than minimum" : "number is less than minimum");
            return false;
        }
    }

    cJSON *maximum = cJSON_GetObjectItemCaseSensitive(schema, "maximum");
    cJSON *exclusive_maximum = cJSON_GetObjectItemCaseSensitive(schema, "exclusiveMaximum");
    if (cJSON_IsNumber(exclusive_maximum)) {
        if (!(number < exclusive_maximum->valuedouble)) {
            set_error(context, "number is not less than exclusiveMaximum");
            return false;
        }
    } else if (cJSON_IsNumber(maximum)) {
        const bool exclusive = cJSON_IsTrue(exclusive_maximum);
        if (exclusive ? !(number < maximum->valuedouble) : !(number <= maximum->valuedouble)) {
            set_error(context, exclusive ? "number is not less than maximum" : "number is greater than maximum");
            return false;
        }
    }

    cJSON *multiple_of = cJSON_GetObjectItemCaseSensitive(schema, "multipleOf");
    if (cJSON_IsNumber(multiple_of) && multiple_of->valuedouble > 0.0) {
        const double remainder = std::fmod(std::fabs(number), multiple_of->valuedouble);
        const double distance = std::min(remainder, std::fabs(multiple_of->valuedouble - remainder));
        if (distance > 0.000000001) {
            set_error(context, "number is not a multipleOf value");
            return false;
        }
    }
    return true;
}

bool validate_array_items(cJSON *value, cJSON *schema, ValidationContext context) {
    cJSON *items_schema = cJSON_GetObjectItemCaseSensitive(schema, "items");
    cJSON *min_items = cJSON_GetObjectItemCaseSensitive(schema, "minItems");
    cJSON *max_items = cJSON_GetObjectItemCaseSensitive(schema, "maxItems");
    cJSON *unique_items = cJSON_GetObjectItemCaseSensitive(schema, "uniqueItems");

    auto validate_count = [&](size_t count) {
        if (cJSON_IsNumber(min_items) && count < static_cast<size_t>(std::max(0.0, min_items->valuedouble))) {
            set_error(context, "array has fewer items than minItems");
            return false;
        }
        if (cJSON_IsNumber(max_items) && count > static_cast<size_t>(std::max(0.0, max_items->valuedouble))) {
            set_error(context, "array has more items than maxItems");
            return false;
        }
        return true;
    };

    if (context.coerce_strings && cJSON_IsString(value) && value->valuestring != nullptr) {
        const std::vector<std::string> parts = split_on_char(value->valuestring, ',');
        if (!validate_count(parts.size())) {
            return false;
        }
        std::set<std::string> seen;
        for (const std::string &part : parts) {
            if (cJSON_IsTrue(unique_items) && !seen.insert(part).second) {
                set_error(context, "array items are not unique");
                return false;
            }
            if (items_schema != nullptr) {
                cJSON *item = cJSON_CreateString(part.c_str());
                if (item == nullptr) {
                    set_error(context, "failed to allocate coerced array item");
                    return false;
                }
                const bool valid = validate_schema(item, items_schema, context);
                cJSON_Delete(item);
                if (!valid) {
                    return false;
                }
            }
        }
        return true;
    }

    if (!cJSON_IsArray(value)) {
        return true;
    }
    const size_t count = static_cast<size_t>(cJSON_GetArraySize(value));
    if (!validate_count(count)) {
        return false;
    }
    std::set<std::string> seen;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, value) {
        if (cJSON_IsTrue(unique_items) && !seen.insert(json_fingerprint(item)).second) {
            set_error(context, "array items are not unique");
            return false;
        }
        if (items_schema != nullptr && !validate_schema(item, items_schema, context)) {
            return false;
        }
    }
    return true;
}

bool validate_object_properties(cJSON *value, cJSON *schema, ValidationContext context) {
    if (!cJSON_IsObject(value)) {
        return true;
    }

    cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (required != nullptr) {
        if (!cJSON_IsArray(required)) {
            set_error(context, "required must be an array");
            return false;
        }
        cJSON *name = nullptr;
        cJSON_ArrayForEach(name, required) {
            if (!cJSON_IsString(name) || name->valuestring == nullptr) {
                set_error(context, "required entries must be strings");
                return false;
            }
            if (cJSON_GetObjectItemCaseSensitive(value, name->valuestring) == nullptr) {
                set_error(context, "object is missing required property " + std::string(name->valuestring));
                return false;
            }
        }
    }

    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (properties != nullptr && !cJSON_IsObject(properties)) {
        set_error(context, "properties must be an object");
        return false;
    }

    cJSON *min_properties = cJSON_GetObjectItemCaseSensitive(schema, "minProperties");
    cJSON *max_properties = cJSON_GetObjectItemCaseSensitive(schema, "maxProperties");
    const size_t property_count = effective_object_property_count(value, properties, required);
    if (cJSON_IsNumber(min_properties) &&
        property_count < static_cast<size_t>(std::max(0.0, min_properties->valuedouble))) {
        set_error(context, "object has fewer properties than minProperties");
        return false;
    }
    if (cJSON_IsNumber(max_properties) &&
        property_count > static_cast<size_t>(std::max(0.0, max_properties->valuedouble))) {
        set_error(context, "object has more properties than maxProperties");
        return false;
    }

    cJSON *additional = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
    const bool additional_false = cJSON_IsFalse(additional);
    const bool additional_schema = cJSON_IsObject(additional);

    cJSON *property = nullptr;
    cJSON_ArrayForEach(property, value) {
        if (property->string == nullptr) {
            set_error(context, "object property is missing a name");
            return false;
        }
        cJSON *property_schema = properties == nullptr
                                         ? nullptr
                                         : cJSON_GetObjectItemCaseSensitive(properties, property->string);
        if (property_schema != nullptr) {
            if (should_treat_object_property_as_missing(property, properties, required)) {
                continue;
            }
            if (!validate_schema(property, property_schema, context)) {
                return false;
            }
            continue;
        }
        if (additional_false) {
            set_error(context, "object has unsupported property " + std::string(property->string));
            return false;
        }
        if (additional_schema && !validate_schema(property, additional, context)) {
            return false;
        }
    }
    return true;
}

bool validate_schema(cJSON *value, cJSON *schema, ValidationContext context) {
    if (context.depth > 64) {
        set_error(context, "schema nesting exceeds validator depth limit");
        return false;
    }
    if (!cJSON_IsObject(schema)) {
        set_error(context, "schema must be an object");
        return false;
    }
    if (has_ref(schema)) {
        set_error(context, "schema references are not supported");
        return false;
    }
    ++context.depth;

    if (cJSON_IsNull(value) && (json_bool(schema, "nullable") || type_is_declared(schema, "null"))) {
        return true;
    }

    if (!validate_enum(value, schema, context)) {
        return false;
    }
    cJSON *const_value = cJSON_GetObjectItemCaseSensitive(schema, "const");
    if (const_value != nullptr && !value_matches_enum_item(value, const_value, context)) {
        set_error(context, "value does not match const");
        return false;
    }

    if (!validate_all_of(value, cJSON_GetObjectItemCaseSensitive(schema, "allOf"), context) ||
        !validate_any_of(value, cJSON_GetObjectItemCaseSensitive(schema, "anyOf"), context) ||
        !validate_one_of(value, cJSON_GetObjectItemCaseSensitive(schema, "oneOf"), context)) {
        return false;
    }
    cJSON *not_schema = cJSON_GetObjectItemCaseSensitive(schema, "not");
    if (not_schema != nullptr) {
        std::string ignored_error;
        ValidationContext nested = context;
        nested.error = &ignored_error;
        if (validate_schema(value, not_schema, nested)) {
            set_error(context, "value matches not schema");
            return false;
        }
    }

    if (context.uploads != nullptr && context.uploads->count(value) &&
        type_is_declared(schema, "string") && json_string(schema, "format") == "binary") {
        if (cJSON_GetObjectItemCaseSensitive(schema, "pattern") != nullptr) {
            set_error(context, "binary file content patterns are unavailable; validate metadata instead");
            return false;
        }
        const double size = cJSON_GetObjectItemCaseSensitive(value, "size")->valuedouble;
        cJSON *minimum = cJSON_GetObjectItemCaseSensitive(schema, "minLength");
        cJSON *maximum = cJSON_GetObjectItemCaseSensitive(schema, "maxLength");
        if ((cJSON_IsNumber(minimum) && size < minimum->valuedouble) ||
            (cJSON_IsNumber(maximum) && size > maximum->valuedouble)) {
            set_error(context, "binary file size is outside schema length bounds");
            return false;
        }
        return true;
    }

    if (has_declared_type(schema)) {
        if (!validate_declared_type(value, schema, context)) {
            return false;
        }
    } else if ((cJSON_GetObjectItemCaseSensitive(schema, "properties") != nullptr ||
                cJSON_GetObjectItemCaseSensitive(schema, "required") != nullptr ||
                cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties") != nullptr) &&
               !cJSON_IsObject(value)) {
        set_error(context, "value does not match inferred object schema");
        return false;
    } else if (cJSON_GetObjectItemCaseSensitive(schema, "items") != nullptr &&
               !value_matches_type(value, "array", context)) {
        set_error(context, "value does not match inferred array schema");
        return false;
    }

    if (!validate_string_constraints(value, schema, context) ||
        !validate_number_constraints(value, schema, context) ||
        !validate_array_items(value, schema, context) ||
        !validate_object_properties(value, schema, context)) {
        return false;
    }
    return true;
}

/* Projection happens only after validation succeeds. It reuses the same schema
 * predicates; it never mutates caller-owned values or guesses Tcl string types. */
using Schemas = std::vector<cJSON *>;

bool applicable_schemas(cJSON *value, cJSON *schema, ValidationContext context,
                        Schemas &result, int depth) {
    if (!schema) return true;
    if (depth > 64) {
        set_error(context, "binding schema nesting exceeds depth limit");
        return false;
    }
    result.push_back(schema);
    cJSON *branch = nullptr;
    cJSON_ArrayForEach(branch, cJSON_GetObjectItemCaseSensitive(schema, "allOf")) {
        if (!applicable_schemas(value, branch, context, result, depth + 1)) return false;
    }
    for (const char *keyword : {"anyOf", "oneOf"}) {
        cJSON *branches = cJSON_GetObjectItemCaseSensitive(schema, keyword);
        if (!branches) continue;
        cJSON *selected = nullptr;
        // Prefer a branch accepting the original JSON type over coercion.
        for (bool coerce : {false, context.coerce_strings}) {
            cJSON_ArrayForEach(branch, branches) {
                std::string ignored;
                ValidationContext probe = context;
                probe.coerce_strings = coerce;
                probe.error = &ignored;
                if (validate_schema(value, branch, probe)) { selected = branch; break; }
            }
            if (selected) break;
        }
        if (selected && !applicable_schemas(value, selected, context, result, depth + 1)) return false;
    }
    return true;
}

std::string projected_type(cJSON *value, cJSON *schema, const ValidationContext &context) {
    cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (type_is_declared(schema, "string")) return "string";
    if (cJSON_IsString(type) && value_matches_type(value, type->valuestring, context)) return type->valuestring;
    cJSON *item = nullptr;
    cJSON *types = cJSON_IsArray(type) ? type : nullptr;
    cJSON_ArrayForEach(item, types) {
        if (cJSON_IsString(item) && value_matches_type(value, item->valuestring, context)) return item->valuestring;
    }
    if (!has_declared_type(schema) && cJSON_GetObjectItemCaseSensitive(schema, "items")) return "array";
    return "";
}

JsonPtr project_value(cJSON *value, const Schemas &roots, ValidationContext context, int depth = 0) {
    if (depth > 64) {
        set_error(context, "binding value nesting exceeds depth limit");
        return JsonPtr(nullptr, cJSON_Delete);
    }
    Schemas schemas;
    for (cJSON *schema : roots) {
        if (!applicable_schemas(value, schema, context, schemas, depth)) return JsonPtr(nullptr, cJSON_Delete);
    }
    std::string type;
    if (context.coerce_strings && cJSON_IsString(value)) {
        for (cJSON *schema : schemas) {
            const std::string candidate = projected_type(value, schema, context);
            if (candidate.empty()) continue;
            if (type.empty() || type == candidate) { type = candidate; continue; }
            if ((type == "integer" && candidate == "number") || (type == "number" && candidate == "integer")) {
                type = "integer";
                continue;
            }
            set_error(context, "binding has conflicting schema conversions");
            return JsonPtr(nullptr, cJSON_Delete);
        }
    }
    JsonPtr converted(nullptr, cJSON_Delete);
    if (type == "integer" || type == "number") {
        double number;
        if (!value_as_number(value, context, number)) return converted;
        converted.reset(cJSON_CreateNumber(number));
    } else if (type == "boolean") {
        bool boolean;
        if (!value_as_bool(value, context, boolean)) return converted;
        converted.reset(cJSON_CreateBool(boolean));
    } else if (type == "array") {
        converted.reset(cJSON_CreateArray());
        if (!converted) throw std::bad_alloc();
        for (const auto &part : split_on_char(value->valuestring, ',')) {
            JsonPtr item(cJSON_CreateString(part.c_str()), cJSON_Delete);
            if (!item || !cJSON_AddItemToArray(converted.get(), item.get())) throw std::bad_alloc();
            item.release();
        }
    }
    if (converted) value = converted.get();
    else if (type == "integer" || type == "number" || type == "boolean") throw std::bad_alloc();

    if (cJSON_IsObject(value)) {
        JsonPtr result(cJSON_CreateObject(), cJSON_Delete);
        if (!result) throw std::bad_alloc();
        cJSON *property = nullptr;
        cJSON_ArrayForEach(property, value) {
            Schemas children;
            bool optional_null = false, required = false;
            for (cJSON *schema : schemas) {
                cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
                cJSON *required_names = cJSON_GetObjectItemCaseSensitive(schema, "required");
                required = required || required_property_names_contains(required_names, property->string);
                optional_null = optional_null || should_treat_object_property_as_missing(property, properties, required_names);
                cJSON *child = cJSON_GetObjectItemCaseSensitive(properties, property->string);
                if (!child) child = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
                if (cJSON_IsObject(child)) children.push_back(child);
            }
            if (optional_null && !required) continue;
            auto child = project_value(property, children, context, depth + 1);
            if (!child) return JsonPtr(nullptr, cJSON_Delete);
            if (!cJSON_AddItemToObject(result.get(), property->string, child.get())) throw std::bad_alloc();
            child.release();
        }
        return result;
    }
    if (cJSON_IsArray(value)) {
        Schemas children;
        for (cJSON *schema : schemas) {
            cJSON *items = cJSON_GetObjectItemCaseSensitive(schema, "items");
            if (items) children.push_back(items);
        }
        JsonPtr result(cJSON_CreateArray(), cJSON_Delete);
        if (!result) throw std::bad_alloc();
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, value) {
            auto child = project_value(item, children, context, depth + 1);
            if (!child) return JsonPtr(nullptr, cJSON_Delete);
            if (!cJSON_AddItemToArray(result.get(), child.get())) throw std::bad_alloc();
            child.release();
        }
        return result;
    }
    JsonPtr result(cJSON_Duplicate(value, 0), cJSON_Delete);
    if (!result) throw std::bad_alloc();
    return result;
}

bool save_projection(cJSON *target, const char *key, JsonPtr value) {
    if (!value) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(target, key);
    if (!cJSON_AddItemToObject(target, key, value.get())) throw std::bad_alloc();
    value.release();
    return true;
}

bool project_body(cJSON *body, cJSON *media, ValidationContext context) {
    if (!context.prepared) return true;
    return save_projection(context.prepared, "body",
                           project_value(body, {cJSON_GetObjectItemCaseSensitive(media, "schema")}, context));
}

bool object_has_children(cJSON *object) {
    return cJSON_IsObject(object) && object->child != nullptr;
}

cJSON *case_insensitive_object_item(cJSON *object, const std::string &name) {
    if (!cJSON_IsObject(object)) {
        return nullptr;
    }
    const std::string wanted = lowercase_ascii_copy(name);
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, object) {
        if (item->string != nullptr && lowercase_ascii_copy(item->string) == wanted) {
            return item;
        }
    }
    return nullptr;
}

cJSON *parameter_location_object(cJSON *request, const std::string &location) {
    if (location == "path") {
        return cJSON_GetObjectItemCaseSensitive(request, "params");
    }
    if (location == "query") {
        return cJSON_GetObjectItemCaseSensitive(request, "query");
    }
    if (location == "header") {
        return cJSON_GetObjectItemCaseSensitive(request, "headers");
    }
    if (location == "cookie") {
        return cJSON_GetObjectItemCaseSensitive(request, "cookies");
    }
    return nullptr;
}

cJSON *parameter_value(cJSON *request, const std::string &location, const std::string &name) {
    cJSON *source = parameter_location_object(request, location);
    if (location == "header") {
        return case_insensitive_object_item(source, name);
    }
    return cJSON_IsObject(source) ? cJSON_GetObjectItemCaseSensitive(source, name.c_str()) : nullptr;
}

cJSON *parse_complete_json(const std::string &text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\') {
            if (text.compare(i + 1, 5, "u0000") == 0) return nullptr;
            if (i + 1 < text.size()) ++i;
        }
    }
    return cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, 1);
}

bool validate_parameter_content(cJSON *value, cJSON *content, ValidationContext context, JsonPtr &projected) {
    if (!cJSON_IsObject(content)) {
        set_error(context, "parameter content must be an object");
        return false;
    }
    if (cJSON_GetArraySize(content) != 1) {
        set_error(context, "parameter content must contain exactly one media type");
        return false;
    }
    cJSON *media = content->child;
    if (!cJSON_IsObject(media)) {
        set_error(context, "parameter media type object must be an object");
        return false;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(media, "schema");
    if (schema == nullptr && !context.prepared) return true;
    const std::string media_type = media->string == nullptr ? "" : lowercase_ascii_copy(media->string);
    if ((media_type == "application/json" ||
         (media_type.size() > 5 && media_type.compare(media_type.size() - 5, 5, "+json") == 0)) &&
        cJSON_IsString(value) && value->valuestring != nullptr) {
        JsonPtr parsed(parse_complete_json(value->valuestring), cJSON_Delete);
        if (parsed == nullptr) {
            set_error(context, "parameter JSON content is invalid");
            return false;
        }
        ValidationContext nested = context;
        nested.coerce_strings = false;
        if (schema && !validate_schema(parsed.get(), schema, nested)) return false;
        if (context.prepared) projected = project_value(parsed.get(), {schema}, nested);
        return !context.prepared || projected != nullptr;
    }
    if (schema && !validate_schema(value, schema, context)) return false;
    if (context.prepared) projected = project_value(value, {schema}, context);
    return !context.prepared || projected != nullptr;
}

bool validate_parameter(cJSON *request, cJSON *parameter, ValidationContext context) {
    if (!cJSON_IsObject(parameter)) {
        set_error(context, "parameter must be an object");
        return false;
    }
    if (has_ref(parameter)) {
        set_error(context, "parameter references are not supported");
        return false;
    }
    const std::string name = json_string(parameter, "name");
    const std::string location = json_string(parameter, "in");
    if (name.empty() || location.empty()) {
        set_error(context, "parameter requires name and in");
        return false;
    }
    if (location != "path" && location != "query" && location != "header" && location != "cookie") {
        set_error(context, "parameter location is not supported: " + location);
        return false;
    }

    cJSON *value = parameter_value(request, location, name);
    const bool required = location == "path" || json_bool(parameter, "required");
    if (value == nullptr) {
        if (required) {
            set_error(context, "request is missing required parameter " + name);
            return false;
        }
        return true;
    }
    if (!json_bool(parameter, "allowEmptyValue", true) &&
        cJSON_IsString(value) && value->valuestring != nullptr && value->valuestring[0] == '\0') {
        set_error(context, "parameter " + name + " cannot be empty");
        return false;
    }

    ValidationContext parameter_context = context;
    parameter_context.coerce_strings = true;
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(parameter, "schema");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(parameter, "content");
    JsonPtr projected(nullptr, cJSON_Delete);
    if (schema) {
        if (!validate_schema(value, schema, parameter_context)) return false;
        if (context.prepared) projected = project_value(value, {schema}, parameter_context);
    } else if (content) {
        if (!validate_parameter_content(value, content, parameter_context, projected)) return false;
    } else if (context.prepared) {
        projected = project_value(value, {}, parameter_context);
    }
    if (!context.prepared) return true;
    cJSON *target = parameter_location_object(context.prepared, location);
    const std::string key = location == "header" ? lowercase_ascii_copy(name) : name;
    return save_projection(target, key.c_str(), std::move(projected));
}

bool media_type_is_json(const std::string &media_type) {
    const std::string value = lowercase_ascii_copy(media_type);
    return value == "application/json" ||
           (value.size() > 5 && value.compare(value.size() - 5, 5, "+json") == 0);
}

std::string normalize_content_type(const std::string &content_type) {
    const size_t parameters = content_type.find(';');
    return lowercase_ascii_copy(trim_ascii_copy(content_type.substr(0, parameters)));
}

bool media_type_is_urlencoded_form(const std::string &media_type) {
    return lowercase_ascii_copy(media_type) == "application/x-www-form-urlencoded";
}

bool media_type_is_multipart_form(const std::string &media_type) {
    return lowercase_ascii_copy(media_type) == "multipart/form-data";
}

bool media_type_is_text(const std::string &media_type) {
    return lowercase_ascii_copy(media_type) == "text/plain";
}

bool validate_media_schema(cJSON *media_object, cJSON *body, ValidationContext context) {
    if (!cJSON_IsObject(media_object)) {
        set_error(context, "requestBody media type object must be an object");
        return false;
    }
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(media_object, "schema");
    return schema == nullptr || validate_schema(body, schema, context);
}

bool validate_json_body(cJSON *media_object, const std::string &raw_body, ValidationContext context) {
    JsonPtr body(parse_complete_json(raw_body), cJSON_Delete);
    if (body == nullptr) {
        set_error(context, "request JSON body is invalid");
        return false;
    }
    ValidationContext json_context = context;
    json_context.coerce_strings = false;
    return validate_media_schema(media_object, body.get(), json_context) &&
           project_body(body.get(), media_object, json_context);
}

bool validate_text_body(cJSON *media_object, const std::string &raw_body, ValidationContext context) {
    JsonPtr body(cJSON_CreateString(raw_body.c_str()), cJSON_Delete);
    if (body == nullptr) {
        set_error(context, "failed to allocate text request body");
        return false;
    }
    ValidationContext text_context = context;
    text_context.coerce_strings = false;
    return validate_media_schema(media_object, body.get(), text_context) &&
           project_body(body.get(), media_object, text_context);
}

cJSON *media_schema(cJSON *media_object) {
    return cJSON_IsObject(media_object) ? cJSON_GetObjectItemCaseSensitive(media_object, "schema") : nullptr;
}

cJSON *direct_object_property_schema(cJSON *schema, const char *property_name) {
    if (!cJSON_IsObject(schema) || property_name == nullptr) {
        return nullptr;
    }
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    return cJSON_IsObject(properties) ? cJSON_GetObjectItemCaseSensitive(properties, property_name) : nullptr;
}

bool schema_expects_array(cJSON *schema) {
    return cJSON_IsObject(schema) &&
           (type_is_declared(schema, "array") ||
            (!has_declared_type(schema) && cJSON_GetObjectItemCaseSensitive(schema, "items") != nullptr));
}

bool value_matches_schema_without_reporting(cJSON *value, cJSON *schema, ValidationContext context) {
    if (schema == nullptr) {
        return true;
    }
    std::string ignored_error;
    ValidationContext nested = context;
    nested.error = &ignored_error;
    return validate_schema(value, schema, nested);
}

bool multipart_file_entry_is_present(cJSON *file, const std::string &field_name, ValidationContext context) {
    if (!cJSON_IsObject(file)) {
        set_error(context, "multipart file field " + field_name + " entries must be objects");
        return false;
    }
    const std::string filename = json_string(file, "filename");
    if (filename.empty()) {
        set_error(context, "multipart file field " + field_name + " has an empty filename");
        return false;
    }
    const std::string encoding = json_string(file, "encoding");
    if (encoding != "base64") {
        set_error(context, "multipart file field " + field_name + " must use base64 encoding");
        return false;
    }
    cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
    if (!cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || size->valuedouble <= 0.0 || std::floor(size->valuedouble) != size->valuedouble) {
        set_error(context, "multipart file field " + field_name + " must contain non-empty uploads");
        return false;
    }

    return true;
}

cJSON *multipart_file_metadata(cJSON *file, ValidationContext context) {
    cJSON *value = cJSON_Duplicate(file, 1);
    if (value == nullptr) {
        set_error(context, "failed to allocate multipart file metadata");
    }
    return value;
}

cJSON *multipart_file_metadata_array(cJSON *files_array, const std::string &field_name, ValidationContext context) {
    cJSON *values = cJSON_CreateArray();
    if (values == nullptr) {
        set_error(context, "failed to allocate multipart file validation array");
        return nullptr;
    }

    cJSON *file = nullptr;
    cJSON_ArrayForEach(file, files_array) {
        cJSON *value = multipart_file_metadata(file, context);
        if (value == nullptr) {
            cJSON_Delete(values);
            return nullptr;
        }
        cJSON_AddItemToArray(values, value);
    }

    if (cJSON_GetArraySize(values) == 0) {
        cJSON_Delete(values);
        set_error(context, "multipart file field " + field_name + " has no files");
        return nullptr;
    }
    return values;
}

cJSON *multipart_file_validation_value(cJSON *files_array,
                                       const std::string &field_name,
                                       cJSON *property_schema,
                                       ValidationContext context) {
    const int file_count = cJSON_GetArraySize(files_array);
    if (file_count <= 0) {
        set_error(context, "multipart file field " + field_name + " has no files");
        return nullptr;
    }

    cJSON *file = nullptr;
    cJSON_ArrayForEach(file, files_array) {
        if (!multipart_file_entry_is_present(file, field_name, context)) {
            return nullptr;
        }
    }

    JsonPtr array_value(multipart_file_metadata_array(files_array, field_name, context), cJSON_Delete);
    if (array_value == nullptr) {
        return nullptr;
    }

    std::set<const cJSON *> uploads;
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, array_value.get()) uploads.insert(entry);
    context.uploads = &uploads;
    if (file_count > 1) {
        if (property_schema != nullptr &&
            !value_matches_schema_without_reporting(array_value.get(), property_schema, context)) {
            set_error(context, "multipart file field " + field_name +
                               " has multiple files but its schema does not accept an array");
            return nullptr;
        }
        return array_value.release();
    }

    cJSON *first_file = cJSON_GetArrayItem(files_array, 0);
    JsonPtr single_value(multipart_file_metadata(first_file, context), cJSON_Delete);
    if (single_value == nullptr) {
        return nullptr;
    }

    uploads.insert(single_value.get());
    if (schema_expects_array(property_schema)) {
        return array_value.release();
    }
    if (property_schema != nullptr &&
        !value_matches_schema_without_reporting(single_value.get(), property_schema, context) &&
        value_matches_schema_without_reporting(array_value.get(), property_schema, context)) {
        return array_value.release();
    }
    return single_value.release();
}

bool add_multipart_form_fields(cJSON *body, cJSON *form, ValidationContext context) {
    if (form == nullptr || cJSON_IsNull(form)) {
        return true;
    }
    if (!cJSON_IsObject(form)) {
        set_error(context, "request form must be an object");
        return false;
    }
    cJSON *field = nullptr;
    cJSON_ArrayForEach(field, form) {
        if (field->string == nullptr) {
            set_error(context, "request form field is missing a name");
            return false;
        }
        cJSON *copy = cJSON_Duplicate(field, 1);
        if (copy == nullptr) {
            set_error(context, "failed to allocate multipart form validation field");
            return false;
        }
        cJSON_AddItemToObject(body, field->string, copy);
    }
    return true;
}

bool add_multipart_file_fields(cJSON *body, cJSON *files, cJSON *schema, ValidationContext context) {
    if (files == nullptr || cJSON_IsNull(files)) {
        return true;
    }
    if (!cJSON_IsObject(files)) {
        set_error(context, "request files must be an object");
        return false;
    }

    cJSON *field = nullptr;
    cJSON_ArrayForEach(field, files) {
        if (field->string == nullptr) {
            set_error(context, "request file field is missing a name");
            return false;
        }
        const std::string field_name = field->string;
        if (cJSON_GetObjectItemCaseSensitive(body, field->string) != nullptr) {
            set_error(context, "multipart field " + field_name + " is present in both form and files");
            return false;
        }
        if (!cJSON_IsArray(field)) {
            set_error(context, "multipart file field " + field_name + " must be an array");
            return false;
        }

        cJSON *property_schema = direct_object_property_schema(schema, field->string);
        cJSON *value = multipart_file_validation_value(field, field_name, property_schema, context);
        if (value == nullptr) {
            return false;
        }
        cJSON_AddItemToObject(body, field->string, value);
    }
    return true;
}

bool validate_multipart_body(cJSON *media_object, cJSON *form, cJSON *files, ValidationContext context) {
    if (!cJSON_IsObject(media_object)) {
        set_error(context, "requestBody media type object must be an object");
        return false;
    }
    cJSON *schema = media_schema(media_object);
    JsonPtr body(cJSON_CreateObject(), cJSON_Delete);
    if (body == nullptr) {
        set_error(context, "failed to allocate multipart request body");
        return false;
    }
    if (!add_multipart_form_fields(body.get(), form, context) ||
        !add_multipart_file_fields(body.get(), files, schema, context)) {
        return false;
    }

    std::set<const cJSON *> uploads;
    cJSON *field = nullptr;
    cJSON_ArrayForEach(field, files) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(body.get(), field->string);
        if (cJSON_IsArray(value)) {
            cJSON *entry = nullptr;
            cJSON_ArrayForEach(entry, value) uploads.insert(entry);
        } else {
            uploads.insert(value);
        }
    }
    ValidationContext form_context = context;
    form_context.uploads = &uploads;
    form_context.coerce_strings = true;
    if (schema && !validate_schema(body.get(), schema, form_context)) return false;
    if (!project_body(body.get(), media_object, form_context)) return false;
    if (context.prepared) {
        cJSON *projected_body = cJSON_GetObjectItemCaseSensitive(context.prepared, "body");
        cJSON *projected_form = cJSON_GetObjectItemCaseSensitive(context.prepared, "form");
        cJSON *field = nullptr;
        cJSON_ArrayForEach(field, form) {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(projected_body, field->string);
            if (value && !save_projection(projected_form, field->string, project_value(value, {}, context))) return false;
        }
        if (files && !save_projection(context.prepared, "files", project_value(files, {}, context))) return false;
    }
    return true;
}

std::string request_content_type(cJSON *request) {
    cJSON *request_context = cJSON_GetObjectItemCaseSensitive(request, "context");
    cJSON *content_type = cJSON_IsObject(request_context)
                                  ? cJSON_GetObjectItemCaseSensitive(request_context, "content_type")
                                  : nullptr;
    return cJSON_IsString(content_type) && content_type->valuestring != nullptr
                   ? normalize_content_type(content_type->valuestring)
                   : "";
}

cJSON *media_object_for_request_content_type(cJSON *content,
                                             const std::string &content_type,
                                             ValidationContext context) {
    if (content_type.empty()) {
        set_error(context, "request content_type is missing");
        return nullptr;
    }
    cJSON *media = nullptr;
    cJSON_ArrayForEach(media, content) {
        if (media->string != nullptr &&
            cJSON_IsObject(media) &&
            normalize_content_type(media->string) == content_type) {
            return media;
        }
    }
    set_error(context, "requestBody content type is unsupported: " + content_type);
    return nullptr;
}

bool validate_request_body(cJSON *request, cJSON *request_body, ValidationContext context) {
    if (request_body == nullptr) {
        return true;
    }
    if (!cJSON_IsObject(request_body)) {
        set_error(context, "requestBody must be an object");
        return false;
    }
    if (has_ref(request_body)) {
        set_error(context, "requestBody references are not supported");
        return false;
    }
    cJSON *content = cJSON_GetObjectItemCaseSensitive(request_body, "content");
    if (!cJSON_IsObject(content)) {
        set_error(context, "requestBody.content must be an object");
        return false;
    }

    const bool required = json_bool(request_body, "required");
    cJSON *raw_body_item = cJSON_GetObjectItemCaseSensitive(request, "raw_body");
    const std::string raw_body = cJSON_IsString(raw_body_item) && raw_body_item->valuestring != nullptr
                                         ? raw_body_item->valuestring
                                         : "";
    cJSON *form = cJSON_GetObjectItemCaseSensitive(request, "form");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(request, "files");
    const bool raw_present = !raw_body.empty() || json_bool(request, "body_present");
    const bool form_present = object_has_children(form);
    const bool files_present = object_has_children(files);
    if (!raw_present && !form_present && !files_present) {
        if (required) {
            set_error(context, "request is missing required body");
            return false;
        }
        return true;
    }

    const std::string content_type = request_content_type(request);
    cJSON *media_object = media_object_for_request_content_type(content, content_type, context);
    if (media_object == nullptr) {
        return false;
    }

    if (media_type_is_urlencoded_form(content_type)) {
        ValidationContext form_context = context;
        form_context.coerce_strings = true;
        if (!validate_media_schema(media_object, form, form_context) ||
            !project_body(form, media_object, form_context)) return false;
        return !context.prepared || save_projection(context.prepared, "form",
            project_value(cJSON_GetObjectItemCaseSensitive(context.prepared, "body"), {}, context));
    }

    if (media_type_is_multipart_form(content_type)) {
        return validate_multipart_body(media_object, form, files, context);
    }

    if (media_type_is_json(content_type)) {
        return validate_json_body(media_object, raw_body, context);
    }

    if (media_type_is_text(content_type)) {
        return validate_text_body(media_object, raw_body, context);
    }

    set_error(context, "requestBody content type is unsupported by this validator: " + content_type);
    return false;
}

bool validate_parameters(cJSON *request, cJSON *parameters, ValidationContext context) {
    if (parameters == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(parameters)) {
        set_error(context, "parameters must be an array");
        return false;
    }
    cJSON *parameter = nullptr;
    cJSON_ArrayForEach(parameter, parameters) {
        if (!validate_parameter(request, parameter, context)) {
            return false;
        }
    }
    return true;
}

bool request_method_is_get(cJSON *request) {
    return lowercase_ascii_copy(trim_ascii_copy(json_string(request, "method"))) == "get";
}

}  // namespace

bool validate_schema(cJSON *value, cJSON *schema) {
    std::string error;
    return validate_schema(value, schema, error);
}

bool validate_schema(cJSON *value, cJSON *schema, std::string &error_out) {
    error_out.clear();
    if (value == nullptr) {
        error_out = "value must be a JSON value";
        return false;
    }
    ValidationContext context;
    context.error = &error_out;
    return validate_schema(value, schema, context);
}

bool validate_request(cJSON *request, cJSON *operation_schema) {
    std::string error;
    return validate_request(request, operation_schema, error);
}

static bool validate_request_impl(cJSON *request, cJSON *operation_schema, std::string &error_out, cJSON *prepared) {
    error_out.clear();
    if (!cJSON_IsObject(request)) {
        error_out = "request must be an object";
        return false;
    }
    if (!cJSON_IsObject(operation_schema)) {
        error_out = "operation schema must be an object";
        return false;
    }
    if (has_ref(operation_schema)) {
        error_out = "operation schema references are not supported";
        return false;
    }

    ValidationContext context;
    context.error = &error_out;
    context.prepared = prepared;
    cJSON *request_body = cJSON_GetObjectItemCaseSensitive(operation_schema, "requestBody");
    if (request_method_is_get(request)) {
        request_body = nullptr;
    }
    return validate_parameters(request,
                               cJSON_GetObjectItemCaseSensitive(operation_schema, "parameters"),
                               context) &&
           validate_request_body(request, request_body, context);
}

bool validate_request(cJSON *request, cJSON *operation_schema, std::string &error_out) {
    return validate_request_impl(request, operation_schema, error_out, nullptr);
}

cJSON *prepare_request(cJSON *request, cJSON *operation_schema, std::string &error_out) {
    JsonPtr result(cJSON_CreateObject(), cJSON_Delete);
    if (!result) throw std::bad_alloc();
    for (const char *name : {"params", "query", "headers", "cookies", "form", "files"}) {
        if (!cJSON_AddObjectToObject(result.get(), name)) throw std::bad_alloc();
    }
    if (!validate_request_impl(request, operation_schema, error_out, result.get())) return nullptr;
    return result.release();
}

}  // namespace topenapi
