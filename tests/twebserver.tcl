# Optional integration test against an installed twebserver (no listening socket).
package require twebserver 1.47.54
package require topenapi 0.1.0
set body "--example\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nReport\r\n--example\r\nContent-Disposition: form-data; name=\"attachment\"; filename=\"report.txt\"\r\nContent-Type: text/plain\r\n\r\nabc\r\n--example--\r\n"
set request [dict create httpMethod POST headers {content-type {multipart/form-data; boundary=example}} \
    multipartBoundary example body [binary encode base64 $body] isBase64Encoded 1]
set parsed [twebserver::get_form $request]
set normalized [topenapi::from_twebserver $request $parsed]
set file [lindex [dict get [topenapi::files $normalized] attachment] 0]
if {[dict get $file filename] ne "report.txt" || [dict get $file size] != 3} {error "incorrect file metadata: $file"}
if {[dict get $parsed files [dict get $file source_key]] ne "YWJj"} {error "incorrect source lookup"}
if {[string first YWJj $normalized] >= 0 || [string first Content-Disposition $normalized] >= 0} {error "copied upload payload"}
set request {httpMethod POST headers {content-type application/x-www-form-urlencoded} body {count=3&tag=red&tag=blue}}
set normalized [topenapi::from_twebserver $request [twebserver::get_form $request]]
if {![dict get [topenapi::validate_request $normalized {{"requestBody":{"content":{"application/x-www-form-urlencoded":{"schema":{"properties":{"count":{"type":"integer"},"tag":{"type":"array","minItems":2}}}}}}}}] valid]} {error "form validation failed"}
puts "Real twebserver multipart and URL-encoded adapter tests passed"
