/**
 * Argument validation and coercion for host functions (mta_read_args and friends).
 */
#include "mt_actions_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern mta_arg_t* _mta_resolve_var_ref_chain(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const mta_arg_t* arg,
    uint16_t* chain,
    int* depth,
    const char* context
);

static const char* arg_type_name(uint8_t type) {
    switch (type) {
    case MTA_ARG_INT: return "int";
    case MTA_ARG_FLOAT: return "float";
    case MTA_ARG_STRING_REF: return "string";
    case MTA_ARG_VAR_REF: return "variable reference";
    case MTA_ARG_ARRAY: return "array";
    default: return "unknown";
    }
}

int mta_read_args(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    mta_arg_t* args,
    uint8_t arg_count,
    const mta_arg_spec_t* specs,
    uint8_t spec_count,
    mta_argv_t* out,
    const char* fn_name
) {
    if (fn_name == NULL) fn_name = "<anonymous>";

    if (arg_count != spec_count) {
        return mta_raise(
            plugin,
            MTA_RT_ERR_ARG_COUNT_MISMATCH,
            "%s: expected %u arg(s), got %u",
            fn_name,
            spec_count,
            arg_count
        );
    }

    // Named arguments must match the spec order.
    for (uint8_t i = 0; i < arg_count; i++) {
        if (args[i].name == 0) continue;

        const char* given = mta_string_get(&plugin->strings, args[i].name);
        const char* expected = specs[i].name;

        if (given != NULL && expected != NULL && strcmp(given, expected)) {
            return mta_raise(
                plugin,
                MTA_RT_ERR_UNKNOWN_ARG,
                "%s: arg %u expected name '%s', got '%s'",
                fn_name,
                i,
                expected,
                given
            );
        }
    }

    for (uint8_t i = 0; i < arg_count; i++) {
        if (mta_read_one_arg(plugin, scope, &args[i], &specs[i], &out[i], fn_name, i) != 0) {
            return -1;
        }
    }

    return 0;
}

static int coerce_string_to_int(const char* str, int32_t* out) {
    if (str == NULL || str[0] == '\0') return -1;

    // Single characters coerce to their character code.
    if (str[1] == '\0') {
        *out = (unsigned char)str[0];
        return 0;
    }

    char* end = NULL;
    long value = strtol(str, &end, 10);
    if (end == str || end == NULL || *end != '\0') return -1;
    *out = (int32_t)value;
    return 0;
}

static int coerce_string_to_float(const char* str, float* out) {
    if (str == NULL || str[0] == '\0') {
        *out = 0.0f;
        return 0;
    }

    if (str[1] == '\0') {
        *out = (float)(unsigned char)str[0];
        return 0;
    }

    char* end = NULL;
    double value = strtod(str, &end);
    if (end == str || end == NULL || *end != '\0') return -1;
    *out = (float)value;
    return 0;
}

