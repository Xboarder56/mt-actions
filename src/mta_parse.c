/**
 * Plugin JSON loading: metadata, config schema, variables, functions, events and action lists.
 */
#include "mt_actions_internal.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG __attribute__((unused)) = "mt-actions:parse";

/**
 * Append variable declarations to plugin->globals.
 *
 * Accepts either an array of names (initialized to 0) or an object of name: initial value.
 */
void mta_load_variables(mta_plugin_t* plugin, cJSON* vars) {
    if (plugin == NULL || vars == NULL) return;

    mta_var_def_t* tail = plugin->globals;
    while (tail != NULL && tail->next != NULL) tail = tail->next;

    if (cJSON_IsArray(vars)) {
        for (cJSON* item = vars->child; item != NULL; item = item->next) {
            if (!cJSON_IsString(item) || item->valuestring == NULL) continue;

            mta_var_def_t* var = malloc(sizeof(mta_var_def_t) + strlen(item->valuestring) + 1);
            if (var == NULL) return;

            strcpy(var->name, item->valuestring);
            var->next = NULL;
            var->type = MTA_ARG_INT;
            var->value.i = 0;
            var->str = 0;

            if (tail == NULL) {
                plugin->globals = var;
            } else {
                tail->next = var;
            }

            tail = var;
        }
    } else if (cJSON_IsObject(vars)) {
        for (cJSON* item = vars->child; item != NULL; item = item->next) {
            if (item->string == NULL) continue;

            mta_var_def_t* var = malloc(sizeof(mta_var_def_t) + strlen(item->string) + 1);
            if (var == NULL) return;

            strcpy(var->name, item->string);
            var->next = NULL;
            var->type = MTA_ARG_INT;
            var->value.i = 0;
            var->str = 0;

            if (cJSON_IsNumber(item)) {
                if (item->valuedouble != (double)item->valueint) {
                    var->type = MTA_ARG_FLOAT;
                    var->value.f = (float)item->valuedouble;
                } else {
                    var->type = MTA_ARG_INT;
                    var->value.i = item->valueint;
                }
            } else if (cJSON_IsString(item)) {
                var->type = MTA_ARG_STRING_REF;
                var->str = mta_string_intern(&plugin->strings, item->valuestring);
            }

            if (tail == NULL) {
                plugin->globals = var;
            } else {
                tail->next = var;
            }

            tail = var;
        }
    }
}

static mta_match_t* load_match(mta_plugin_t* plugin, cJSON* json) {
    mta_match_t* match = calloc(1, sizeof(mta_match_t));
    if (match == NULL) return NULL;

    cJSON* item = cJSON_GetObjectItem(json, "pid");
    if (item != NULL && cJSON_IsNumber(item)) match->pid = item->valueint;

    item = cJSON_GetObjectItem(json, "vid");
    if (item != NULL && cJSON_IsNumber(item)) match->vid = item->valueint;

    item = cJSON_GetObjectItem(json, "serial");
    if (item != NULL && cJSON_IsNumber(item)) match->serial = item->valueint;

    item = cJSON_GetObjectItem(json, "ble_name");
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) {
        match->ble_name = mta_string_intern(&plugin->strings, item->valuestring);
    }

    item = cJSON_GetObjectItem(json, "ble_name_prefix");
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) {
        match->ble_name_prefix = mta_string_intern(&plugin->strings, item->valuestring);
    }

    return match;
}

/**
 * Copy the base name of a "name[N]" declaration into buf. Returns NULL if it does not fit.
 */
static const char* var_base_name(const char* name, char* buf, size_t size) {
    const char* bracket = strchr(name, '[');
    if (bracket == NULL) return name;

    size_t len = bracket - name;
    if (len >= size) return NULL;

    memcpy(buf, name, len);
    buf[len] = '\0';
    return buf;
}

