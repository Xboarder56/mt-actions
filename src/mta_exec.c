/**
 * Interpreter: action execution, variable resolution, host dispatch and event invocation.
 */
#include "mt_actions_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG __attribute__((unused)) = "mt-actions:exec";

static mta_action_observer_t _action_observer = NULL;
static void* _action_observer_data = NULL;

static mta_unresolved_function_handler_t _unresolved_handler = NULL;
static void* _unresolved_handler_data = NULL;

static mta_error_reporter_t _error_reporter = NULL;
static void* _error_reporter_data = NULL;

static mta_system_function_entry_t* _system_functions = NULL;
static uint16_t _system_function_count = 0;
static uint16_t _system_function_capacity = 0;

static mta_event_entry_t* _events = NULL;
static uint16_t _event_count = 0;
static uint16_t _event_capacity = 0;

void mta_set_action_observer(mta_action_observer_t observer, void* user_data) {
    _action_observer = observer;
    _action_observer_data = user_data;
}

void mta_set_unresolved_function_handler(
    mta_unresolved_function_handler_t handler, void* user_data
) {
    _unresolved_handler = handler;
    _unresolved_handler_data = user_data;
}

void mta_set_error_reporter(mta_error_reporter_t reporter, void* user_data) {
    _error_reporter = reporter;
    _error_reporter_data = user_data;
}

static void notify(mta_plugin_t* plugin, mta_action_event_t event, const char* name, int value) {
    if (_action_observer == NULL) return;
    _action_observer(plugin, event, name ? name : "", value, _action_observer_data);
}

const char* mta_runtime_error_kind_str(mta_runtime_error_kind_t kind) {
    switch (kind) {
    case MTA_RT_ERR_VAR_NOT_SET: return "var-not-set";
    case MTA_RT_ERR_CYCLE_DETECTED: return "cycle-detected";
    case MTA_RT_ERR_MISSING_RETURN: return "missing-return";
    case MTA_RT_ERR_ARG_COUNT_MISMATCH: return "arg-count-mismatch";
    case MTA_RT_ERR_MISSING_ARG: return "missing-arg";
    case MTA_RT_ERR_UNKNOWN_ARG: return "unknown-arg";
    case MTA_RT_ERR_TYPE_MISMATCH: return "type-mismatch";
    case MTA_RT_ERR_HOST_DISPATCH_FAILED: return "host-dispatch-failed";
    default: return "unknown";
    }
}

int mta_raise(mta_plugin_t* plugin, mta_runtime_error_kind_t kind, const char* fmt, ...) {
    char msg[256];

    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        int ret = vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        if (ret < 0) msg[0] = '\0';
    } else {
        const char* str = mta_runtime_error_kind_str(kind);
        size_t len = strlen(str);
        if (len > sizeof(msg) - 1) len = sizeof(msg) - 1;
        memcpy(msg, str, len);
        msg[len] = '\0';
    }

    notify(plugin, MTA_ACTION_ERROR, msg, kind);

    if (_error_reporter != NULL) {
        _error_reporter(plugin, msg, kind, _error_reporter_data);
    }

    return -1;
}

/**
 * Resolve the pool id of the variable named by a "$name" string id. Returns 0 if invalid.
 */
static uint16_t var_ref_id(mta_plugin_t* plugin, uint16_t str) {
    const char* name = mta_string_get(&plugin->strings, str);
    if (name == NULL) return 0;
    if (name[0] == '$') name++;
    if (name[0] == '\0') return 0;
    return mta_string_intern(&plugin->strings, name);
}

static int find_function(mta_plugin_t* plugin, const char* name) {
    if (plugin == NULL || name == NULL) return -1;

    for (uint16_t i = 0; i < plugin->function_count; i++) {
        const char* fn_name = mta_string_get(&plugin->strings, plugin->functions[i].name);
        if (fn_name != NULL && !strcmp(fn_name, name)) return i;
    }

    return -1;
}

static int find_event(mta_plugin_t* plugin, const char* name) {
    if (plugin == NULL || name == NULL) return -1;

    for (uint16_t i = 0; i < plugin->event_count; i++) {
        const char* evt_name = mta_string_get(&plugin->strings, plugin->events[i].name);
        if (evt_name != NULL && !strcmp(evt_name, name)) return i;
    }

    return -1;
}

