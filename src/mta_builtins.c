/**
 * Built-in functions available to every plugin.
 *
 * Comparison builtins both return their result via "_" and use the function return value as
 * the condition outcome (0 = true / success, 1 = false), so they can be used directly inside
 * if / unless / while blocks.
 */
#include "mt_actions_internal.h"
#include <stdio.h>
#include <string.h>


#define COMPARE_BUILTIN(fn_name, str_name, op)                                                   \
    int fn_name(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {  \
        static const mta_arg_spec_t specs[] = {                                                  \
            { "a", MTA_KIND_NUMBER },                                                            \
            { "b", MTA_KIND_NUMBER },                                                            \
        };                                                                                       \
        mta_argv_t v[2];                                                                         \
        int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, str_name);          \
        if (ret != 0) return ret;                                                                \
        bool result = v[0].val.f op v[1].val.f;                                                  \
        mta_return_int(plugin, scope, result);                                                   \
        return !result;                                                                          \
    }

COMPARE_BUILTIN(mta_builtin_eq, "eq", ==)
COMPARE_BUILTIN(mta_builtin_neq, "neq", !=)
COMPARE_BUILTIN(mta_builtin_gt, "gt", >)
COMPARE_BUILTIN(mta_builtin_lt, "lt", <)
COMPARE_BUILTIN(mta_builtin_gte, "gte", >=)
COMPARE_BUILTIN(mta_builtin_lte, "lte", <=)

#define ARITH_BUILTIN(fn_name, str_name, op)                                                     \
    int fn_name(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {  \
        static const mta_arg_spec_t specs[] = {                                                  \
            { "a", MTA_KIND_NUMBER },                                                            \
            { "b", MTA_KIND_NUMBER },                                                            \
        };                                                                                       \
        mta_argv_t v[2];                                                                         \
        int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, str_name);          \
        if (ret != 0) return ret;                                                                \
        return mta_return_float(plugin, scope, v[0].val.f op v[1].val.f);                        \
    }

ARITH_BUILTIN(mta_builtin_add, "add", +)
ARITH_BUILTIN(mta_builtin_sub, "sub", -)
ARITH_BUILTIN(mta_builtin_mul, "mul", *)

int mta_builtin_div(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "a", MTA_KIND_NUMBER },
        { "b", MTA_KIND_NUMBER },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "div");
    if (ret != 0) return ret;

    if (v[1].val.f == 0.0f) {
        return mta_raise(plugin, MTA_RT_ERR_TYPE_MISMATCH, "div: divide by zero");
    }

    return mta_return_float(plugin, scope, v[0].val.f / v[1].val.f);
}

int mta_builtin_mod(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "a", MTA_KIND_INT },
        { "b", MTA_KIND_INT },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "mod");
    if (ret != 0) return ret;

    if (v[1].val.i == 0) {
        return mta_raise(plugin, MTA_RT_ERR_TYPE_MISMATCH, "mod: divide by zero");
    }

    return mta_return_int(plugin, scope, v[0].val.i % v[1].val.i);
}

int mta_builtin_round(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "value", MTA_KIND_NUMBER },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "round");
    if (ret != 0) return ret;

    float f = v[0].val.f;
    return mta_return_int(plugin, scope, (int32_t)(f + (f < 0.0f ? -0.5f : 0.5f)));
}

int mta_builtin_inc(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "var", MTA_KIND_VAR },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "inc");
    if (ret != 0) return ret;

    int32_t value = mta_arg_as_int(plugin, scope, mta_scope_get_var(scope, v[0].val.var)) + 1;
    mta_arg_t arg = { .name = v[0].val.var, .type = MTA_ARG_INT, .val.i = value };
    mta_scope_set_var(scope, v[0].val.var, &arg);
    return mta_return_int(plugin, scope, value);
}