void mta_load_plugin(mta_plugin_t** out, cJSON* root) {
    if (out == NULL || root == NULL) return;

    mta_plugin_t* plugin = calloc(1, sizeof(mta_plugin_t));
    if (plugin == NULL) return;

    mta_plugin_init(plugin);

    cJSON* item = cJSON_GetObjectItem(root, "display_name");

    // String id 0 is always "", so a 0 id means "unset".
    mta_string_intern(&plugin->strings, "");

    const char* display_name = "<untitled>";
    if (cJSON_IsString(item) && item->valuestring != NULL) display_name = item->valuestring;
    plugin->display_name = mta_string_intern(&plugin->strings, display_name);

    item = cJSON_GetObjectItem(root, "name");
    if (item != NULL && cJSON_IsString(item)) plugin->name = strdup(item->valuestring);

    item = cJSON_GetObjectItem(root, "type");
    if (item != NULL && cJSON_IsString(item)) plugin->type = strdup(item->valuestring);

    item = cJSON_GetObjectItem(root, "description");
    if (item != NULL && cJSON_IsString(item)) plugin->description = strdup(item->valuestring);

    item = cJSON_GetObjectItem(root, "permissions");
    if (item != NULL && cJSON_IsArray(item)) plugin->permissions = cJSON_Duplicate(item, 1);

    item = cJSON_GetObjectItem(root, "config");
    if (item != NULL && cJSON_IsObject(item)) plugin->config_schema = cJSON_Duplicate(item, 1);

    item = cJSON_GetObjectItem(root, "match");
    if (item != NULL) {
        mta_match_t* match = load_match(plugin, item);
        if (match != NULL) plugin->match = match;
    }

    item = cJSON_GetObjectItem(root, "functions");
    if (item != NULL && cJSON_IsObject(item)) {
        uint16_t count = 0;
        for (cJSON* fn = item->child; fn != NULL; fn = fn->next) count++;

        if (count != 0) {
            plugin->functions = calloc(count, sizeof(mta_function_def_t));

            if (plugin->functions == NULL) {
                plugin->function_count = 0;
                plugin->function_capacity = 0;
            } else {
                plugin->function_count = count;
                plugin->function_capacity = count;

                uint16_t i = 0;
                for (cJSON* fn = item->child; fn != NULL; fn = fn->next, i++) {
                    mta_function_def_t* def = &plugin->functions[i];
                    def->name = mta_string_intern(&plugin->strings, fn->string);
                    def->reserved = 0;
                    mta_parse_callable_body(
                        plugin,
                        fn,
                        &def->actions,
                        &def->action_count,
                        &def->locals,
                        &def->params,
                        &def->param_count
                    );
                }
            }
        }
    }

    item = cJSON_GetObjectItem(root, "variables");
    if (item != NULL) mta_load_variables(plugin, item);

    item = cJSON_GetObjectItem(root, "events");
    if (item != NULL && cJSON_IsObject(item)) {
        uint16_t count = 0;
        for (cJSON* evt = item->child; evt != NULL; evt = evt->next) count++;

        if (count != 0) {
            plugin->events = calloc(count, sizeof(mta_event_def_t));

            if (plugin->events == NULL) {
                plugin->event_count = 0;
                plugin->event_capacity = 0;
            } else {
                plugin->event_count = count;
                plugin->event_capacity = count;

                uint16_t i = 0;
                for (cJSON* evt = item->child; evt != NULL; evt = evt->next, i++) {
                    mta_event_def_t* def = &plugin->events[i];
                    def->name = mta_string_intern(&plugin->strings, evt->string);
                    mta_parse_callable_body(
                        plugin,
                        evt,
                        &def->actions,
                        &def->action_count,
                        &def->locals,
                        &def->params,
                        &def->param_count
                    );
                }
            }
        }
    }

    // Pre-intern names the runtime always needs so they live in the persistent part of the pool.
    mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
    mta_string_intern(&plugin->strings, MTA_STATUS_VAR);
    mta_string_intern(&plugin->strings, "arg");

    char buf[128];

    for (mta_var_def_t* var = plugin->globals; var != NULL; var = var->next) {
        const char* name = var_base_name(var->name, buf, sizeof(buf));
        if (name != NULL) mta_string_intern(&plugin->strings, name);
    }

    plugin->base_string_count = plugin->strings.count;

    // Locals that shadow a plugin global would silently break global propagation.
    for (uint16_t i = 0; i < plugin->function_count; i++) {
        for (mta_var_def_t* local = plugin->functions[i].locals; local != NULL;
             local = local->next) {
            for (mta_var_def_t* global = plugin->globals; global != NULL; global = global->next) {
                const char* name = global->name;
                const char* bracket = strchr(name, '[');

                if (bracket != NULL && (size_t)(bracket - name) < sizeof(buf)) {
                    memcpy(buf, name, bracket - name);
                    buf[bracket - name] = '\0';
                    name = buf;
                }

                if (!strcmp(local->name, name)) {
                    const char* fn_name =
                        mta_string_get(&plugin->strings, plugin->functions[i].name);
                    mta_raise(
                        plugin,
                        MTA_RT_ERR_NONE,
                        "function '%s': var '$%s' shadows plugin-level global",
                        fn_name ? fn_name : "?",
                        local->name
                    );
                }
            }
        }
    }

    if (plugin->config_schema != NULL && cJSON_IsObject(plugin->config_schema)) {
        mta_plugin_set_config(plugin, NULL);
    }

    *out = plugin;
}