int32_t mta_arg_as_int(mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* arg) {
    uint16_t chain[MTA_VAR_REF_MAX_HOPS];
    int depth = 0;

    while (arg != NULL && arg->type == MTA_ARG_VAR_REF) {
        arg = mta_resolve_var_ref_hop(plugin, scope, arg, chain, &depth);
    }

    if (arg == NULL) return 0;

    switch (arg->type) {
    case MTA_ARG_INT: return arg->val.i;
    case MTA_ARG_FLOAT: return (int32_t)arg->val.f;
    case MTA_ARG_STRING_REF: {
        const char* str = mta_string_get(&plugin->strings, arg->val.str);
        if (str == NULL || str[0] == '\0') return 0;
        if (str[1] == '\0') return (unsigned char)str[0];
        return atoi(str);
    }
    default: return 0;
    }
}

/**
 * Follow one "$name" hop, recording the chain for cycle detection.
 */
mta_arg_t* mta_resolve_var_ref_hop(
    mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* ref, uint16_t* chain, int* depth
) {
    uint16_t id = var_ref_id(plugin, ref->val.str);
    if (id == 0) return NULL;

    int count = *depth;

    for (int i = 0; i < count; i++) {
        if (chain[i] != id) continue;

        char path[200];
        size_t len = 0;

        for (int j = 0; j < count && len < sizeof(path); j++) {
            const char* name = mta_string_get(&plugin->strings, chain[j]);
            int ret = snprintf(
                path + len, sizeof(path) - len, "%s$%s", j ? " -> " : "", name ? name : "?"
            );
            if (ret < 0) break;
            len += ret;
        }

        if (len < sizeof(path)) {
            const char* name = mta_string_get(&plugin->strings, id);
            snprintf(path + len, sizeof(path) - len, " -> $%s", name ? name : "?");
        }

        mta_raise(plugin, MTA_RT_ERR_CYCLE_DETECTED, "variable reference cycle: %s", path);
        return NULL;
    }

    if (count >= MTA_VAR_REF_MAX_HOPS) {
        const char* name = mta_string_get(&plugin->strings, id);
        mta_raise(
            plugin,
            MTA_RT_ERR_CYCLE_DETECTED,
            "variable reference chain exceeded %d hops at $%s",
            MTA_VAR_REF_MAX_HOPS,
            name ? name : "?"
        );
        return NULL;
    }

    chain[count] = id;
    *depth = count + 1;
    return mta_scope_get_var(scope, id);
}

/**
 * Follow variable references until a concrete value is reached. Raises (with `context` as a
 * message prefix) if a reference is malformed or names an unset variable.
 */
mta_arg_t* _mta_resolve_var_ref_chain(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const mta_arg_t* arg,
    uint16_t* chain,
    int* depth,
    const char* context
) {
    uint16_t id = 0;
    int before = 0;

    if (arg == NULL) return NULL;

    do {
        if (arg->type != MTA_ARG_VAR_REF) return (mta_arg_t*)arg;

        id = var_ref_id(plugin, arg->val.str);

        if (id == 0) {
            mta_raise(
                plugin,
                MTA_RT_ERR_VAR_NOT_SET,
                "%s%sinvalid variable reference",
                context ? context : "",
                context ? ": " : ""
            );
            return NULL;
        }

        before = *depth;
        arg = mta_resolve_var_ref_hop(plugin, scope, arg, chain, depth);
    } while (arg != NULL);

    // A hop was recorded but the variable is not in scope (cycles raise inside the hop).
    if (before < *depth) {
        const char* name = mta_string_get(&plugin->strings, id);
        mta_raise(
            plugin,
            MTA_RT_ERR_VAR_NOT_SET,
            "%s%sreferences variable '$%s' not set in scope",
            context ? context : "",
            context ? " " : "",
            name ? name : "?"
        );
    }

    return NULL;
}

const char* mta_arg_as_string(mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* arg) {
    uint16_t chain[MTA_VAR_REF_MAX_HOPS];
    int depth = 0;

    while (arg != NULL && arg->type == MTA_ARG_VAR_REF) {
        arg = mta_resolve_var_ref_hop(plugin, scope, arg, chain, &depth);
    }

    if (arg == NULL || arg->type != MTA_ARG_STRING_REF) return "";

    const char* str = mta_string_get(&plugin->strings, arg->val.str);
    return str ? str : "";
}

