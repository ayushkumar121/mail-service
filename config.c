#include "config.h"
#include "basic.h"

static JsonValue* config = NULL;

Error config_load(const char *path) {
  assert(path != NULL);
  if (!file_exists(path)) {
    return errorf("file %s does not exist", path);
  }

  StringBuilder sb = {0};
  try(read_entire_file(path, &sb));
  String file_content = sb_to_sv(&sb);
  
  Error err = json_decode(file_content, &config);
  sb_free(&sb);

  if (has_error(err)) {
    return err;
  }

  INFO("config loaded successfully");
  return ErrorNil;
}

String config_get_string(String key, String default_value) {
  assert(config != NULL);
  JsonValue* value = json_get(config, key);
  if (value == NULL) return default_value;
  return json_get_string(value);
} 

double config_get_double(String key, int default_value) {
  assert(config != NULL);
  JsonValue* value = json_get(config, key);
  if (value == NULL) return default_value;
  return json_get_number(value);
}

int config_get_int(String key, int default_value) {
  assert(config != NULL);
  return (int)config_get_double(key, default_value);
}

void config_free(void) {
  assert(config != NULL);
  json_free(config);
}

String get_hostname(void) {
  static String host;
  if (host.length == 0) {
    host = config_get_string(SV("server.host"), SV("localhost"));
  }
  return host;
}

String get_local_domain(void) {
  static String local_domain;
  if (local_domain.length == 0) {
    local_domain = config_get_string(SV("server.local_domain"), get_hostname());
  }
  return local_domain;
}

String get_maildir(void) {
  static String maildir;
  if (maildir.length == 0) {
    maildir = config_get_string(SV("server.maildir"), SV("maildir"));
  }
  const char* maildir_cstr = sv_to_tmp_c(maildir);
  if (!file_exists(maildir_cstr)) {
    make_directory(maildir_cstr);
  }
  return maildir;
}

String get_logdir(void) {
  static String logdir;
  if (logdir.length == 0) {
    logdir = config_get_string(SV("server.logdir"), SV("logs"));
  }
  const char* logdir_cstr = sv_to_tmp_c(logdir);
  if (!file_exists(logdir_cstr)) {
    make_directory(logdir_cstr);
  }
  return logdir;
}

String get_auth_username(void) {
  static String username;
  if (username.length == 0) {
    username = config_get_string(SV("auth.username"), StringNil);
  }
  return username;
}

String get_auth_password(void) {
  static String password;
  if (password.length == 0) {
    password = config_get_string(SV("auth.password"), StringNil);
  }
  return password;
}
