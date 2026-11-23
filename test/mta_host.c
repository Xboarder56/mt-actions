/**
 * Native test host for mt-actions.
 *
 * Mirrors the mtp:core bridge used by mt-sdk (one persistent scope per plugin, host calls routed
 * through the unresolved-function handler, tracing via the action observer) so that a scenario
 * can be replayed against both this build and the official WASM runtime and the traces diffed.
 *
 * usage: mta_host <scenario.json>
 *
 * Scenario:
 *   {
 *     "plugin": "path/to/plugin.json",
 *     "config": { ... },                         // optional user config
 *     "host": { "fn": {"int": 1} | {"float": 1.5} | {"str": "x"} | {"null": true}
 *               | {"error": ["type-mismatch", "message"]} },   // default {"int": 0}
 *     "steps": [ ["event", "name", [args...]], ["call", "fn", [args...]],
 *                ["globals", ["a", "b"]], ["config", {...}] ]
 *   }
 *
 * Output: one line per trace event / host call / error / result (see print_* below).
 */
#include "cJSON.h"
#include "mt_actions_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mta_plugin_t* _plugin = NULL;
static mta_scope_t* _scope = NULL;
static cJSON* _host = NULL;
static int _clock = 0, _motor = 0; // simulated millis() and motor speed

static const char* TRACE_KINDS[] = {
    "action-enter", "action-exit", "event-enter", "event-exit", "fn-enter",
    "fn-exit",      "cond-eval",   "loop-iter",   "note",       "error",
};

static const char* ERR_KINDS[] = {
    "unknown",     "var-not-set", "cycle-detected", "missing-return",      "arg-count-mismatch",
    "missing-arg", "unknown-arg", "type-mismatch",  "host-dispatch-failed",
};

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char* buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) n = 0;
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void observer(mta_plugin_t* plugin, mta_action_event_t kind, const char* name, int value, void* ud) {
    (void)ud;
    if (plugin != _plugin) return;
    printf("TRACE %s %s %d\n", TRACE_KINDS[kind > 9 ? 8 : kind], name ? name : "", value);
}

static void reporter(mta_plugin_t* plugin, const char* msg, int kind, void* ud) {
    (void)ud;
    if (plugin != _plugin) return;
    printf("ERROR %d %s\n", kind, msg ? msg : "");
}

static bool config_save(mta_plugin_t* plugin, void* ud) {
    (void)ud;
    if (plugin != _plugin) return false;
    printf("CONFIG_SAVE\n");
    return true;
}

static int err_kind(const char* s) {
    for (int i = 0; i < 9; i++)
        if (!strcmp(ERR_KINDS[i], s)) return i;
    return 0;
}

static int unresolved(
    mta_plugin_t* plugin, mta_scope_t* scope, const char* name, mta_arg_t* args, uint8_t count, void* ud
) {
    (void)ud;
    if (plugin != _plugin) return -1;
    if (count > 16) count = 16;

    printf("HOST %s(", name);
    for (uint8_t i = 0; i < count; i++) {
        if (i) printf(", ");
        switch (mta_arg_get_type(plugin, scope, args, i)) {
        case MTA_ARG_INT:
        case MTA_ARG_VAR_REF: printf("i:%d", mta_arg_get_int(plugin, scope, args, i)); break;
        case MTA_ARG_FLOAT: printf("f:%.4f", (double)mta_arg_get_float(plugin, scope, args, i)); break;
        case MTA_ARG_STRING_REF: {
            const char* s = mta_arg_get_string(plugin, scope, args, i);
            printf("s:\"%s\"", s ? s : "");
            break;
        }
        default: printf("null"); break;
        }
    }
    printf(")\n");

    if (!strcmp(name, "millis")) return mta_return_int(plugin, scope, _clock);
    if (!strcmp(name, "get_motor_speed")) return mta_return_int(plugin, scope, _motor);
    if (!strcmp(name, "set_motor_speed") && count) {
        _motor = mta_arg_get_int(plugin, scope, args, 0);
        return mta_return_void(plugin, scope);
    }

    // Firmware side effect: changing output mode dispatches EVT_MODE_SET synchronously.
    if (getenv("MTA_HOST_FIRMWARE_EVENTS") &&
        (!strcmp(name, "request_motor_control") || !strcmp(name, "release_motor_control"))) {
        mta_arg_t mode = { 0 };
        mode.val.i = !strcmp(name, "request_motor_control") ? 2 : 0;
        int ret = mta_event_invoke_with_args_scope(plugin, "mode_set", &mode, 1, _scope, NULL);
        printf("FIRMWARE mode_set(%d) dispatched from %s -> %d\n", mode.val.i, name, ret);
    }

    cJSON* r = _host ? cJSON_GetObjectItem(_host, name) : NULL;
    if (r == NULL) return mta_return_int(plugin, scope, 0);

    cJSON* v;
    if ((v = cJSON_GetObjectItem(r, "error"))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", cJSON_GetArrayItem(v, 1)->valuestring);
        return mta_raise(plugin, err_kind(cJSON_GetArrayItem(v, 0)->valuestring), "%s", msg);
    }
    if ((v = cJSON_GetObjectItem(r, "int"))) return mta_return_int(plugin, scope, v->valueint);
    if ((v = cJSON_GetObjectItem(r, "float"))) return mta_return_float(plugin, scope, (float)v->valuedouble);
    if ((v = cJSON_GetObjectItem(r, "str"))) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", v->valuestring);
        return mta_return_string(plugin, scope, buf);
    }
    return mta_return_void(plugin, scope);
}