void mta_register_system_function(
    const char* fn_name, mta_system_function_t fn, const char* permission
) {
    if (fn_name == NULL || fn == NULL) return;

    for (uint16_t i = 0; i < _system_function_count; i++) {
        if (!strcmp(_system_functions[i].name, fn_name)) {
            _system_functions[i].fn = fn;
            _system_functions[i].permission = permission;
            return;
        }
    }

    if (_system_function_count == _system_function_capacity) {
        uint16_t capacity = _system_function_count ? _system_function_count * 2 : 8;
        mta_system_function_entry_t* entries =
            realloc(_system_functions, capacity * sizeof(mta_system_function_entry_t));
        if (entries == NULL) return;
        _system_functions = entries;
        _system_function_capacity = capacity;
    }

    mta_system_function_entry_t* entry = &_system_functions[_system_function_count++];
    entry->name = fn_name;
    entry->fn = fn;
    entry->permission = permission;
}

void mta_register_event(const char* event_name, const char* permission) {
    if (event_name == NULL) return;

    for (uint16_t i = 0; i < _event_count; i++) {
        if (!strcmp(_events[i].name, event_name)) {
            _events[i].permission = permission;
            return;
        }
    }

    if (_event_count == _event_capacity) {
        uint16_t capacity = _event_count ? _event_count * 2 : 8;
        mta_event_entry_t* entries = realloc(_events, capacity * sizeof(mta_event_entry_t));
        if (entries == NULL) return;
        _events = entries;
        _event_capacity = capacity;
    }

    _events[_event_count].name = event_name;
    _events[_event_count].permission = permission;
    _event_count++;
}

bool mta_check_permission(mta_plugin_t* plugin, const char* permission) {
    if (permission == NULL) return true;
    if (plugin == NULL) return false;

    cJSON* permissions = mta_plugin_get_permissions(plugin);
    if (permissions == NULL || !cJSON_IsArray(permissions)) return false;

    // "group:action" is also granted by "group:*".
    const char* colon = strchr(permission, ':');
    size_t group_len = colon ? (size_t)(colon - permission) : strlen(permission);

    for (cJSON* item = permissions->child; item != NULL; item = item->next) {
        if (!cJSON_IsString(item)) continue;
        const char* granted = item->valuestring;

        if (!strcmp(granted, permission)) return true;
        if (!strncmp(granted, permission, group_len) && !strcmp(granted + group_len, ":*")) {
            return true;
        }
    }

    return false;
}

void mta_init_scope_from_plugin_vars(mta_plugin_t* plugin, mta_scope_t* scope) {
    if (plugin == NULL || scope == NULL) return;

    for (mta_var_def_t* var = plugin->globals; var != NULL; var = var->next) {
        const char* bracket = strchr(var->name, '[');

        if (bracket == NULL) {
            uint16_t id = mta_string_intern(&plugin->strings, var->name);
            mta_arg_t value = { .name = id, .type = var->type };
            if (var->type == MTA_ARG_STRING_REF) {
                value.val.str = var->str;
            } else {
                value.val.i = var->value.i;
            }
            mta_scope_set_var(scope, id, &value);
            continue;
        }

        char name[128];
        size_t len = bracket - var->name;
        if (len >= sizeof(name)) continue;
        memcpy(name, var->name, len);
        name[len] = '\0';

        int capacity = atoi(bracket + 1);
        if (capacity < 1 || capacity > MTA_ARRAY_MAX_CAPACITY) capacity = MTA_ARRAY_DEFAULT_CAPACITY;

        mta_array_t* array = calloc(1, sizeof(mta_array_t));
        if (array == NULL) continue;

        array->data = calloc(capacity, sizeof(int32_t));
        array->capacity = capacity;
        array->length = 0;

        if (array->data == NULL) {
            free(array);
            continue;
        }

        mta_scope_register_owned_array(scope, array);

        uint16_t id = mta_string_intern(&plugin->strings, name);
        mta_arg_t value = { .name = id, .type = MTA_ARG_ARRAY, .val.arr = array };
        mta_scope_set_var(scope, id, &value);
    }
}