/**
 * Parse a function / event body.
 *
 * A body is either an action list directly, or an object with "actions", optional "vars"
 * (locals) and optional "args" (positional parameter names).
 */
void mta_parse_callable_body(
    mta_plugin_t* plugin,
    cJSON* body,
    mta_action_t** actions,
    uint16_t* action_count,
    mta_var_def_t** locals,
    uint16_t** params,
    uint8_t* param_count
) {
    *actions = NULL;
    *action_count = 0;
    if (locals != NULL) *locals = NULL;
    if (params != NULL) *params = NULL;
    if (param_count != NULL) *param_count = 0;

    if (params != NULL && param_count != NULL) {
        cJSON* args = cJSON_GetObjectItem(body, "args");

        if (args != NULL && cJSON_IsArray(args)) {
            int count = cJSON_GetArraySize(args);

            if (count >= 1 && count <= 255) {
                uint16_t* names = calloc(count, sizeof(uint16_t));

                if (names != NULL) {
                    uint16_t* name = names;

                    for (cJSON* arg = args->child; arg != NULL; arg = arg->next, name++) {
                        if (!cJSON_IsString(arg) || arg->valuestring == NULL) {
                            *name = 0;
                            continue;
                        }

                        const char* str = arg->valuestring;
                        if (str[0] == '$') str++;
                        *name = mta_string_intern(&plugin->strings, str);
                    }

                    *params = names;
                    *param_count = count;
                }
            }
        }
    }

    cJSON* list = cJSON_GetObjectItem(body, "actions");

    if (list == NULL) {
        *action_count = mta_parse_action_list(plugin, actions, body);
        return;
    }

    *action_count = mta_parse_action_list(plugin, actions, list);

    if (locals == NULL) return;

    cJSON* vars = cJSON_GetObjectItem(body, "vars");
    if (vars == NULL) return;

    // Load locals through the globals list, then detach the newly appended tail.
    mta_var_def_t* tail = plugin->globals;
    while (tail != NULL && tail->next != NULL) tail = tail->next;

    mta_load_variables(plugin, vars);

    if (tail == NULL) {
        *locals = plugin->globals;
        plugin->globals = NULL;
    } else {
        *locals = tail->next;
        tail->next = NULL;
    }
}

void mta_plugin_set_config(mta_plugin_t* plugin, cJSON* user_config) {
    if (plugin == NULL) return;

    if (plugin->config != NULL) {
        cJSON_Delete(plugin->config);
        plugin->config = NULL;
    }

    cJSON* schema = plugin->config_schema;
    cJSON* config = cJSON_CreateObject();
    if (config == NULL) return;

    if (schema == NULL || !cJSON_IsObject(schema)) {
        // No schema: take the user config verbatim.
        if (user_config != NULL) {
            cJSON_Delete(config);
            config = cJSON_Duplicate(user_config, 1);
        }

        plugin->config = config;
        return;
    }

    for (cJSON* field = schema->child; field != NULL; field = field->next) {
        const char* key = field->string;
        if (key == NULL) continue;

        cJSON* type = cJSON_GetObjectItem(field, "type");
        cJSON* def = cJSON_GetObjectItem(field, "default");
        if (type == NULL || !cJSON_IsString(type)) continue;

        cJSON* value = user_config ? cJSON_GetObjectItem(user_config, key) : NULL;

        if (value != NULL) {
            const char* t = type->valuestring;

            if (!strcmp(t, "bool") && cJSON_IsBool(value)) {
                cJSON_AddBoolToObject(config, key, cJSON_IsTrue(value));
            } else if (!strcmp(t, "int") && cJSON_IsNumber(value)) {
                cJSON_AddNumberToObject(config, key, (double)value->valueint);
            } else if (!strcmp(t, "string") && cJSON_IsString(value)) {
                cJSON_AddStringToObject(config, key, value->valuestring);
            } else if (def != NULL) {
                cJSON_AddItemToObject(config, key, cJSON_Duplicate(def, 1));
            }

            continue;
        }

        if (def != NULL) cJSON_AddItemToObject(config, key, cJSON_Duplicate(def, 1));
    }

    plugin->config = config;
}