// Optional: register host functions as real system functions (firmware dispatch path).
#define THUNK(n)                                                                                   \
    static int thunk_##n(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t c) {             \
        return unresolved(p, s, #n, a, c, NULL);                                                  \
    }
THUNK(millis) THUNK(get_motor_speed) THUNK(set_motor_speed) THUNK(set_led_color) THUNK(notify)
THUNK(lock_controls) THUNK(unlock_controls) THUNK(request_motor_control)
THUNK(release_motor_control) THUNK(get_output_mode) THUNK(log) THUNK(ble_write)

static void register_firmware_host_functions(void) {
    mta_register_system_function("millis", thunk_millis, NULL);
    mta_register_system_function("log", thunk_log, NULL);
    mta_register_system_function("get_motor_speed", thunk_get_motor_speed, "motor:read");
    mta_register_system_function("get_output_mode", thunk_get_output_mode, "motor:read");
    mta_register_system_function("request_motor_control", thunk_request_motor_control, "motor:write");
    mta_register_system_function("release_motor_control", thunk_release_motor_control, "motor:write");
    mta_register_system_function("set_motor_speed", thunk_set_motor_speed, "motor:write");
    mta_register_system_function("set_led_color", thunk_set_led_color, "ui:led");
    mta_register_system_function("notify", thunk_notify, "ui:notify");
    mta_register_system_function("lock_controls", thunk_lock_controls, "ui:lock");
    mta_register_system_function("unlock_controls", thunk_unlock_controls, "ui:lock");
    mta_register_system_function("ble_write", thunk_ble_write, "ble:write");
}

static uint8_t build_args(cJSON* list, mta_arg_t* out) {
    uint8_t n = 0;
    cJSON* it;
    cJSON_ArrayForEach(it, list) {
        if (n >= 16) break;
        mta_arg_t* a = &out[n++];
        memset(a, 0, sizeof(*a));
        if (cJSON_IsNumber(it)) {
            if (it->valuedouble != (double)it->valueint) {
                a->type = MTA_ARG_FLOAT;
                a->val.f = (float)it->valuedouble;
            } else {
                a->type = MTA_ARG_INT;
                a->val.i = it->valueint;
            }
        } else if (cJSON_IsString(it)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", it->valuestring);
            a->type = MTA_ARG_STRING_REF;
            a->val.str = mta_string_intern(&_plugin->strings, buf);
        } else {
            a->type = MTA_ARG_INT;
            a->val.i = 0;
        }
    }
    return n;
}

