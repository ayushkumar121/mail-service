#ifndef CONFIG_H
#define CONFIG_H

#include "basic.h"

Error config_load(const char *path);
String config_get_string(String key, String default_value);
double config_get_double(String key, int default_value);
int config_get_int(String key, int default_value);
void config_free(void);

String get_hostname(void);      // server.host — FQDN we advertise (EHLO/banner)
String get_local_domain(void);  // server.local_domain — @domain we accept as local
String get_maildir(void);

String get_auth_username(void);  // auth.username — the single account we authenticate
String get_auth_password(void);  // auth.password — plaintext password for that account

#endif