const char* mta_plugin_get_name(mta_plugin_t* plugin) {
    if (plugin == NULL) return NULL;
    return plugin->name;
}

const char* mta_plugin_get_display_name(mta_plugin_t* plugin) {
    if (plugin == NULL) return NULL;

    const char* name = mta_string_get(&plugin->strings, plugin->display_name);
    if (name == NULL || name[0] == '\0') return plugin->name;
    return name;
}

const char* mta_plugin_get_type(mta_plugin_t* plugin) {
    if (plugin == NULL) return NULL;
    return plugin->type;
}

const char* mta_plugin_get_description(mta_plugin_t* plugin) {
    if (plugin == NULL) return NULL;
    return plugin->description;
}

cJSON* mta_plugin_get_permissions(mta_plugin_t* plugin) {
    if (plugin == NULL) return NULL;
    return plugin->permissions;
}

bool mta_plugin_has_config_schema(mta_plugin_t* plugin) {
    if (plugin == NULL || plugin->config_schema == NULL) return false;
    if (!cJSON_IsObject(plugin->config_schema)) return false;
    return plugin->config_schema->child != NULL;
}

bool mta_plugin_get_config_value(
    mta_plugin_t* plugin, const char* key, mta_config_value_t* out_value
) {
    if (plugin == NULL || key == NULL || out_value == NULL) return false;
    if (plugin->config == NULL) return false;

    cJSON* item = cJSON_GetObjectItem(plugin->config, key);
    if (item == NULL) return false;

    if (cJSON_IsBool(item)) {
        out_value->type = MTA_CONFIG_TYPE_BOOL;
        out_value->value.bool_value = cJSON_IsTrue(item) != 0;
        return true;
    }

    if (cJSON_IsNumber(item)) {
        out_value->type = MTA_CONFIG_TYPE_INT;
        out_value->value.int_value = item->valueint;
        return true;
    }

    if (cJSON_IsString(item)) {
        out_value->type = MTA_CONFIG_TYPE_STRING;
        out_value->value.string_value = item->valuestring;
        return true;
    }

    return false;
}

bool mta_plugin_set_config_value(
    mta_plugin_t* plugin, const char* key, const mta_config_value_t* value
) {
    if (plugin == NULL || key == NULL || value == NULL) return false;
    if (plugin->config == NULL) return false;

    // Enforce the schema type when the field is declared.
    if (plugin->config_schema != NULL) {
        cJSON* field = cJSON_GetObjectItem(plugin->config_schema, key);
        cJSON* type = field ? cJSON_GetObjectItem(field, "type") : NULL;

        if (type != NULL && cJSON_IsString(type)) {
            const char* t = type->valuestring;
            if (!strcmp(t, "bool") && value->type != MTA_CONFIG_TYPE_BOOL) return false;
            if (!strcmp(t, "int") && value->type != MTA_CONFIG_TYPE_INT) return false;
            if (!strcmp(t, "string") && value->type != MTA_CONFIG_TYPE_STRING) return false;
        }
    }

    cJSON_DeleteItemFromObject(plugin->config, key);

    switch (value->type) {
    case MTA_CONFIG_TYPE_BOOL:
        cJSON_AddBoolToObject(plugin->config, key, value->value.bool_value);
        return true;

    case MTA_CONFIG_TYPE_INT:
        cJSON_AddNumberToObject(plugin->config, key, (double)value->value.int_value);
        return true;

    case MTA_CONFIG_TYPE_STRING:
        if (value->value.string_value != NULL) {
            cJSON_AddStringToObject(plugin->config, key, value->value.string_value);
        }
        return true;
    }

    return false;
}

