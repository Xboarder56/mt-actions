/**
 * Accessors and return helpers for host (system) functions.
 */
#include "mt_actions_internal.h"
#include <stdlib.h>
#include <string.h>

/**
 * Resolve a "$name" variable reference argument one hop through the scope.
 */
static mta_arg_t* resolve_ref(mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* arg) {
    const char* name = mta_string_get(&plugin->strings, arg->val.str);
    if (name != NULL && name[0] == '$') name++;
    if (name == NULL) return NULL;
    return mta_scope_get_var(scope, mta_string_intern(&plugin->strings, name));
}

mta_arg_type_t
mta_arg_get_type(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index) {
    if (args == NULL) return MTA_ARG_INT;
    mta_arg_t* arg = &args[index];

    if (arg->type != MTA_ARG_VAR_REF) return (mta_arg_type_t)arg->type;

    mta_arg_t* var = resolve_ref(plugin, scope, arg);
    if (var != NULL) return mta_arg_get_type(plugin, scope, var, 0);
    return MTA_ARG_INT;
}

int32_t mta_arg_get_int(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index) {
    if (args == NULL) return 0;
    mta_arg_t* arg = &args[index];

    switch (arg->type) {
    case MTA_ARG_FLOAT: return (int32_t)arg->val.f;
    case MTA_ARG_INT: return arg->val.i;
    case MTA_ARG_STRING_REF: {
        const char* str = mta_string_get(&plugin->strings, arg->val.str);
        if (str != NULL) return atoi(str);
        break;
    }
    case MTA_ARG_VAR_REF: {
        mta_arg_t* var = resolve_ref(plugin, scope, arg);
        if (var != NULL) return mta_arg_get_int(plugin, scope, var, 0);
        break;
    }
    }

    return 0;
}

float mta_arg_get_float(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index) {
    if (args == NULL) return 0.0f;
    mta_arg_t* arg = &args[index];

    switch (arg->type) {
    case MTA_ARG_FLOAT: return arg->val.f;
    case MTA_ARG_INT: return (float)arg->val.i;
    case MTA_ARG_STRING_REF: {
        const char* str = mta_string_get(&plugin->strings, arg->val.str);
        if (str != NULL) return (float)atof(str);
        break;
    }
    case MTA_ARG_VAR_REF: {
        mta_arg_t* var = resolve_ref(plugin, scope, arg);
        if (var != NULL) return mta_arg_get_float(plugin, scope, var, 0);
        break;
    }
    }

    return 0.0f;
}

const char*
mta_arg_get_string(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index) {
    if (args == NULL) return NULL;
    mta_arg_t* arg = &args[index];

    if (arg->type == MTA_ARG_STRING_REF) return mta_string_get(&plugin->strings, arg->val.str);

    if (arg->type == MTA_ARG_VAR_REF) {
        mta_arg_t* var = resolve_ref(plugin, scope, arg);
        if (var != NULL) return mta_arg_get_string(plugin, scope, var, 0);
    }

    return NULL;
}

int mta_arg_copy_bytes(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index, uint8_t* buf, int max
) {
    if (buf == NULL || max <= 0 || args == NULL) return -1;
    mta_arg_t* arg = &args[index];

    if (arg->type == MTA_ARG_VAR_REF) {
        mta_arg_t* var = resolve_ref(plugin, scope, arg);
        if (var != NULL) return mta_arg_copy_bytes(plugin, scope, var, 0, buf, max);
    } else if (arg->type == MTA_ARG_STRING_REF) {
        const char* str = mta_string_get(&plugin->strings, arg->val.str);
        if (str != NULL) {
            int len = (int)strlen(str);
            if (len > max) len = max;
            memcpy(buf, str, len);
            return len;
        }
    } else if (arg->type == MTA_ARG_ARRAY) {
        mta_array_t* array = arg->val.arr;
        if (array != NULL && array->data != NULL) {
            int len = array->length;
            if (len > max) len = max;
            for (int i = 0; i < len; i++) {
                buf[i] = (uint8_t)array->data[i];
            }
            return len;
        }
    }

    return -1;
}

void mta_scope_mark_returned(mta_scope_t* scope) {
    if (scope == NULL) return;
    scope->has_returned = true;
}

int mta_return_void(mta_plugin_t* plugin, mta_scope_t* scope) {
    (void)plugin;
    mta_scope_mark_returned(scope);
    return 0;
}

int mta_return_int(mta_plugin_t* plugin, mta_scope_t* scope, int32_t value) {
    uint16_t name = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
    mta_arg_t ret = { .name = name, .type = MTA_ARG_INT, .val.i = value };
    mta_scope_set_var(scope, name, &ret);
    mta_scope_mark_returned(scope);
    return 0;
}

int mta_return_float(mta_plugin_t* plugin, mta_scope_t* scope, float value) {
    uint16_t name = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
    mta_arg_t ret = { .name = name, .type = MTA_ARG_FLOAT, .val.f = value };
    mta_scope_set_var(scope, name, &ret);
    mta_scope_mark_returned(scope);
    return 0;
}

int mta_return_string(mta_plugin_t* plugin, mta_scope_t* scope, const char* value) {
    uint16_t name = mta_string_intern(&plugin->strings, MTA_RETURN_VAR);
    mta_arg_t ret;
    ret.name = name;
    ret.type = MTA_ARG_STRING_REF;
    if (value == NULL) value = "";
    ret.val.str = mta_string_intern(&plugin->strings, value);
    mta_scope_set_var(scope, name, &ret);
    mta_scope_mark_returned(scope);
    return 0;
}
