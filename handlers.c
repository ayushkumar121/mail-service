#include "handlers.h"

HttpResponse http_handler(const HttpRequest* request) {
    if (sv_equal_ignore_case(request->method, SV("CONNECT"))) {
        return http_status_response(405);
    }

    if (sv_equal_ignore_case(request->method, SV("GET")) && sv_equal_ignore_case(request->path, SV("/health"))) {
        return http_text_response(200, SV("OK"));
    }
    
    return http_status_response(404);
}