cJSON* mta_plugin_serialize_config(mta_plugin_t* plugin) {
    if (plugin == NULL || plugin->config == NULL) return NULL;
    return cJSON_Duplicate(plugin->config, 1);
}

const char* mta_plugin_get_match_ble_name(mta_plugin_t* plugin) {
    if (plugin == NULL || plugin->match == NULL) return NULL;
    if (plugin->match->ble_name == 0) return NULL;
    return mta_string_get(&plugin->strings, plugin->match->ble_name);
}

const char* mta_plugin_get_match_ble_name_prefix(mta_plugin_t* plugin) {
    if (plugin == NULL || plugin->match == NULL) return NULL;
    if (plugin->match->ble_name_prefix == 0) return NULL;
    return mta_string_get(&plugin->strings, plugin->match->ble_name_prefix);
}

void mta_plugin_iterate_config_fields(
    mta_plugin_t* plugin, mta_config_field_iterator_t iterator, void* user_data
) {
    if (plugin == NULL || iterator == NULL || plugin->config_schema == NULL) return;

    cJSON* schema = plugin->config_schema;
    cJSON* config = plugin->config;

    if (!cJSON_IsObject(schema)) return;

    for (cJSON* field = schema->child; field != NULL; field = field->next) {
        if (field->string == NULL) continue;

        cJSON* type = cJSON_GetObjectItem(field, "type");
        if (type == NULL || !cJSON_IsString(type)) continue;

        const char* t = type->valuestring;
        mta_config_field_info_t info;
        memset(&info, 0, sizeof(info));
        info.field_name = field->string;

        cJSON* item = cJSON_GetObjectItem(field, "label");
        info.label = (item != NULL && cJSON_IsString(item)) ? item->valuestring : NULL;

        item = cJSON_GetObjectItem(field, "description");
        info.description = (item != NULL && cJSON_IsString(item)) ? item->valuestring : NULL;

        cJSON* min = cJSON_GetObjectItem(field, "min");
        cJSON* max = cJSON_GetObjectItem(field, "max");

        info.has_min = min != NULL && cJSON_IsNumber(min);
        info.min = info.has_min ? min->valueint : 0;
        info.has_max = max != NULL && cJSON_IsNumber(max);
        info.max = info.has_max ? max->valueint : 0;

        cJSON* def = cJSON_GetObjectItem(field, "default");
        cJSON* cur = config ? cJSON_GetObjectItem(config, field->string) : NULL;

        if (!strcmp(t, "bool")) {
            bool d = def != NULL && cJSON_IsBool(def) && cJSON_IsTrue(def);
            info.type = MTA_CONFIG_TYPE_BOOL;
            info.default_value.type = MTA_CONFIG_TYPE_BOOL;
            info.default_value.value.bool_value = d;
            info.current_value.type = MTA_CONFIG_TYPE_BOOL;
            info.current_value.value.bool_value =
                (cur != NULL && cJSON_IsBool(cur)) ? cJSON_IsTrue(cur) != 0 : d;
        } else if (!strcmp(t, "int")) {
            int d = (def != NULL && cJSON_IsNumber(def)) ? def->valueint : 0;
            info.type = MTA_CONFIG_TYPE_INT;
            info.default_value.type = MTA_CONFIG_TYPE_INT;
            info.default_value.value.int_value = d;
            info.current_value.type = MTA_CONFIG_TYPE_INT;
            info.current_value.value.int_value =
                (cur != NULL && cJSON_IsNumber(cur)) ? cur->valueint : d;
        } else if (!strcmp(t, "string")) {
            const char* d = (def != NULL && cJSON_IsString(def)) ? def->valuestring : NULL;
            info.type = MTA_CONFIG_TYPE_STRING;
            info.default_value.type = MTA_CONFIG_TYPE_STRING;
            info.default_value.value.string_value = d;
            info.current_value.type = MTA_CONFIG_TYPE_STRING;
            info.current_value.value.string_value =
                (cur != NULL && cJSON_IsString(cur)) ? cur->valuestring : d;
        } else {
            continue;
        }

        iterator(&info, user_data);
    }
}