int mta_call_plugin_function_scope(
    mta_plugin_t* plugin,
    const char* fn_name,
    mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* caller_scope,
    mta_result_t* result
) {
    if (plugin != NULL && fn_name != NULL) {
        for (uint16_t i = 0; i < plugin->function_count; i++) {
            mta_function_def_t* fn = &plugin->functions[i];
            const char* name = mta_string_get(&plugin->strings, fn->name);
            if (name == NULL || strcmp(name, fn_name)) continue;

            mta_scope_t scope;
            mta_scope_init(&scope);
            mta_init_scope_from_local_vars(plugin, fn->locals, &scope);
            mta_init_scope_from_plugin_vars(plugin, &scope);
            scope.user_data = caller_scope ? caller_scope->user_data : NULL;
            scope.parent = caller_scope;

            int ret = 0;

            if (mta_bind_positional_args(
                    plugin, &scope, fn->params, fn->param_count, args, arg_count, NULL, fn_name
                ) != 0) {
                ret = -1;
            } else {
                for (uint16_t a = 0; a < fn->action_count; a++) {
                    ret = mta_execute_action(plugin, &fn->actions[a], &scope);
                    if (ret != 0) {
                        if (ret == 1) ret = 0;
                        break;
                    }
                }
            }

            mta_capture_result(plugin, &scope, result);
            mta_scope_free(&scope);
            mta_string_pool_truncate(&plugin->strings, plugin->base_string_count);
            return ret;
        }
    }

    if (result != NULL) {
        result->type = MTA_ARG_INT;
        result->val.i = 0;
        result->status = -1;
    }

    return -1;
}

void mta_init_scope_from_local_vars(mta_plugin_t* plugin, mta_var_def_t* vars, mta_scope_t* scope) {
    if (scope == NULL) return;

    for (mta_var_def_t* var = vars; var != NULL; var = var->next) {
        const char* bracket = strchr(var->name, '[');

        if (bracket == NULL) {
            uint16_t id = mta_string_intern(&plugin->strings, var->name);
            mta_arg_t value = { .name = id, .type = var->type };
            if (var->type == MTA_ARG_STRING_REF) {
                value.val.str = var->str;
            } else {
                value.val.i = var->value.i;
            }
            mta_scope_set_var(scope, id, &value);
            continue;
        }

        char name[128];
        size_t len = bracket - var->name;
        if (len >= sizeof(name)) continue;
        memcpy(name, var->name, len);
        name[len] = '\0';

        int capacity = atoi(bracket + 1);
        if (capacity < 1 || capacity > MTA_ARRAY_MAX_CAPACITY) capacity = MTA_ARRAY_DEFAULT_CAPACITY;

        mta_array_t* array = calloc(1, sizeof(mta_array_t));
        if (array == NULL) continue;

        array->data = calloc(capacity, sizeof(int32_t));
        array->capacity = capacity;
        array->length = 0;

        if (array->data == NULL) {
            free(array);
            continue;
        }

        mta_scope_register_owned_array(scope, array);

        uint16_t id = mta_string_intern(&plugin->strings, name);
        mta_arg_t value = { .name = id, .type = MTA_ARG_ARRAY, .val.arr = array };
        mta_scope_set_var(scope, id, &value);
    }
}

/**
 * Bind call arguments to a callee's positional parameter names. "$var" arguments are resolved
 * in the caller scope (when given) at bind time.
 */
int mta_bind_positional_args(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const uint16_t* params,
    uint8_t param_count,
    const mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* caller_scope,
    const char* fn_name
) {
    if (plugin == NULL || scope == NULL) return -1;

    uint8_t count = param_count < arg_count ? param_count : arg_count;

    for (uint8_t i = 0; i < count; i++) {
        if (params == NULL || params[i] == 0) continue;

        mta_arg_t value = args[i];

        if (value.type == MTA_ARG_VAR_REF) {
            mta_arg_t* resolved = NULL;

            if (caller_scope != NULL) {
                const char* name = mta_string_get(&plugin->strings, value.val.str);
                if (name != NULL && name[0] == '$') name++;
                if (name != NULL && name[0] != '\0') {
                    resolved =
                        mta_scope_get_var(caller_scope, mta_string_intern(&plugin->strings, name));
                }
            }

            if (resolved == NULL) {
                const char* name = mta_string_get(&plugin->strings, args[i].val.str);
                if (name != NULL && name[0] == '$') name++;
                const char* param = mta_string_get(&plugin->strings, params[i]);

                return mta_raise(
                    plugin,
                    MTA_RT_ERR_VAR_NOT_SET,
                    "argument '%s' references variable '$%s' not set in caller scope when calling "
                    "%s",
                    param ? param : "?",
                    (name != NULL && name[0] != '\0') ? name : "?",
                    fn_name ? fn_name : "?"
                );
            }

            value = *resolved;
        }

        value.name = params[i];
        mta_scope_set_var(scope, params[i], &value);
    }

    return 0;
}

