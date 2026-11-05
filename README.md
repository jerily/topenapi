# topenapi

OpenAPI request validation for Tcl and C. The package validates inline operation
fragments and JSON Schema objects, with a C adapter for `twebserver` requests.

## Build

Requires CMake 3.22.1+, C11/C++17, cJSON 1.7.19+, and Tcl 8.6 or 9.x development
headers. Loadable extensions use **Tcl stubs**, including `USE_TCL_STUBS`,
`Tcl_InitStubs`, and the matching stub archive. They do not link the Tcl runtime.
Build separately for Tcl 8.6 and Tcl 9; the package index enforces that boundary.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/install \
  -DCMAKE_PREFIX_PATH=/path/to/dependencies \
  -DTCL_INCLUDE_PATH=/path/to/tcl/include \
  -DTCL_STUB_LIBRARY=/path/to/tcl/lib/libtclstub.a \
  -DTCL_TCLSH=/path/to/tcl/bin/tclsh9.1
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

Tcl 8.6 normally names its stub archive `libtclstub8.6.a`. These explicit paths
are optional when discovery finds the intended installation. For cJSON, set
`CJSON_INCLUDE_DIR` and `CJSON_LIBRARY` when necessary. A static cJSON archive
must be built with position independent code for use in the shared extension.

`-DBUILD_SHARED_LIBS=OFF` builds static archives for embedding. Link
`libtopenapi.a`, `libtopenapi_core.a`, cJSON, Tcl and the C++ runtime; register
`Topenapi_Init` with `Tcl_StaticPackage` (`Tcl_StaticLibrary` on Tcl 9), then
`load {} Topenapi`. The installed package index performs that load when the
application has registered the package. Static wrappers use direct Tcl calls,
as in tjson. See `tests/embed.c` for a complete host.

## Tcl commands

Validation inputs are **JSON text**, preserving JSON types without guessing
whether a Tcl string represents an object, list, number or boolean.

```tcl
package require topenapi 0.2.0
set operation {{"parameters":[
  {"name":"id","in":"path","required":true,
   "schema":{"type":"integer","minimum":1}}
]}}
set validator [topenapi::compile $operation]
set request [topenapi::from_twebserver {
  httpMethod GET pathParameters {id 42} headers {Accept application/json}
}]
set result [$validator validate $request]
if {![dict get $result valid]} {error [dict get $result error]}
$validator destroy
```

| Command | Result |
| --- | --- |
| `topenapi::validate_schema valueJson schemaJson` | Tcl dict `{valid BOOLEAN error MESSAGE}` |
| `topenapi::validate_request requestJson operationJson` | Same validation result |
| `topenapi::compile operationJson` | Command holding a parsed operation |
| `$validator validate requestJson` | Validation result, reusing the operation |
| `$validator bind requestJson mapping` | Validates, converts and assigns selected caller variables; returns empty |
| `$validator destroy` | Deletes the command and frees its operation |
| `topenapi::from_twebserver requestDict ?parsedFormDict?` | Normalized request JSON |
| `topenapi::files requestJson` | Tcl dict mapping field names to lists of metadata dicts |

A valid result has an empty error string. Validation failures return `valid 0`;
malformed JSON, invalid adapter dictionaries, allocation failures and command
usage errors raise Tcl errors. Package-specific errors start with `TOPENAPI`.
`compile` parses and owns an operation object; it does not perform full OpenAPI
metaschema validation. Schema constraints are checked during validation.
Commands may be renamed. Interpreter deletion frees their state.

## Bind caller variables

`$validator bind requestJson mapping` combines request validation, typed value
extraction and local variable assignment. It uses the compiled OpenAPI operation;
there is no second validation language or dependency on tjv.

```tcl
set request [topenapi::from_twebserver $requestDict]
$validator bind $request {
    user_id {params id}
    page    {query page}
    email   {body user email}
    avatar  {files avatar 0}
}
puts "User $user_id, page $page, email $email"
puts [dict get $avatar filename]
```

The operation must declare the selected parameters and request body. Each mapping
entry is a Tcl variable name followed by a Tcl list describing its field path.
Destinations are caller-scope scalar names matching `[A-Za-z_][A-Za-z0-9_]*`;
existing scalar values are replaced. Namespace-qualified names and array element
names are rejected. Source roots are:

| Source | Values |
| --- | --- |
| `params`, `query`, `headers`, `cookies` | Present parameters declared in the operation |
| `body` | Parsed JSON, a text string, or the combined validated form/file object |
| `form` | Validated and converted ordinary form fields |
| `files` | Field-name lists of upload metadata, including when a field has one file |

Paths traverse dictionary keys and nonnegative decimal array indexes. Header
parameter names are case insensitive; other keys are case sensitive. A root
alone, such as `{body}`, selects the entire value. GET bodies and absent optional
bodies are unavailable. Undeclared parameters and missing paths are errors;
`bind` does not guess defaults or retain an old value for a missing selected field.
Optional null properties treated as absent by validation are omitted. Other JSON
nulls become empty Tcl values. Objects become dictionaries and arrays become
lists. Integral numbers in the signed 64-bit range become Tcl wide integers;
other numbers become doubles. cJSON's existing double-precision limit still
applies. Booleans become Tcl booleans; strings remain strings unless a parameter
or form schema requires coercion. JSON body types are preserved.

Array parameters support repeated values and comma-separated strings. JSON
parameter content is parsed and JSON bodies are parsed once during preparation.
For unions, a branch accepting the original type takes precedence over coercion;
otherwise declaration order selects the conversion. Incompatible conversions
across `allOf` schemas fail explicitly. Binding traversal is limited to 64 levels.
File bindings expose metadata; the adapter does not copy upload contents.

Validation, conversion, mapping checks and field extraction all finish **before
any destination is assigned**. These failures leave caller variables unchanged.
Tcl errors have `TOPENAPI VALIDATION`, `BIND_MAPPING`, `BIND_PATH` or `BIND_VALUE`
codes where applicable; standard Tcl argument/dictionary errors are propagated.
Assignments then follow mapping order and use ordinary Tcl variable semantics.
A write trace error or an existing array destination can stop assignment after
earlier variables were written; trace side effects are not rolled back. A trace
may delete or rename the validator safely. The complete runnable example is
[`examples/bind.tcl`](examples/bind.tcl).

## Request contract

The C API accepts the same structure as an in-memory cJSON object:

```json
{
  "method": "POST",
  "params": {"id": "42"},
  "query": {"page": "2", "tag": ["red", "blue"]},
  "headers": {"content-type": "application/json"},
  "cookies": {"session": "example"},
  "context": {"content_type": "application/json"},
  "raw_body": "{\"enabled\":true}",
  "form": {},
  "files": {}
}
```

Parameter sources are `params`, `query`, `headers` and `cookies`. Parameter names
are case sensitive except headers. Numeric and boolean strings are coerced only
for parameters and form fields. Repeated values are arrays. Comma-delimited
strings can satisfy array schemas. JSON body values are strictly typed.

The adapter maps `httpMethod`, `pathParameters`, `queryStringParameters`,
`multiValueQueryStringParameters`, `headers`, `multiValueHeaders`, and `body`.
Multi-value entries override their single-value counterpart. It lowercases
header names, reads cookies from the Cookie header, and accepts an explicit
`cookies` dictionary. The first cookie with a name wins; explicit cookies take
precedence. Cookie values are not URL-decoded. Base64-encoded non-form bodies
are decoded when `isBase64Encoded` is true. Text values use UTF-8.

For forms, parse once with the server and pass the result explicitly:

```tcl
set form [twebserver::get_form $requestDict]
set request [topenapi::from_twebserver $requestDict $form]
set metadata [topenapi::files $request]
foreach file [dict get $metadata attachment] {
    puts "[dict get $file filename]: [dict get $file size] bytes"
    # Only the application accesses the original content, when needed:
    set encoded [dict get $form files [dict get $file source_key]]
}
```

The adapter itself does not invoke Tcl commands or link to twebserver. A nonempty
form body requires `parsedFormDict`; otherwise conversion raises an error. It
uses `fields`, `multiValueFields`, and `files` from `twebserver::get_form`.

### Uploads

`files` maps field names to arrays of metadata objects:

```json
{"attachment":[{"filename":"report.pdf","source_key":"report.pdf",
                 "size":1234,"encoding":"base64"}]}
```

`size` is the decoded byte count. The adapter checks base64 in place without
allocating decoded upload bytes. Neither upload contents nor raw multipart
bodies are included in the normalized JSON. The application retains ownership
of the original request and parsed form; metadata remains usable independently.
No files are opened, created, moved or deleted.