int mta_builtin_dec(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "var", MTA_KIND_VAR },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "dec");
    if (ret != 0) return ret;

    int32_t value = mta_arg_as_int(plugin, scope, mta_scope_get_var(scope, v[0].val.var)) - 1;
    mta_arg_t arg = { .name = v[0].val.var, .type = MTA_ARG_INT, .val.i = value };
    mta_scope_set_var(scope, v[0].val.var, &arg);
    return mta_return_int(plugin, scope, value);
}

/**
 * set(value)        -> returns value
 * set($dest, value) -> assigns (and propagates plugin globals to enclosing scopes)
 */
int mta_builtin_set(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    if (arg_count == 1) {
        static const mta_arg_spec_t specs[] = {
            { "value", MTA_KIND_ANY },
        };

        mta_argv_t v[1];
        int ret = mta_read_args(plugin, scope, args, 1, specs, 1, v, "set");
        if (ret != 0) return ret;

        const mta_arg_t* value = v[0].val.any;

        switch (value->type) {
        case MTA_ARG_FLOAT: return mta_return_float(plugin, scope, value->val.f);
        case MTA_ARG_INT: return mta_return_int(plugin, scope, value->val.i);
        case MTA_ARG_STRING_REF: {
            const char* str = mta_string_get(&plugin->strings, value->val.str);
            return mta_return_string(plugin, scope, str ? str : "");
        }
        default: break;
        }
    } else {
        static const mta_arg_spec_t specs[] = {
            { "dest", MTA_KIND_VAR },
            { "value", MTA_KIND_ANY },
        };

        mta_argv_t v[2];
        int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "set");
        if (ret != 0) return ret;

        mta_arg_t value = *v[1].val.any;
        value.name = v[0].val.var;

        ret = mta_scope_set_var(scope, v[0].val.var, &value);
        if (ret != 0) return ret;

        mta_scope_propagate_global(scope, plugin, v[0].val.var, &value);
    }

    return mta_return_void(plugin, scope);
}

int mta_builtin_strcmp(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "a", MTA_KIND_STRING },
        { "b", MTA_KIND_STRING },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "strcmp");
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, strcmp(v[0].val.s, v[1].val.s));
}

int mta_builtin_strlen(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "str", MTA_KIND_STRING },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "strlen");
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, (int32_t)strlen(v[0].val.s));
}

int mta_builtin_charat(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "str", MTA_KIND_STRING },
        { "index", MTA_KIND_INT },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "charat");
    if (ret != 0) return ret;

    int len = (int)strlen(v[0].val.s);
    int index = v[1].val.i;

    if (index < 0 || index >= len) {
        return mta_raise(
            plugin,
            MTA_RT_ERR_TYPE_MISMATCH,
            "charat: index %d out of bounds for string of length %d",
            index,
            len
        );
    }

    char buf[2] = { v[0].val.s[index], '\0' };
    return mta_return_string(plugin, scope, buf);
}

int mta_builtin_chr(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "code", MTA_KIND_INT },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "chr");
    if (ret != 0) return ret;

    char buf[2] = { (uint32_t)v[0].val.i < 256 ? (char)v[0].val.i : '?', '\0' };
    return mta_return_string(plugin, scope, buf);
}

int mta_builtin_toString(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "value", MTA_KIND_ANY },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "to_string");
    if (ret != 0) return ret;

    const mta_arg_t* value = v[0].val.any;
    char buf[32];

    switch (value->type) {
    case MTA_ARG_INT:
        snprintf(buf, sizeof(buf), "%d", value->val.i);
        return mta_return_string(plugin, scope, buf);

    case MTA_ARG_FLOAT:
        snprintf(buf, sizeof(buf), "%g", (double)value->val.f);
        return mta_return_string(plugin, scope, buf);

    case MTA_ARG_STRING_REF: {
        const char* str = mta_string_get(&plugin->strings, value->val.str);
        return mta_return_string(plugin, scope, str ? str : "");
    }

    default:
        return mta_raise(
            plugin, MTA_RT_ERR_TYPE_MISMATCH, "to_string: cannot stringify value of this type"
        );
    }
}