/**
 * Render call arguments for debug logging: `a=1, $x=2.5, "str"`.
 */
static void format_args(
    mta_plugin_t* plugin, mta_scope_t* scope, const mta_action_t* action, char* buf, int size
) {
    int len = 0;
    buf[0] = '\0';

    if (action->args == NULL || action->arg_count == 0) return;

    for (uint8_t i = 0; i < action->arg_count && len < size - 2; i++) {
        const mta_arg_t* arg = &action->args[i];
        const char* name = NULL;
        int ret;

        if (i != 0 && len != size - 2) {
            buf[len++] = ',';
            buf[len++] = ' ';
        }

        if (arg->name != 0) {
            name = mta_string_get(&plugin->strings, arg->name);
            if (name != NULL && name[0] != '\0' && len < size - 1) {
                ret = snprintf(buf + len, size - len, "%s=", name);
                if (ret < 0 || ret > size - 1 - len) {
                    len = size - 1;
                    break;
                }
                len += ret;
            }
        }

        switch (arg->type) {
        case MTA_ARG_INT: ret = snprintf(buf + len, size - len, "%d", arg->val.i); break;
        case MTA_ARG_FLOAT: ret = snprintf(buf + len, size - len, "%g", (double)arg->val.f); break;
        case MTA_ARG_STRING_REF: {
            const char* str = mta_string_get(&plugin->strings, arg->val.str);
            ret = snprintf(buf + len, size - len, "\"%s\"", str ? str : "");
            break;
        }
        case MTA_ARG_VAR_REF: {
            const char* ref = mta_string_get(&plugin->strings, arg->val.str);
            ret = 0;

            if ((name == NULL || name[0] == '\0') && len < size - 1) {
                ret = snprintf(buf + len, size - len, "%s", ref ? ref : "$?");
                if (ret < 0 || ret > size - 1 - len) {
                    len = size - 1;
                    ret = 0;
                } else {
                    len += ret;
                    ret = 0;
                }
            }

            if (ref != NULL && ref[0] == '$') ref++;

            if (scope != NULL && len < size - 1 && ref != NULL) {
                mta_arg_t* value =
                    mta_scope_get_var(scope, mta_string_intern(&plugin->strings, ref));

                if (value == NULL) {
                    ret = snprintf(buf + len, size - len, "=<undef>");
                } else if (value->type == MTA_ARG_INT) {
                    ret = snprintf(buf + len, size - len, "=%d", value->val.i);
                } else if (value->type == MTA_ARG_FLOAT) {
                    ret = snprintf(buf + len, size - len, "=%g", (double)value->val.f);
                } else if (value->type == MTA_ARG_STRING_REF) {
                    const char* str = mta_string_get(&plugin->strings, value->val.str);
                    ret = snprintf(buf + len, size - len, "=\"%s\"", str ? str : "");
                } else {
                    ret = snprintf(buf + len, size - len, "=?");
                }
            }
            break;
        }
        default: ret = snprintf(buf + len, size - len, "?"); break;
        }

        if (ret < 0 || ret > size - 1 - len) {
            len = size - 1;
        } else {
            len += ret;
        }
    }

    buf[len < size - 1 ? len : size - 1] = '\0';
}

/**
 * Evaluate a condition's all / any / none blocks. Returns true if the condition failed.
 */
static inline __attribute__((always_inline)) bool
condition_failed(mta_plugin_t* plugin, mta_condition_t* cond, mta_scope_t* scope) {
    int all_result = 0;
    bool any_failed = false;
    bool none_matched = false;

    if (cond->all != NULL) {
        for (uint16_t i = 0; i < cond->all_count; i++) {
            all_result = mta_execute_action(plugin, &cond->all[i], scope);
            if (all_result != 0) break;
        }
    }

    if (cond->any != NULL) {
        any_failed = true;
        for (uint16_t i = 0; i < cond->any_count; i++) {
            if (mta_execute_action(plugin, &cond->any[i], scope) == 0) {
                any_failed = false;
                break;
            }
        }
    }

    if (cond->none != NULL) {
        for (uint16_t i = 0; i < cond->none_count; i++) {
            if (mta_execute_action(plugin, &cond->none[i], scope) == 0) {
                none_matched = true;
                break;
            }
        }
    }

    if (all_result != 0 && cond->all != NULL) return true;
    if (any_failed && cond->any != NULL) return true;
    return none_matched && cond->none != NULL;
}

