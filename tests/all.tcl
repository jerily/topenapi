package require tcltest 2
package require topenapi 0.2.0
namespace import ::tcltest::test
proc valid {result} {dict get $result valid}

test schema-1 {Typed JSON values are not confused with Tcl strings} -body {
    list [valid [topenapi::validate_schema 42 {{"type":"integer"}}]] \
        [valid [topenapi::validate_schema {"42"} {{"type":"integer"}}]]
} -result {1 0}

test schema-2 {Complete JSON input required} -body {
    list [catch {topenapi::validate_schema {42 garbage} {{}}}] \
        [catch {topenapi::validate_schema 42 {[}}]
} -result {1 1}

test schema-3 {Unicode length counts code points} -body {
    valid [topenapi::validate_schema {"é"} {{"type":"string","minLength":1,"maxLength":1}}]
} -result 1

test schema-4 {NUL cannot silently truncate a JSON string} -body {
    list [catch {topenapi::validate_schema {"a\u0000b"} {{"const":"a"}}}] \
        [valid [topenapi::validate_schema {"\\u0000"} {{"type":"string"}}]]
} -result {1 1}

test adapter-1 {Query, path, case insensitive headers, cookie and repeated values} -body {
    set request [topenapi::from_twebserver {httpMethod GET pathParameters {id 42}
        queryStringParameters {tag second} multiValueQueryStringParameters {tag {first second}}
        headers {X-Trace abc Cookie {session=xyz; theme=dark}}}]
    set schema {{"parameters":[
        {"name":"id","in":"path","schema":{"type":"integer","minimum":1}},
        {"name":"tag","in":"query","schema":{"type":"array","minItems":2,"items":{"type":"string"}}},
        {"name":"x-TRACE","in":"header","required":true,"schema":{"const":"abc"}},
        {"name":"theme","in":"cookie","required":true,"schema":{"const":"dark"}}
    ]}}
    valid [topenapi::validate_request $request $schema]
} -result 1

test adapter-2 {Text is not guessed from its Tcl list or dictionary representation} -body {
    set request [topenapi::from_twebserver {queryStringParameters {q {hello world}}}]
    valid [topenapi::validate_request $request {{"parameters":[{"name":"q","in":"query","schema":{"const":"hello world"}}]}}]
} -result 1

test body-1 {Base64 JSON bodies are decoded and strictly validated} -body {
    set request [topenapi::from_twebserver [dict create httpMethod POST \
        headers {Content-Type {application/json; charset=utf-8}} \
        isBase64Encoded 1 body [binary encode base64 {{"count":2}}]]]
    valid [topenapi::validate_request $request {{"requestBody":{"required":true,"content":{"application/json":{"schema":{"type":"object","required":["count"],"properties":{"count":{"type":"integer"}}}}}}}}]
} -result 1

test body-2 {Invalid JSON bodies with valid prefixes are rejected} -body {
    set request [topenapi::from_twebserver {httpMethod POST headers {content-type application/json} body {{} garbage}}]
    valid [topenapi::validate_request $request {{"requestBody":{"content":{"application/json":{"schema":{"type":"object"}}}}}}]
} -result 0

test body-3 {URL encoded forms use the supplied parser result} -body {
    set request [topenapi::from_twebserver {httpMethod POST headers {content-type application/x-www-form-urlencoded} body {count=3}} {fields {count 3}}]
    valid [topenapi::validate_request $request {{"requestBody":{"required":true,"content":{"application/x-www-form-urlencoded":{"schema":{"properties":{"count":{"type":"integer","minimum":2}}}}}}}}]
} -result 1

test body-4 {Form bodies require an explicit parsed form} -body {
    catch {topenapi::from_twebserver {headers {content-type multipart/form-data} body content}} message options
    dict get $options -errorcode
} -result {TOPENAPI FORM}

set upload_request {httpMethod POST headers {content-type {multipart/form-data; boundary=x}} body OMIT_THIS_RAW_PAYLOAD}
set parsed_form {fields {title hello image sample.bin} files {sample.bin AQID}}
set multipart_schema {{"requestBody":{"required":true,"content":{"multipart/form-data":{"schema":{"type":"object","required":["image","title"],"additionalProperties":false,"properties":{"title":{"type":"string"},"image":{"type":"object","required":["filename","size"],"properties":{"size":{"type":"integer","minimum":1,"maximum":3}}}}}}}}}}

test files-1 {Metadata has a lookup key and no body or upload payload} -body {
    set request [topenapi::from_twebserver $upload_request $parsed_form]
    set meta [lindex [dict get [topenapi::files $request] image] 0]
    list [valid [topenapi::validate_request $request $multipart_schema]] \
        [dict get $meta filename] [expr {[dict get $meta size] == 3}] \
        [dict get $parsed_form files [dict get $meta source_key]] \
        [expr {[string first AQID $request] == -1}] \
        [expr {[string first OMIT_THIS_RAW_PAYLOAD $request] == -1}]
} -result {1 sample.bin 1 AQID 1 1}

test files-2 {Binary schemas use decoded size with no content copy} -body {
    set request [topenapi::from_twebserver $upload_request $parsed_form]
    set binary [string map {{"type":"object","required":["filename","size"],"properties":{"size":{"type":"integer","minimum":1,"maximum":3}}} {"type":"string","format":"binary","minLength":1,"maxLength":3}} $multipart_schema]
    list [valid [topenapi::validate_request $request $binary]] \
        [valid [topenapi::validate_request $request [string map {maxLength\":3 maxLength\":2} $binary]]]
} -result {1 0}

test files-3 {Multiple files are arrays of metadata and never overwrite each other} -body {
    set request [topenapi::from_twebserver $upload_request {fields {image b.bin} multiValueFields {image {a.bin b.bin}} files {a.bin AQID b.bin BAUG}}]
    list [llength [dict get [topenapi::files $request] image]] \
        [valid [topenapi::validate_request $request {{"requestBody":{"content":{"multipart/form-data":{"schema":{"properties":{"image":{"type":"array","minItems":2,"items":{"type":"string","format":"binary"}}}}}}}}}]]
} -result {2 1}

test files-4 {Malformed upload base64 and mixed text/file fields fail conversion} -body {
    list [catch {topenapi::from_twebserver $upload_request {fields {image a.bin} files {a.bin ??==}}}] \
        [catch {topenapi::from_twebserver $upload_request {fields {image a.bin} multiValueFields {image {a.bin text}} files {a.bin AQID}}}]
} -result {1 1}

test files-5 {Large upload content does not grow the normalized request} -body {
    set form [dict create fields {image big.bin} files [dict create big.bin [string repeat AQID 100000]]]
    set request [topenapi::from_twebserver $upload_request $form]
    list [expr {[string length $request] < 1024}] \
        [expr {[dict get [lindex [dict get [topenapi::files $request] image] 0] size] == 300000}]
} -result {1 1}

test lifetime-1 {Compiled operations own their schema, can be renamed, and are deleted} -body {
    set schema {{"parameters":[{"name":"id","in":"path","schema":{"type":"integer"}}]}}
    set validator [topenapi::compile $schema]
    unset schema
    rename $validator ::renamed_validator
    set result [list [valid [::renamed_validator validate {{"params":{"id":"7"}}}]] \
        [valid [::renamed_validator validate {{"params":{}}}]]]
    ::renamed_validator destroy
    lappend result [llength [info commands ::renamed_validator]]
} -result {1 0 0}

test errors-1 {Malformed dictionaries and command arguments fail without crashing} -body {
    list [catch {topenapi::from_twebserver odd}] \
        [catch {topenapi::from_twebserver {headers odd}}] \
        [catch {topenapi::validate_request {}}] [catch {topenapi::compile {[]}}]
} -result {1 1 1 1}

test schema-5 {Compositions, bounds, arrays and object restrictions} -body {
    list [valid [topenapi::validate_schema 4 {{"allOf":[{"type":"integer"},{"minimum":3}],"not":{"const":5}}}]] \
        [valid [topenapi::validate_schema 4 {{"oneOf":[{"type":"integer"},{"type":"number"}]}}]] \
        [valid [topenapi::validate_schema {[1,1]} {{"type":"array","uniqueItems":true}}]] \
        [valid [topenapi::validate_schema {{"extra":1}} {{"type":"object","additionalProperties":false}}]] \
        [valid [topenapi::validate_schema null {{"type":"integer","nullable":true}}]]
} -result {1 0 0 0 1}

test body-5 {Escaped NUL in a JSON body cannot bypass a const constraint} -body {
    set request [topenapi::from_twebserver {httpMethod POST headers {content-type application/json} body {"a\u0000b"}}]
    valid [topenapi::validate_request $request {{"requestBody":{"content":{"application/json":{"schema":{"const":"a"}}}}}}]
} -result 0

test files-6 {Malformed metadata, missing uploads and zero sizes fail validation} -body {
    set request [topenapi::from_twebserver $upload_request $parsed_form]
    list [valid [topenapi::validate_request [string map {\"size\":3 \"size\":0} $request] $multipart_schema]] \
        [valid [topenapi::validate_request [string map {\"size\":3 \"size\":1.5} $request] $multipart_schema]] \
        [valid [topenapi::validate_request [topenapi::from_twebserver $upload_request {fields {title hello}}] $multipart_schema]]
} -result {0 0 0}

test body-6 {Bad base64 and missing content types fail deterministically} -body {
    list [catch {topenapi::from_twebserver {body AB== isBase64Encoded 1}}] \
        [catch {topenapi::from_twebserver {body A=== isBase64Encoded 1}}] \
        [valid [topenapi::validate_request {{"method":"POST","raw_body":"{}"}} {{"requestBody":{"content":{"application/json":{"schema":{}}}}}}]]
} -result {1 1 0}

source [file join [file dirname [info script]] bind.test]

set failed $::tcltest::numTests(Failed)
::tcltest::cleanupTests
exit [expr {$failed > 0}]
