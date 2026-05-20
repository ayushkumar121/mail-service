#include "handlers.h"

#include "metrics.h"

HttpResponse http_handler(const HttpRequest* request) {
    if (sv_equal_ignore_case(request->method, SV("CONNECT"))) {
        return http_status_response(405);
    }

    if (sv_equal_ignore_case(request->method, SV("GET")) && sv_equal_ignore_case(request->path, SV("/metrics"))) {
        return metrics_handler(request);
    }
    
    return http_status_response(404);
}