static int run_actions(
    mta_plugin_t* plugin, mta_action_t* actions, uint16_t count, mta_scope_t* scope
) {
    if (actions == NULL) return 0;

    for (uint16_t i = 0; i < count; i++) {
        int ret = mta_execute_action(plugin, &actions[i], scope);
        if (ret != 0) return ret;
    }

    return 0;
}

static int execute_condition(mta_plugin_t* plugin, mta_condition_t* cond, mta_scope_t* scope) {
    if (cond->type == MTA_CONDITION_WHILE) {
        for (int iteration = 0; iteration < MTA_WHILE_MAX_ITERATIONS; iteration++) {
            notify(plugin, MTA_ACTION_LOOP, "while", iteration);
            if (iteration == 0) mta_dump_scope(plugin, scope, "Before while condition");

            bool stop = condition_failed(plugin, cond, scope);
            notify(plugin, MTA_ACTION_CONDITION, "while", !stop);
            if (stop) break;

            int ret = run_actions(plugin, cond->then_actions, cond->then_count, scope);
            if (ret != 0) return ret;
        }

        return 0;
    }

    bool failed = condition_failed(plugin, cond, scope);
    const char* label = "if";

    if (cond->type == MTA_CONDITION_UNLESS) {
        label = "unless";
        failed = !failed;
    }

    notify(plugin, MTA_ACTION_CONDITION, label, !failed);

    if (failed) return run_actions(plugin, cond->else_actions, cond->else_count, scope);
    return run_actions(plugin, cond->then_actions, cond->then_count, scope);
}

static int call_plugin_function(
    mta_plugin_t* plugin,
    mta_action_t* action,
    mta_scope_t* scope,
    const char* fn_name,
    const char* name,
    int index
) {
    mta_function_def_t* fn = &plugin->functions[index];
    mta_scope_t local;

    notify(plugin, MTA_ACTION_FUNCTION_ENTER, name, action->arg_count);

    mta_scope_init(&local);
    local.user_data = scope->user_data;
    local.parent = scope;

    mta_init_scope_from_local_vars(plugin, fn->locals, &local);

    // Callee sees the caller's variables (globals propagate back via set).
    for (uint8_t i = 0; i < scope->var_count; i++) {
        mta_scope_set_var(&local, scope->vars[i].name, &scope->vars[i]);
    }

    int ret = mta_bind_positional_args(
        plugin, &local, fn->params, fn->param_count, action->args, action->arg_count, scope, name
    );

    if (ret != 0) {
        notify(plugin, MTA_ACTION_FUNCTION_EXIT, name, ret);
        mta_scope_free(&local);
        notify(plugin, MTA_ACTION_EXIT, fn_name, ret);
        return ret;
    }

    for (uint16_t i = 0; i < fn->action_count; i++) {
        ret = mta_execute_action(plugin, &fn->actions[i], &local);

        if (ret == 1) {
            notify(plugin, MTA_ACTION_FUNCTION_EXIT, name, 0);
            break;
        }

        if (ret < 0) {
            notify(plugin, MTA_ACTION_FUNCTION_EXIT, name, ret);
            mta_scope_free(&local);
            return ret;
        }
    }

    notify(plugin, MTA_ACTION_FUNCTION_EXIT, name, 0);

    uint16_t ret_id = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
    mta_arg_t* value = mta_scope_get_var(&local, ret_id);
    if (value != NULL) {
        mta_scope_set_var(scope, action->capture ? action->capture : ret_id, value);
    }

    mta_scope_free(&local);
    notify(plugin, MTA_ACTION_EXIT, fn_name, 0);
    return 0;
}

