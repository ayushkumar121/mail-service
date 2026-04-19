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
  json_print(stderr, *config, 4);

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
