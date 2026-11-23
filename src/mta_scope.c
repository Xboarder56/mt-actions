#include "mt_actions_internal.h"
#include <stdlib.h>
#include <string.h>

void mta_scope_init(mta_scope_t* scope) {
    if (scope == NULL) return;
    scope->vars = NULL;
    scope->var_count = 0;
    scope->var_capacity = 0;
    scope->owned_arrays = NULL;
    scope->owned_count = 0;
    scope->owned_capacity = 0;
    scope->user_data = NULL;
    scope->has_returned = false;
    scope->parent = NULL;
}

void mta_scope_free(mta_scope_t* scope) {
    if (scope == NULL) return;

    for (uint8_t i = 0; i < scope->owned_count; i++) {
        mta_array_t* array = scope->owned_arrays[i];
        if (array != NULL) {
            if (array->data != NULL) free(array->data);
            free(scope->owned_arrays[i]);
        }
    }

    free(scope->owned_arrays);
    free(scope->vars);

    scope->vars = NULL;
    scope->var_count = 0;
    scope->var_capacity = 0;
    scope->owned_arrays = NULL;
    scope->owned_count = 0;
    scope->owned_capacity = 0;
}

mta_scope_t* mta_scope_create(void) {
    mta_scope_t* scope = calloc(1, sizeof(mta_scope_t));
    if (scope != NULL) mta_scope_init(scope);
    return scope;
}

void mta_scope_destroy(mta_scope_t* scope) {
    if (scope == NULL) return;
    mta_scope_free(scope);
    free(scope);
}

void mta_scope_set_user_data(mta_scope_t* scope, void* user_data) {
    if (scope == NULL) return;
    scope->user_data = user_data;
}

void* mta_scope_get_user_data(mta_scope_t* scope) {
    if (scope == NULL) return NULL;
    return scope->user_data;
}

void mta_scope_register_owned_array(mta_scope_t* scope, mta_array_t* array) {
    if (scope == NULL || array == NULL) return;

    if (scope->owned_count >= scope->owned_capacity) {
        uint8_t capacity = scope->owned_capacity ? scope->owned_capacity * 2 : 4;
        mta_array_t** arrays = realloc(scope->owned_arrays, capacity * sizeof(mta_array_t*));
        if (arrays == NULL) return;
        scope->owned_arrays = arrays;
        scope->owned_capacity = capacity;
    }

    scope->owned_arrays[scope->owned_count++] = array;
}

int mta_scope_set_var(mta_scope_t* scope, uint16_t name, const mta_arg_t* value) {
    if (scope == NULL || value == NULL) return -1;

    for (uint8_t i = 0; i < scope->var_count; i++) {
        if (scope->vars[i].name == name) {
            scope->vars[i] = *value;
            scope->vars[i].name = name;
            return 0;
        }
    }

    mta_arg_t copy = *value;

    if (scope->var_count == scope->var_capacity) {
        uint8_t capacity = scope->var_count + 4;
        mta_arg_t* vars = realloc(scope->vars, capacity * sizeof(mta_arg_t));
        if (vars == NULL) return -1;
        scope->vars = vars;
        scope->var_capacity = capacity;
    }

    scope->vars[scope->var_count] = copy;
    scope->vars[scope->var_count++].name = name;
    return 0;
}

mta_arg_t* mta_scope_get_var(mta_scope_t* scope, uint16_t name) {
    if (scope == NULL) return NULL;

    for (uint8_t i = 0; i < scope->var_count; i++) {
        if (scope->vars[i].name == name) return &scope->vars[i];
    }

    return NULL;
}

/**
 * Global names may carry an array suffix ("buf[16]"); compare only the base name.
 */
bool mta_plugin_has_global(mta_plugin_t* plugin, uint16_t name) {
    if (plugin == NULL) return false;

    const char* str = mta_string_get(&plugin->strings, name);
    if (str == NULL) return false;

    for (mta_var_def_t* var = plugin->globals; var != NULL; var = var->next) {
        const char* bracket = strchr(var->name, '[');
        size_t len = bracket ? (size_t)(bracket - var->name) : strlen(var->name);
        if (strlen(str) == len && !strncmp(var->name, str, len)) return true;
    }

    return false;
}

/**
 * Writes to a plugin global inside a nested (function) scope are copied to every enclosing
 * scope that already holds the variable, so they survive the call.
 */
void mta_scope_propagate_global(
    mta_scope_t* scope, mta_plugin_t* plugin, uint16_t name, const mta_arg_t* value
) {
    if (scope == NULL || plugin == NULL || value == NULL) return;
    if (!mta_plugin_has_global(plugin, name)) return;

    for (mta_scope_t* parent = scope->parent; parent != NULL; parent = parent->parent) {
        for (uint8_t i = 0; i < parent->var_count; i++) {
            if (parent->vars[i].name == name) {
                parent->vars[i] = *value;
                parent->vars[i].name = name;
                break;
            }
        }
    }
}

void mta_dump_scope(mta_plugin_t* plugin, mta_scope_t* scope, const char* label) {
    (void)plugin;
    (void)scope;
    (void)label;
}
