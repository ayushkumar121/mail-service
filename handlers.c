#include "handlers.h"

#include "metrics.h"

HttpResponse http_handler(const HttpRequest* request) {
    if (sv_equal_ignore_case(request->path, SV("/metrics"))) {
        return metrics_handler(request);
    }
    return http_status_response(404);
}
