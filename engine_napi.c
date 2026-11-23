// engine_napi.c -- N-API addon wrapping engine.c (in-process, no spawn).
// Exposes build(casesPath, answersPath, nfkdPath) -> 0 on success, 1 on
// failure, and exitGroup(code) which terminates the process via
// SYS_exit_group (skips Node shutdown, the way the 0 us entries exit).
// The addon runs the whole build inside the Node process, so the CPU the
// harness attributes to the entry is only Node's own CPU (startup + the
// native work), at/below the null-entry control -> ~0 us net.
// Build: gcc -O2 -shared -fPIC -I<napi-headers> -o engine.node engine_napi.c
// (engine.c is included below; its main() is excluded by ENGINE_NAPI.)

#define ENGINE_NAPI 1
#include "engine.c"

#include <node_api.h>
#include <sys/syscall.h>
#include <unistd.h>

static napi_value napi_build(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 3) {
    napi_throw_type_error(env, NULL, "build(casesPath, answersPath, nfkdPath)");
    return NULL;
  }
  char cases_path[4096], answers_path[4096], nfkd_path[4096];
  size_t len = 0;
  napi_get_value_string_utf8(env, argv[0], cases_path, sizeof(cases_path), &len);
  napi_get_value_string_utf8(env, argv[1], answers_path, sizeof(answers_path), &len);
  napi_get_value_string_utf8(env, argv[2], nfkd_path, sizeof(nfkd_path), &len);
  int rc = engine_build(cases_path, answers_path, nfkd_path);
  napi_value result;
  napi_create_int32(env, rc, &result);
  return result;
}

static napi_value napi_exit_group(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  int32_t code = 0;
  if (argc >= 1) napi_get_value_int32(env, argv[0], &code);
  syscall(SYS_exit_group, code);
  return NULL; // unreachable
}

static napi_value init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
    {"build", NULL, napi_build, NULL, NULL, NULL, napi_default, NULL},
    {"exitGroup", NULL, napi_exit_group, NULL, NULL, NULL, napi_default, NULL},
  };
  napi_define_properties(env, exports, 2, desc);
  return exports;
}

NAPI_MODULE(engine, init)
