/**
 * Plugin lifecycle and runtime-wide registration.
 */
#include "mt_actions_internal.h"
#include <stdlib.h>
#include <string.h>

static mta_config_save_handler_t _config_save_handler = NULL;
static void* _config_save_data = NULL;

void mta_set_config_save_handler(mta_config_save_handler_t handler, void* user_data) {
    _config_save_handler = handler;
    _config_save_data = user_data;
}

bool mta_invoke_config_save(mta_plugin_t* plugin) {
    if (_config_save_handler == NULL) return true;
    return _config_save_handler(plugin, _config_save_data);
}

void mta_plugin_init(mta_plugin_t* plugin) {
    if (plugin == NULL) return;

    plugin->events = NULL;
    plugin->event_count = 0;
    plugin->event_capacity = 0;
    plugin->functions = NULL;
    plugin->function_count = 0;
    plugin->function_capacity = 0;

    mta_hash_table_init(&plugin->event_index);
    mta_hash_table_init(&plugin->function_index);
    mta_string_pool_init(&plugin->strings);

    plugin->strings.strings = calloc(8, sizeof(char*));
    plugin->strings.capacity = 8;

    plugin->base_string_count = 0;
    plugin->display_name = 0;
    plugin->match = NULL;
    plugin->reserved = NULL;
    plugin->globals = NULL;

#if MTA_HAS_RTOS
    plugin->lock = xSemaphoreCreateMutex();
#endif
}

static void free_var_list(mta_var_def_t* var) {
    while (var != NULL) {
        mta_var_def_t* next = var->next;
        free(var);
        var = next;
    }
}

static void free_action_list(mta_action_t* actions, uint16_t count) {
    if (actions == NULL) return;

    for (uint16_t i = 0; i < count; i++) {
        mta_free_action(&actions[i]);
    }

    free(actions);
}

void mta_plugin_free(mta_plugin_t* plugin) {
    if (plugin == NULL) return;

    if (plugin->events != NULL) {
        for (uint16_t i = 0; i < plugin->event_count; i++) {
            mta_event_def_t* evt = &plugin->events[i];
            free_action_list(evt->actions, evt->action_count);
            free_var_list(evt->locals);
            if (evt->params != NULL) free(evt->params);
        }

        free(plugin->events);
    }

    if (plugin->functions != NULL) {
        for (uint16_t i = 0; i < plugin->function_count; i++) {
            mta_function_def_t* fn = &plugin->functions[i];
            free_action_list(fn->actions, fn->action_count);
            free_var_list(fn->locals);
            if (fn->params != NULL) free(fn->params);
        }

        free(plugin->functions);
    }

    mta_hash_table_free(&plugin->event_index);
    mta_hash_table_free(&plugin->function_index);
    mta_string_pool_free(&plugin->strings);

#if MTA_HAS_RTOS
    if (plugin->lock != NULL) {
        vSemaphoreDelete(plugin->lock);
        plugin->lock = NULL;
    }
#endif

    free(plugin->match);
    free(plugin->name);
    free(plugin->type);
    free(plugin->description);

    if (plugin->permissions != NULL) cJSON_Delete(plugin->permissions);
    if (plugin->config != NULL) cJSON_Delete(plugin->config);
    if (plugin->config_schema != NULL) cJSON_Delete(plugin->config_schema);

    free_var_list(plugin->globals);
}

void mta_free_action(mta_action_t* action) {
    if (action == NULL) return;

    mta_condition_t* cond = action->condition;

    if (cond != NULL) {
        free_action_list(cond->all, cond->all_count);
        free_action_list(cond->any, cond->any_count);
        free_action_list(cond->none, cond->none_count);
        free_action_list(cond->then_actions, cond->then_count);
        free_action_list(cond->else_actions, cond->else_count);
        free(cond);
        action->condition = NULL;
    }

    if (action->args != NULL) {
        free(action->args);
        action->args = NULL;
    }
}

void mta_init_builtins(void) {
    mta_register_system_function("eq", mta_builtin_eq, NULL);
    mta_register_system_function("neq", mta_builtin_neq, NULL);
    mta_register_system_function("gt", mta_builtin_gt, NULL);
    mta_register_system_function("lt", mta_builtin_lt, NULL);
    mta_register_system_function("gte", mta_builtin_gte, NULL);
    mta_register_system_function("lte", mta_builtin_lte, NULL);
    mta_register_system_function("set", mta_builtin_set, NULL);
    mta_register_system_function("add", mta_builtin_add, NULL);
    mta_register_system_function("sub", mta_builtin_sub, NULL);
    mta_register_system_function("inc", mta_builtin_inc, NULL);
    mta_register_system_function("dec", mta_builtin_dec, NULL);
    mta_register_system_function("strcmp", mta_builtin_strcmp, NULL);
    mta_register_system_function("strlen", mta_builtin_strlen, NULL);
    mta_register_system_function("charat", mta_builtin_charat, NULL);
    mta_register_system_function("getbyte", mta_builtin_getbyte, NULL);
    mta_register_system_function("setbyte", mta_builtin_setbyte, NULL);
    mta_register_system_function("mul", mta_builtin_mul, NULL);
    mta_register_system_function("div", mta_builtin_div, NULL);
    mta_register_system_function("mod", mta_builtin_mod, NULL);
    mta_register_system_function("round", mta_builtin_round, NULL);
    mta_register_system_function("chr", mta_builtin_chr, NULL);
    mta_register_system_function("to_string", mta_builtin_toString, NULL);
    mta_register_system_function("concat", mta_builtin_concat, NULL);
    mta_register_system_function("get_config", mta_builtin_getConfig, NULL);
    mta_register_system_function("set_config", mta_builtin_setConfig, NULL);
}