An upload property can use an object schema to validate metadata (for example
`properties.size.maximum`), or the standard `type: string, format: binary`.
For binary upload schemas, `minLength`/`maxLength` constrain decoded **bytes**;
content patterns are rejected because file contents are unavailable. Multiple
files require a schema accepting an array, including when only one file is
submitted. Required fields are checked across the combined form/file namespace.
Named files must have a positive integral size. Empty uploads are rejected.

The server's parser keys contents by filename, so duplicate filenames may
already have overwritten each other before adaptation. It does not preserve
per-file media types or zero-byte uploads; the adapter does not invent those
values. A form value matching an existing filename is treated as an upload.
Mixed text and file values under one field are rejected. Applications needing
unambiguous duplicate filenames must supply richer metadata through the native
request contract or use a parser that preserves that information.

## Validation scope

This is a focused operation-fragment validator, not a complete OpenAPI document
validator. It does not perform routing, authentication, response validation,
reference resolution, network access or file access. Inline schemas support:

- Types, type arrays, `nullable`, `enum`, `const`, `allOf`, `anyOf`, `oneOf`, `not`.
- String `minLength`, `maxLength`, and ECMAScript `pattern` (Unicode code-point
  length; regular expression matching operates on UTF-8 bytes).
- `minimum`, `maximum`, both OpenAPI 3.0 boolean and numeric exclusive bounds,
  and `multipleOf`.
- Array `items`, `minItems`, `maxItems`, `uniqueItems`.
- Object `required`, `properties`, `additionalProperties`, `minProperties`,
  `maxProperties`.

Bodies support JSON and `+json`, text/plain, URL-encoded forms and multipart.
`context.content_type` selects an exact media type after removing parameters;
there is no content-type guessing or wildcard negotiation. GET requests ignore
`requestBody`. `body_present` can record an empty parsed form whose raw body was
present. JSON bodies must contain exactly one complete value.

Optional declared object properties with null values are treated as missing.
Required and undeclared null properties are validated normally. References
encountered while validating schemas are rejected. Other unsupported keywords,
including ordinary `format` assertions, are ignored. Parameter serialization
styles beyond comma-separated and repeated values are not implemented. Numbers
use cJSON doubles; integer precision is limited to 53 bits. Equality for nested
objects uses their serialized representation, so member order can affect enum,
const and uniqueItems. cJSON strings cannot contain embedded NUL; Tcl JSON input
and JSON bodies reject it. Use trusted schemas and bound request sizes at the
server; regex matching is not time limited. Schema recursion is limited to 64.

## C API

```c
#include <cJSON.h>
#include <topenapi/topenapi.h>

char error[2048];
int valid = topenapi_validate_request(request, operation, error, sizeof(error));
```

`topenapi_validate_schema` and
`topenapi_validate_request` return 1 or 0, borrow their cJSON inputs without
mutation, and catch C++ exceptions. Optional caller-owned error buffers are NUL
terminated when capacity is nonzero; long diagnostics are truncated. Link
`libtopenapi_core.a`, cJSON and the C++ runtime for validation without Tcl.

Include `topenapi/topenapi_tcl.h` for `Topenapi_Init` and the C adapter:

```c
cJSON *normalized = NULL;
if (Topenapi_FromTwebserver(interp, request_dict, parsed_form, &normalized) == TCL_OK) {
    int valid = topenapi_validate_request(normalized, operation, error, sizeof(error));
    /* Use valid and error here. */
    cJSON_Delete(normalized);
}
```

`topenapi_prepare_request(request, operation, &output, error, capacity)` exposes
the same validation and conversion stage to C callers. It returns 1 on success
with a caller-owned cJSON tree (release it with `cJSON_Delete`) or 0 with `output`
set to NULL. Inputs remain unmodified. The tree contains the binding source maps
and a `body` member when the operation validates a present body. It contains no
raw body string. This function also catches C++ exceptions and accepts optional
bounded error storage. It performs no Tcl variable assignment.

The adapter returns TCL_OK/TCL_ERROR and sets interpreter diagnostics on failure.
It borrows Tcl objects only for the duration of the call. `parsed_form` may be
NULL for non-form requests. Call it on the interpreter's owning thread after
package initialization. Independent interpreters/requests share no mutable
validator state.

## Tests

CTest covers the C API, schema/request rules, adapter dictionaries, large uploads,
command lifetime and installation. Run the optional parser integration with an
installed twebserver:

```sh
tclsh9.1 tests/twebserver.tcl
```

CI builds Tcl 8.6 and 9.1 in shared and static configurations.
