#include <pthread.h>

#include "basic.h"
#include "config.h"
#include "imap.h"
#include "smtp.h"

void* smtp_thread(void* arg) {
  SmtpServer* server = arg;
  try(smtp_server_listen(server));
  return NULL;
}

int main(int argc, char** argv) {
  try(config_load("config.json"));

  INFO("Setting up maildir: "SV_Fmt, SV_Arg(get_maildir()));

  SmtpServer smtp_server = {};
  try(smtp_server_init(&smtp_server));

  pthread_t tid;
  pthread_create(&tid, NULL, smtp_thread, &smtp_server);
  pthread_detach(tid);

  ImapServer imap_server = {};
  try(imap_server_init(&imap_server));
  try(imap_server_listen(&imap_server));

  return 0;
}