int mta_builtin_concat(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t spec = { "part", MTA_KIND_STRING };

    mta_argv_iter_t it;
    mta_argv_t v;
    size_t len = 0;

    mta_argv_iter_init(&it, plugin, scope, args, arg_count, &spec, "concat");
    char buf[256] = { 0 };

    do {
        int ret = mta_argv_iter_next(&it, &v);
        if (ret == 2) return -1;
        if (ret != 0) break;

        size_t part = strlen(v.val.s);
        if (len + part > sizeof(buf) - 1) part = sizeof(buf) - 1 - len;
        memcpy(buf + len, v.val.s, part);
        len += part;
    } while (len < sizeof(buf) - 1);

    buf[len] = '\0';
    return mta_return_string(plugin, scope, buf);
}

int mta_builtin_getbyte(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "array", MTA_KIND_ARRAY },
        { "index", MTA_KIND_INT },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "getbyte");
    if (ret != 0) return ret;

    mta_array_t* array = v[0].val.arr;
    int index = v[1].val.i;

    if (index < 0 || index >= array->capacity) {
        return mta_raise(
            plugin,
            MTA_RT_ERR_TYPE_MISMATCH,
            "getbyte: index %d out of bounds for array of capacity %u",
            index,
            array->capacity
        );
    }

    return mta_return_int(plugin, scope, array->data[index]);
}

int mta_builtin_setbyte(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "array", MTA_KIND_ARRAY },
        { "index", MTA_KIND_INT },
        { "value", MTA_KIND_INT },
    };

    mta_argv_t v[3];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 3, v, "setbyte");
    if (ret != 0) return ret;

    mta_array_t* array = v[0].val.arr;
    int index = v[1].val.i;

    if (index < 0 || index >= array->capacity) {
        return mta_raise(
            plugin,
            MTA_RT_ERR_TYPE_MISMATCH,
            "setbyte: index %d out of bounds for array of capacity %u",
            index,
            array->capacity
        );
    }

    array->data[index] = v[2].val.i;
    if ((uint32_t)index >= array->length) array->length = index + 1;

    return mta_return_void(plugin, scope);
}

int mta_builtin_getConfig(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "key", MTA_KIND_STRING },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "get_config");
    if (ret != 0) return ret;

    mta_config_value_t value;

    if (mta_plugin_get_config_value(plugin, v[0].val.s, &value)) {
        switch (value.type) {
        case MTA_CONFIG_TYPE_BOOL: return mta_return_int(plugin, scope, value.value.bool_value);
        case MTA_CONFIG_TYPE_INT: return mta_return_int(plugin, scope, value.value.int_value);
        case MTA_CONFIG_TYPE_STRING:
            return mta_return_string(
                plugin, scope, value.value.string_value ? value.value.string_value : ""
            );
        }
    }

    return mta_raise(plugin, MTA_RT_ERR_VAR_NOT_SET, "get_config: key '%s' not set", v[0].val.s);
}

int mta_builtin_setConfig(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "key", MTA_KIND_STRING },
        { "value", MTA_KIND_ANY },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "set_config");
    if (ret != 0) return ret;

    const mta_arg_t* arg = v[1].val.any;
    mta_config_value_t value;

    if (arg->type == MTA_ARG_FLOAT) {
        value.type = MTA_CONFIG_TYPE_INT;
        value.value.int_value = (int)arg->val.f;
    } else if (arg->type == MTA_ARG_STRING_REF) {
        value.type = MTA_CONFIG_TYPE_STRING;
        value.value.string_value = mta_arg_as_string(plugin, scope, arg);
    } else {
        value.type = MTA_CONFIG_TYPE_INT;
        value.value.int_value = arg->val.i;
    }

    if (!mta_plugin_set_config_value(plugin, v[0].val.s, &value)) {
        return mta_raise(
            plugin,
            MTA_RT_ERR_HOST_DISPATCH_FAILED,
            "set_config: failed to store key '%s'",
            v[0].val.s
        );
    }

    mta_invoke_config_save(plugin);
    return mta_return_void(plugin, scope);
}
