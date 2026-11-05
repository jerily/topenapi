package require topenapi 0.2.0

set operation {{"parameters":[
  {"name":"id","in":"path","schema":{"type":"integer","minimum":1}},
  {"name":"page","in":"query","schema":{"type":"integer","minimum":1}}
]}}
set validator [topenapi::compile $operation]
try {
    set request [topenapi::from_twebserver {
        httpMethod GET pathParameters {id 42} queryStringParameters {page 2}
    }]
    $validator bind $request {
        user_id {params id}
        page    {query page}
    }
    puts "User $user_id, page $page"
} finally {
    $validator destroy
}