int mta_execute_action(mta_plugin_t* plugin, mta_action_t* action, mta_scope_t* scope) {
    if (plugin == NULL || action == NULL) return -1;

    const char* fn_name = mta_string_get(&plugin->strings, action->function);
    if (fn_name == NULL) return -1;

    notify(plugin, MTA_ACTION_ENTER, fn_name, 0);

    const char* plugin_name = mta_plugin_get_name(plugin);
    char args_str[96];
    format_args(plugin, scope, action, args_str, sizeof(args_str));
    MTA_LOGD(TAG, "[%s] %s(%s)", plugin_name ? plugin_name : "?", fn_name, args_str);
    (void)plugin_name;

    // return [value]: store into "_" and unwind the current body.
    if (!strcmp(fn_name, "return")) {
        if (action->arg_count != 0) {
            uint16_t ret_id = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
            mta_arg_t* value = action->args;

            if (value->type == MTA_ARG_VAR_REF) {
                const char* name = mta_string_get(&plugin->strings, value->val.str);
                if (name != NULL && name[0] == '$') name++;
                if (name == NULL) return 1;

                value = mta_scope_get_var(scope, mta_string_intern(&plugin->strings, name));
                if (value == NULL) return 1;
                mta_arg_as_int(plugin, scope, value);
            }

            mta_scope_set_var(scope, ret_id, value);
        }

        return 1;
    }

    if (action->condition != NULL) {
        return execute_condition(plugin, action->condition, scope);
    }

    // Plugin functions ("@name" or bare name) take precedence over host functions.
    const char* name = fn_name[0] == '@' ? fn_name + 1 : fn_name;
    int index = find_function(plugin, name);
    if (index >= 0) return call_plugin_function(plugin, action, scope, fn_name, name, index);

    mta_system_function_t fn = NULL;
    bool found = false;

    for (uint16_t i = 0; i < _system_function_count; i++) {
        if (strcmp(_system_functions[i].name, fn_name)) continue;

        if (_system_functions[i].permission != NULL &&
            !mta_check_permission(plugin, _system_functions[i].permission)) {
            return -1;
        }

        fn = _system_functions[i].fn;
        found = true;
        break;
    }

    if (!found && _unresolved_handler == NULL) {
        notify(plugin, MTA_ACTION_EXIT, fn_name, -1);
        return mta_raise(plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "unresolved function '%s'", fn_name);
    }

    // Host functions report through "_"; preserve the caller's "_" when capturing elsewhere.
    bool had_returned = scope->has_returned;
    uint16_t ret_id = 0;
    bool capture_is_ret = false;
    bool saved = false;
    mta_arg_t saved_ret;

    scope->has_returned = false;

    if (action->capture != 0) {
        ret_id = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
        capture_is_ret = action->capture == ret_id;

        if (!capture_is_ret) {
            mta_arg_t* prev = mta_scope_get_var(scope, ret_id);
            if (prev != NULL) {
                saved_ret = *prev;
                saved = true;
            }
        }
    }

    int ret;

    if (found) {
        ret = fn(plugin, scope, action->args, action->arg_count);
    } else {
        ret = _unresolved_handler(
            plugin, scope, fn_name, action->args, action->arg_count, _unresolved_handler_data
        );
    }

    if (ret == 0 && !scope->has_returned) {
        ret = mta_raise(
            plugin,
            MTA_RT_ERR_MISSING_RETURN,
            "host function '%s' returned success without declaring a return value",
            fn_name
        );
        scope->has_returned = had_returned;
    } else {
        scope->has_returned = had_returned;

        if (ret == 0 && action->capture != 0) {
            if (found) mta_string_get(&plugin->strings, action->capture);

            mta_arg_t* value =
                mta_scope_get_var(scope, mta_string_intern(&plugin->strings, MTA_RETURN_VAR));

            if (value != NULL) {
                if (found) mta_arg_as_int(plugin, scope, value);
                mta_scope_set_var(scope, action->capture, value);
                if (found) mta_dump_scope(plugin, scope, "After 'to' capture (host fn)");
            }

            if (!capture_is_ret && saved) mta_scope_set_var(scope, ret_id, &saved_ret);
        }
    }

    notify(plugin, MTA_ACTION_EXIT, fn_name, ret);
    return ret;
}

void mta_capture_result(mta_plugin_t* plugin, mta_scope_t* scope, mta_result_t* result) {
    if (result == NULL) return;

    result->type = MTA_ARG_INT;
    result->val.i = 0;
    result->status = 0;

    if (scope == NULL || scope->var_count == 0) return;

    for (uint8_t i = 0; i < scope->var_count; i++) {
        const char* name = mta_string_get(&plugin->strings, scope->vars[i].name);
        if (name == NULL || strcmp(name, MTA_RETURN_VAR)) continue;

        mta_arg_t* value = &scope->vars[i];
        result->type = (mta_arg_type_t)value->type;

        if (value->type == MTA_ARG_STRING_REF) {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            result->val.s = str ? strdup(str) : NULL;
        } else {
            result->val.i = value->val.i;
        }

        break;
    }

    for (uint8_t i = 0; i < scope->var_count; i++) {
        const char* name = mta_string_get(&plugin->strings, scope->vars[i].name);
        if (name == NULL || strcmp(name, MTA_STATUS_VAR)) continue;

        result->status = scope->vars[i].val.i;
        return;
    }
}