static mta_condition_type_t control_type(const char* name, bool* is_control) {
    *is_control = true;
    if (!strcmp(name, "if")) return MTA_CONDITION_IF;
    if (!strcmp(name, "while")) return MTA_CONDITION_WHILE;
    if (!strcmp(name, "unless")) return MTA_CONDITION_UNLESS;
    *is_control = false;
    return MTA_CONDITION_IF;
}

/**
 * Initialize one action from `name` and its argument / condition JSON, plus an optional
 * "to" capture taken from `container`.
 */
static void parse_action(
    mta_plugin_t* plugin, mta_action_t* action, const char* name, cJSON* value, cJSON* container
) {
    bool is_control;
    mta_condition_type_t type = control_type(name, &is_control);

    if (is_control) {
        action->condition = mta_parse_condition(plugin, value, type);
    } else {
        mta_parse_action_args(plugin, action, value);
    }

    cJSON* to = cJSON_GetObjectItem(container, "to");

    if (to != NULL && cJSON_IsString(to)) {
        const char* capture = to->valuestring;
        if (capture != NULL && capture[0] == '$') capture++;
        if (capture == NULL) capture = "";
        action->capture = mta_string_intern(&plugin->strings, capture);
    }
}

/**
 * Parse an action list. Accepts an array of single-key objects ([{"fn": args}, ...]) or an
 * object whose keys are called in order ({"fn": args, ...}).
 */
uint16_t mta_parse_action_list(mta_plugin_t* plugin, mta_action_t** out, cJSON* json) {
    uint16_t count = 0;

    if (plugin == NULL || json == NULL) goto empty;

    if (cJSON_IsArray(json)) {
        count = cJSON_GetArraySize(json);
    } else if (cJSON_IsObject(json)) {
        for (cJSON* item = json->child; item != NULL; item = item->next) count++;
    } else {
        goto empty;
    }

    if (count == 0) goto empty;

    mta_action_t* actions = calloc(count, sizeof(mta_action_t));
    if (actions == NULL) goto empty;

    uint16_t index = 0;

    if (cJSON_IsArray(json)) {
        for (cJSON* item = json->child; item != NULL; item = item->next) {
            if (cJSON_IsString(item)) {
                mta_raise(
                    plugin,
                    MTA_RT_ERR_NONE,
                    "Bare-string action '%s' is not valid; use {\"%s\": []}",
                    item->valuestring,
                    item->valuestring
                );
                continue;
            }

            cJSON* call = cJSON_IsObject(item) ? item->child : NULL;

            if (call != NULL && call->string != NULL) {
                mta_action_t* action = &actions[index];
                action->function = mta_string_intern(&plugin->strings, call->string);
                action->args = NULL;
                action->arg_count = 0;
                action->condition = NULL;
                action->capture = 0;
                parse_action(plugin, action, call->string, call, item);
            }

            index++;
        }
    } else if (cJSON_IsObject(json)) {
        for (cJSON* item = json->child; item != NULL; item = item->next) {
            if (item->string == NULL) continue;

            mta_action_t* action = &actions[index];
            action->function = mta_string_intern(&plugin->strings, item->string);
            action->condition = NULL;
            action->capture = 0;
            parse_action(plugin, action, item->string, item, json);
            index++;
        }
    }

    *out = actions;
    return index;

empty:
    *out = NULL;
    return 0;
}

static const char* const COMPARISON_OPS[] = {
    "eq", "neq", "lt", "lte", "gt", "gte", "strcmp", NULL,
};

/**
 * Parse an if / unless / while block:
 *   { "all": [...], "any": [...], "none": [...], "then": [...], "else": [...] }
 * or the shorthand { "<op>": [a, b], "then": [...] } with a single comparison operator.
 */