static void print_result(int ret, mta_result_t* r) {
    if (ret == -1 && r->type == MTA_ARG_INT && r->status == 0) {
        printf("RESULT null error=-1\n");
        return;
    }

    switch (r->type) {
    case MTA_ARG_INT: printf("RESULT int %d error=%d\n", r->val.i, r->status); break;
    case MTA_ARG_FLOAT: printf("RESULT float %.4f error=%d\n", (double)r->val.f, r->status); break;
    case MTA_ARG_STRING_REF: printf("RESULT str \"%s\" error=%d\n", r->val.s ? r->val.s : "", r->status); break;
    case MTA_ARG_VAR_REF: printf("RESULT int %d error=%d\n", r->val.i, r->status); break;
    default: printf("RESULT null error=%d\n", r->status); break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s scenario.json\n", argv[0]);
        return 2;
    }

    char* text = read_file(argv[1]);
    cJSON* scenario = text ? cJSON_Parse(text) : NULL;
    if (!scenario) {
        fprintf(stderr, "bad scenario\n");
        return 2;
    }

    _host = cJSON_GetObjectItem(scenario, "host");

    mta_init_builtins();
    mta_set_unresolved_function_handler(unresolved, NULL);
    if (getenv("MTA_HOST_REGISTER")) register_firmware_host_functions();
    mta_set_action_observer(NULL, NULL);
    mta_set_error_reporter(reporter, NULL);
    mta_set_config_save_handler(config_save, NULL);

    char* ptext = read_file(cJSON_GetObjectItem(scenario, "plugin")->valuestring);
    cJSON* pjson = ptext ? cJSON_Parse(ptext) : NULL;
    if (!pjson) {
        printf("LOAD failed\n");
        return 1;
    }

    mta_plugin_t* plugin = NULL;
    mta_load_plugin(&plugin, pjson);
    cJSON_Delete(pjson);
    if (!plugin) {
        printf("LOAD failed\n");
        return 1;
    }

    _scope = mta_scope_create();
    mta_init_scope_from_plugin_vars(plugin, _scope);
    _plugin = plugin;
    printf("LOAD %s\n", mta_plugin_get_name(plugin) ? mta_plugin_get_name(plugin) : "");

    cJSON* config = cJSON_GetObjectItem(scenario, "config");
    if (config) mta_plugin_set_config(plugin, config);

    mta_set_action_observer(observer, NULL);

    cJSON* step;
    cJSON_ArrayForEach(step, cJSON_GetObjectItem(scenario, "steps")) {
        const char* op = cJSON_GetArrayItem(step, 0)->valuestring;

        if (!strcmp(op, "event") || !strcmp(op, "call")) {
            const char* name = cJSON_GetArrayItem(step, 1)->valuestring;
            mta_arg_t args[16];
            uint8_t n = build_args(cJSON_GetArrayItem(step, 2), args);
            mta_result_t r = { 0 };
            printf("%s %s\n", !strcmp(op, "event") ? "FIRE" : "CALL", name);
            int ret = !strcmp(op, "event")
                ? mta_event_invoke_with_args_scope(plugin, name, args, n, _scope, &r)
                : mta_call_plugin_function_scope(plugin, name, args, n, _scope, &r);
            print_result(ret, &r);
            mta_result_free(&r);
        } else if (!strcmp(op, "globals")) {
            cJSON* g;
            cJSON_ArrayForEach(g, cJSON_GetArrayItem(step, 1)) {
                const char* key = g->valuestring;
                mta_arg_t* v = NULL;
                for (uint16_t i = 0; i < plugin->strings.count; i++) {
                    if (plugin->strings.strings[i] && !strcmp(plugin->strings.strings[i], key)) {
                        v = mta_scope_get_var(_scope, i);
                        break;
                    }
                }
                if (!v) printf("GLOBAL %s=<none>\n", key);
                else if (v->type == MTA_ARG_INT) printf("GLOBAL %s=i:%d\n", key, v->val.i);
                else if (v->type == MTA_ARG_FLOAT) printf("GLOBAL %s=i:%d\n", key, (int32_t)v->val.f);
                else if (v->type == MTA_ARG_STRING_REF) {
                    const char* s = mta_string_get(&plugin->strings, v->val.str);
                    if (s) printf("GLOBAL %s=s:\"%s\"\n", key, s);
                    else printf("GLOBAL %s=<none>\n", key);
                } else printf("GLOBAL %s=<none>\n", key);
            }
        } else if (!strcmp(op, "advance")) {
            _clock += cJSON_GetArrayItem(step, 1)->valueint;
        } else if (!strcmp(op, "config")) {
            mta_plugin_set_config(plugin, cJSON_GetArrayItem(step, 1));
            printf("CONFIG set\n");
        }
    }

    mta_set_action_observer(NULL, NULL);
    mta_scope_destroy(_scope);
    mta_plugin_free(plugin);
    free(plugin);
    cJSON_Delete(scenario);
    free(text);
    free(ptext);
    return 0;
}
