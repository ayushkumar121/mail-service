#include "basic.h"
#include "config.h"
#include "smtp.h"

// Figure out Schedulers
// TODO: figure out imap protocol

int main(int argc, char** argv) {
  try(config_load("config.json"));

  SmtpServer smtp_server = {};
  try(smtp_server_init(&smtp_server));
  try(smtp_server_listen(&smtp_server));
  return 0;
}
