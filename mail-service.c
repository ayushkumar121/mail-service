#include "basic.h"
#include "http.h"
#include "config.h"
#include "smtp.h"

// Figure out Schedulers
// TODO: figure out imap protocol

HttpResponse http_listen_callback(const HttpRequest* request) {
  return http_text_response(200, SV("Hello World"));
}

int main(int argc, char** argv) {
  try(config_load("config.json"));

  smtp_send((Email){
    .from    = SV("me@ayush-kumar.com"),
    .to      = SV("test@localhost"),
    .subject = SV("Hello"),
    .body    = SV("Hello from my SMTP client!"),
  });

  HttpServer server = {0};
  HttpServerInitOptions options = http_server_init_defaults();
  options.port = config_get_int(SV("server.port"), 8080);

  try(http_server_init_opts(&server, options));
  try(http_server_listen(&server, http_listen_callback));
  return 0;
}