int mta_event_invoke_with_args_scope(
    mta_plugin_t* plugin,
    const char* event,
    mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* scope,
    mta_result_t* result
) {
    int index = -1;

    if (plugin != NULL && event != NULL && scope != NULL) {
        bool permitted = true;

        for (uint16_t i = 0; i < _event_count; i++) {
            if (!strcmp(_events[i].name, event)) {
                permitted = mta_check_permission(plugin, _events[i].permission);
                break;
            }
        }

        if (permitted) index = find_event(plugin, event);
    }

    if (index < 0) {
        if (result != NULL) {
            result->type = MTA_ARG_INT;
            result->val.i = 0;
            result->status = -1;
        }

        return -1;
    }

#if MTA_HAS_RTOS
    if (plugin->lock != NULL) {
        // An event raised from inside a running handler (same task) is queued and dispatched
        // once the current handler has finished, instead of deadlocking or recursing.
        if (xSemaphoreGetMutexHolder(plugin->lock) == xTaskGetCurrentTaskHandle()) {
            if (plugin->pending_count < MTA_PENDING_EVENTS_MAX) {
                mta_pending_event_t* pending = &plugin->pending[plugin->pending_count];
                uint8_t count =
                    arg_count < MTA_PENDING_EVENT_ARGS_MAX ? arg_count : MTA_PENDING_EVENT_ARGS_MAX;

                strncpy(pending->name, event, sizeof(pending->name) - 1);
                pending->name[sizeof(pending->name) - 1] = '\0';

                for (uint8_t i = 0; i < count; i++) {
                    pending->args[i] = args[i];
                }

                pending->arg_count = count;
                pending->scope = scope;
                plugin->pending_count++;
            } else {
                MTA_LOGW(TAG, "pending event queue full, dropping re-entrant event '%s'", event);
            }

            if (result != NULL) {
                result->type = MTA_ARG_INT;
                result->val.i = 0;
                result->status = -1;
            }

            return 0;
        }

        xSemaphoreTake(plugin->lock, portMAX_DELAY);
    }
#endif

    mta_event_def_t* evt = &plugin->events[index];
    int ret = 0;

    notify(plugin, MTA_ACTION_EVENT_ENTER, event, 0);
    mta_init_scope_from_local_vars(plugin, evt->locals, scope);

    if (evt->param_count != 0 &&
        mta_bind_positional_args(
            plugin, scope, evt->params, evt->param_count, args, arg_count, NULL, event
        ) != 0) {
        ret = -1;
    } else {
        for (uint16_t i = 0; i < evt->action_count; i++) {
            int r = mta_execute_action(plugin, &evt->actions[i], scope);
            if (r == 1) break;
            if (r != 0) {
                ret = r;
                break;
            }
        }
    }

    mta_capture_result(plugin, scope, result);

    // Strings interned during the handler are released below; drop references to them from the
    // persistent scope so they cannot dangle.
    for (uint8_t i = 0; i < scope->var_count; i++) {
        mta_arg_t* var = &scope->vars[i];
        if (var->type == MTA_ARG_STRING_REF && var->val.str >= plugin->base_string_count) {
            var->type = MTA_ARG_INT;
            var->val.i = 0;
        }
    }

    mta_string_pool_truncate(&plugin->strings, plugin->base_string_count);

#if MTA_HAS_RTOS
    if (plugin->lock != NULL) xSemaphoreGive(plugin->lock);

    while (plugin->pending_count != 0) {
        mta_pending_event_t pending = plugin->pending[0];
        plugin->pending_count--;
        memmove(
            &plugin->pending[0],
            &plugin->pending[1],
            plugin->pending_count * sizeof(mta_pending_event_t)
        );
        mta_event_invoke_with_args_scope(
            plugin, pending.name, pending.args, pending.arg_count, pending.scope, NULL
        );
    }
#endif

    notify(plugin, MTA_ACTION_EVENT_EXIT, event, ret);
    return ret;
}

void mta_result_free(mta_result_t* result) {
    if (result == NULL) return;

    if (result->type == MTA_ARG_STRING_REF && result->val.s != NULL) {
        free(result->val.s);
        result->val.s = NULL;
    }
}