int mta_read_one_arg(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const mta_arg_t* arg,
    const mta_arg_spec_t* spec,
    mta_argv_t* out,
    const char* fn_name,
    uint8_t index
) {
    out->kind = spec->kind;

    // Variable-name arguments are not dereferenced: deliver the interned name.
    if (spec->kind == MTA_KIND_VAR) {
        if (arg->type != MTA_ARG_VAR_REF) {
            return mta_raise(
                plugin,
                MTA_RT_ERR_TYPE_MISMATCH,
                "%s: arg %u ('%s') expected variable name (e.g. \"$x\"), got %s",
                fn_name,
                index,
                spec->name,
                arg_type_name(arg->type)
            );
        }

        const char* name = mta_string_get(&plugin->strings, arg->val.str);

        if (name == NULL) {
            return mta_raise(
                plugin,
                MTA_RT_ERR_VAR_NOT_SET,
                "%s: arg %u ('%s') has invalid variable reference",
                fn_name,
                index,
                spec->name
            );
        }

        if (name[0] == '$') name++;

        if (name[0] == '\0') {
            return mta_raise(
                plugin,
                MTA_RT_ERR_VAR_NOT_SET,
                "%s: arg %u ('%s') has empty variable name",
                fn_name,
                index,
                spec->name
            );
        }

        uint16_t id = mta_string_intern(&plugin->strings, name);

        if (mta_scope_get_var(scope, id) == NULL) {
            return mta_raise(
                plugin,
                MTA_RT_ERR_VAR_NOT_SET,
                "%s: arg %u ('%s') references variable '$%s' not set in scope",
                fn_name,
                index,
                spec->name,
                name
            );
        }

        out->val.var = id;
        return 0;
    }

    char context[64];
    uint16_t chain[MTA_VAR_REF_MAX_HOPS];
    int depth = 0;

    snprintf(context, sizeof(context), "%s: arg %u ('%s')", fn_name, index, spec->name);

    mta_arg_t* value = _mta_resolve_var_ref_chain(plugin, scope, arg, chain, &depth, context);
    if (value == NULL) return -1;

    switch (spec->kind) {
    case MTA_KIND_INT: {
        int32_t i = 0;

        if (value->type == MTA_ARG_INT) {
            i = value->val.i;
        } else if (value->type == MTA_ARG_FLOAT) {
            i = (int32_t)value->val.f;
        } else if (value->type == MTA_ARG_STRING_REF) {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            if (coerce_string_to_int(str, &i) != 0) {
                return mta_raise(
                    plugin,
                    MTA_RT_ERR_TYPE_MISMATCH,
                    "%s: expected int, got non-numeric string \"%s\"",
                    context,
                    str ? str : ""
                );
            }
        } else {
            return mta_raise(
                plugin,
                MTA_RT_ERR_TYPE_MISMATCH,
                "%s: expected int, got %s",
                context,
                arg_type_name(value->type)
            );
        }

        out->val.i = i;
        return 0;
    }

    case MTA_KIND_FLOAT: {
        float f = 0.0f;

        if (value->type == MTA_ARG_FLOAT) {
            f = value->val.f;
        } else if (value->type == MTA_ARG_INT) {
            f = (float)value->val.i;
        } else if (value->type == MTA_ARG_STRING_REF) {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            if (coerce_string_to_float(str, &f) != 0) {
                return mta_raise(
                    plugin,
                    MTA_RT_ERR_TYPE_MISMATCH,
                    "%s: expected float, got non-numeric string \"%s\"",
                    context,
                    str ? str : ""
                );
            }
        } else {
            return mta_raise(
                plugin,
                MTA_RT_ERR_TYPE_MISMATCH,
                "%s: expected float, got %s",
                context,
                arg_type_name(value->type)
            );
        }

        out->val.f = f;
        return 0;
    }

    case MTA_KIND_NUMBER: {
        float f = 0.0f;

        if (value->type == MTA_ARG_FLOAT) {
            f = value->val.f;
        } else if (value->type == MTA_ARG_INT) {
            f = (float)value->val.i;
        } else if (value->type == MTA_ARG_STRING_REF) {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            if (coerce_string_to_float(str, &f) != 0) {
                return mta_raise(
                    plugin,
                    MTA_RT_ERR_TYPE_MISMATCH,
                    "%s: expected number, got non-numeric string \"%s\"",
                    context,
                    str ? str : ""
                );
            }
        } else {
            return mta_raise(
                plugin,
                MTA_RT_ERR_TYPE_MISMATCH,
                "%s: expected number, got %s",
                context,
                arg_type_name(value->type)
            );
        }

        out->val.f = f;
        return 0;
    }

    case MTA_KIND_STRING: {
        if (value->type == MTA_ARG_STRING_REF) {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            out->val.s = str ? str : "";
            return 0;
        }

        return mta_raise(
            plugin,
            MTA_RT_ERR_TYPE_MISMATCH,
            "%s: expected string, got %s",
            context,
            arg_type_name(value->type)
        );
    }

    case MTA_KIND_ARRAY: {
        if (value->type == MTA_ARG_ARRAY && value->val.arr != NULL) {
            out->val.arr = value->val.arr;
            return 0;
        }

        return mta_raise(
            plugin,
            MTA_RT_ERR_TYPE_MISMATCH,
            "%s: expected array, got %s",
            context,
            arg_type_name(value->type)
        );
    }

    case MTA_KIND_ANY: out->val.any = value; return 0;

    default: break;
    }

    return mta_raise(
        plugin, MTA_RT_ERR_NONE, "%s: internal error: unhandled spec kind %d", context, spec->kind
    );
}

void mta_argv_iter_init(
    mta_argv_iter_t* it,
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    mta_arg_t* args,
    uint8_t count,
    const mta_arg_spec_t* spec,
    const char* fn_name
) {
    it->plugin = plugin;
    it->scope = scope;
    it->args = args;
    it->count = count;
    it->index = 0;
    it->spec = spec;
    it->fn_name = fn_name ? fn_name : "<anonymous>";
}

int mta_argv_iter_next(mta_argv_iter_t* it, mta_argv_t* out) {
    int ret = 1;

    if (it != NULL && it->index < it->count && it->spec != NULL) {
        uint8_t index = it->index++;
        ret = mta_read_one_arg(it->plugin, it->scope, &it->args[index], it->spec, out, it->fn_name, index) != 0 ? 2 : 0;
    }

    return ret;
}