mta_condition_t* mta_parse_condition(mta_plugin_t* plugin, cJSON* json, mta_condition_type_t type) {
    if (plugin == NULL || json == NULL) return NULL;

    mta_condition_t* cond = calloc(1, sizeof(mta_condition_t));
    if (cond == NULL) return NULL;

    cond->type = type;
    cond->all = NULL;
    cond->any = NULL;
    cond->none = NULL;
    cond->then_actions = NULL;
    cond->else_actions = NULL;
    cond->all_count = 0;
    cond->any_count = 0;
    cond->none_count = 0;
    cond->then_count = 0;
    cond->else_count = 0;

    cJSON* all = cJSON_GetObjectItem(json, "all");
    if (all != NULL) cond->all_count = mta_parse_action_list(plugin, &cond->all, all);

    cJSON* any = cJSON_GetObjectItem(json, "any");
    if (any != NULL) cond->any_count = mta_parse_action_list(plugin, &cond->any, any);

    cJSON* none = cJSON_GetObjectItem(json, "none");
    if (none != NULL) cond->none_count = mta_parse_action_list(plugin, &cond->none, none);

    if (all == NULL && any == NULL) {
        if (none == NULL) {
            const char* ops[sizeof(COMPARISON_OPS) / sizeof(COMPARISON_OPS[0])];
            cJSON* args = NULL;
            const char* op = NULL;
            int found = 0;

            memcpy(ops, COMPARISON_OPS, sizeof(ops));

            for (const char** p = ops; *p != NULL; p++) {
                cJSON* item = cJSON_GetObjectItem(json, *p);
                if (item != NULL) {
                    found++;
                    args = item;
                    op = *p;
                }
            }

            if (found > 1) {
                MTA_LOGW(TAG, "Multiple comparison operators in condition without explicit combinator");
            }

            if (args != NULL && op != NULL) {
                mta_action_t* action = calloc(1, sizeof(mta_action_t));
                cond->all = action;

                if (action != NULL) {
                    action->function = mta_string_intern(&plugin->strings, op);
                    action->condition = NULL;
                    action->capture = 0;
                    action->args = NULL;
                    action->arg_count = 0;
                    mta_parse_action_args(plugin, action, args);
                    cond->all_count = 1;
                }
            }
        }
    } else if (all != NULL && (any != NULL || none != NULL)) {
        MTA_LOGW(TAG, "Multiple combinators (all/any/none) at same level - using 'all' only");
    }

    cJSON* then_json = cJSON_GetObjectItem(json, "then");
    if (then_json != NULL) {
        cond->then_count = mta_parse_action_list(plugin, &cond->then_actions, then_json);
    }

    cJSON* else_json = cJSON_GetObjectItem(json, "else");
    if (else_json != NULL) {
        cond->else_count = mta_parse_action_list(plugin, &cond->else_actions, else_json);
    }

    return cond;
}

static void parse_literal(mta_plugin_t* plugin, mta_arg_t* arg, cJSON* json) {
    if (cJSON_IsNumber(json)) {
        if (json->valuedouble != (double)json->valueint) {
            arg->type = MTA_ARG_FLOAT;
            arg->val.f = (float)json->valuedouble;
        } else {
            arg->type = MTA_ARG_INT;
            arg->val.i = json->valueint;
        }
    } else if (cJSON_IsString(json)) {
        const char* str = json->valuestring;

        if (str != NULL && str[0] == '$') {
            arg->type = MTA_ARG_VAR_REF;
        } else {
            arg->type = MTA_ARG_STRING_REF;
            if (str == NULL) str = "";
        }

        arg->val.str = mta_string_intern(&plugin->strings, str);
    } else {
        arg->type = MTA_ARG_INT;
        arg->val.i = 0;
    }
}

/**
 * Parse call arguments: an array of literals, or a single literal. Objects yield no args.
 * Strings starting with '$' are variable references.
 */
void mta_parse_action_args(mta_plugin_t* plugin, mta_action_t* action, cJSON* json) {
    if (plugin == NULL || action == NULL || json == NULL) goto none;

    if (cJSON_IsArray(json)) {
        uint8_t count = cJSON_GetArraySize(json);
        if (count == 0) goto none;

        action->args = calloc(count, sizeof(mta_arg_t));
        if (action->args == NULL) goto empty;
        action->arg_count = count;

        uint8_t i = 0;
        for (cJSON* item = json->child; item != NULL; item = item->next, i++) {
            action->args[i].name = 0;
            parse_literal(plugin, &action->args[i], item);
        }

        return;
    }

    if (cJSON_IsObject(json)) goto none;

    action->args = calloc(1, sizeof(mta_arg_t));
    if (action->args == NULL) goto empty;

    action->args[0].name = 0;
    action->arg_count = 1;
    parse_literal(plugin, &action->args[0], json);
    return;

none:
    if (action != NULL) action->args = NULL;
empty:
    if (action != NULL) action->arg_count = 0;
}
