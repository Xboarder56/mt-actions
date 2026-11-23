#ifndef __mt_actions_h
#define __mt_actions_h

/**
 * mt-actions: Maus-Tec plugin action runtime.
 *
 * This is a clean-room reconstruction of the mt-actions runtime (v1.3.2) as shipped in
 * Edge-o-Matic 3000 firmware v2.1.0-rc.1. The behavior and data layout were recovered from
 * the official firmware image and from the WASM build of the same runtime that ships in
 * @maustec/mt-runtimes (mtp-core.wasm).
 *
 * Plugins are JSON documents (usually compiled from MTP source by mt-sdk) which declare
 * metadata, a config schema, global variables, functions and event handlers. Each function
 * or event handler body is a list of actions. An action is a call to a builtin, a host
 * ("system") function registered by the firmware, or a plugin function ("@name"), optionally
 * with control flow (if / unless / while).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MTA_VERSION "1.3.2"

struct mta_plugin;
struct mta_scope;
struct mta_driver_instance;

typedef struct mta_plugin mta_plugin_t;
typedef struct mta_scope mta_scope_t;
typedef struct mta_driver_instance mta_driver_instance_t;

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------

/**
 * @brief Runtime type tag of an argument / variable value.
 */
typedef enum mta_arg_type {
    MTA_ARG_INT = 0,        // val.i
    MTA_ARG_FLOAT = 1,      // val.f
    MTA_ARG_STRING_REF = 2, // val.str: string pool id
    MTA_ARG_VAR_REF = 3,    // val.str: string pool id of "$name"
    MTA_ARG_ARRAY = 4,      // val.arr
} mta_arg_type_t;

/**
 * @brief Fixed-capacity integer array (plugin "name[N]" variables).
 */
typedef struct mta_array {
    int32_t* data;
    uint16_t capacity;
    uint16_t length;
} mta_array_t;

/**
 * @brief An argument, scope variable, or literal value.
 *
 * `name` is a string pool id: for scope variables it is the variable name, for call arguments
 * it is the (optional) named-argument key.
 */
typedef struct mta_arg {
    uint16_t name;
    uint8_t type; // mta_arg_type_t
    union {
        int32_t i;
        float f;
        uint16_t str;
        mta_array_t* arr;
    } val;
} mta_arg_t;

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

typedef enum mta_runtime_error_kind {
    MTA_RT_ERR_NONE = 0,
    MTA_RT_ERR_VAR_NOT_SET = 1,
    MTA_RT_ERR_CYCLE_DETECTED = 2,
    MTA_RT_ERR_MISSING_RETURN = 3,
    MTA_RT_ERR_ARG_COUNT_MISMATCH = 4,
    MTA_RT_ERR_MISSING_ARG = 5,
    MTA_RT_ERR_UNKNOWN_ARG = 6,
    MTA_RT_ERR_TYPE_MISMATCH = 7,
    MTA_RT_ERR_HOST_DISPATCH_FAILED = 8,
} mta_runtime_error_kind_t;

/**
 * @brief Return a stable kebab-case identifier for an error kind ("type-mismatch", ...).
 */
const char* mta_runtime_error_kind_str(mta_runtime_error_kind_t kind);

/**
 * @brief Error reporter callback. `kind` is an mta_runtime_error_kind_t.
 */
typedef void (*mta_error_reporter_t)(mta_plugin_t* plugin, const char* msg, int kind, void* user_data);

void mta_set_error_reporter(mta_error_reporter_t reporter, void* user_data);

/**
 * @brief Raise a runtime error. Formats the message, notifies the action observer and the error
 * reporter, and returns -1 so host functions can `return mta_raise(...)`.
 *
 * If fmt is NULL the kind's identifier is used as the message.
 */
int mta_raise(mta_plugin_t* plugin, mta_runtime_error_kind_t kind, const char* fmt, ...);

// ---------------------------------------------------------------------------------------------
// Host functions & argument parsing
// ---------------------------------------------------------------------------------------------

/**
 * @brief Host (system) function. Return 0 on success (after calling one of the mta_return_*
 * helpers), a non-zero value to signal a false/failed condition, or -1 (via mta_raise) on error.
 */
typedef int (*mta_system_function_t)(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
);

/**
 * @brief Register (or replace) a host function. `permission` may be NULL for unrestricted
 * functions; otherwise the plugin must declare it (or "<group>:*") in its permissions list.
 * The name and permission strings are not copied.
 */
void mta_register_system_function(
    const char* fn_name, mta_system_function_t fn, const char* permission
);

/**
 * @brief Register (or update) a known event and the permission needed to receive it.
 */
void mta_register_event(const char* event_name, const char* permission);

/**
 * @brief Register the built-in functions (eq, add, set, concat, ...).
 */
void mta_init_builtins(void);

/**
 * @brief Expected kind for an argument spec.
 */
typedef enum mta_arg_kind {
    MTA_KIND_INT = 0,    // coerces float (truncate) and numeric strings
    MTA_KIND_FLOAT = 1,  // coerces int and numeric strings
    MTA_KIND_NUMBER = 2, // int or float, delivered as float
    MTA_KIND_STRING = 3, // string only
    MTA_KIND_ARRAY = 4,  // array variable
    MTA_KIND_VAR = 5,    // a "$name" reference; delivers the interned variable name id
    MTA_KIND_ANY = 6,    // delivers the resolved mta_arg_t*
} mta_arg_kind_t;

typedef struct mta_arg_spec {
    const char* name;
    mta_arg_kind_t kind;
} mta_arg_spec_t;

/**
 * @brief Validated / coerced argument value produced by mta_read_args.
 */
typedef struct mta_argv {
    mta_arg_kind_t kind;
    union {
        int32_t i;
        float f;
        const char* s;
        mta_array_t* arr;
        uint16_t var;
        const mta_arg_t* any;
    } val;
} mta_argv_t;

/**
 * @brief Validate and coerce a host function's arguments against a spec list.
 *
 * Checks the argument count, named-argument keys, resolves variable references in `scope`, and
 * coerces each value to the requested kind. Raises and returns -1 on any mismatch.
 */
int mta_read_args(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    mta_arg_t* args,
    uint8_t arg_count,
    const mta_arg_spec_t* specs,
    uint8_t spec_count,
    mta_argv_t* out,
    const char* fn_name
);

/**
 * @brief Iterator for variadic argument lists that share a single spec.
 */
typedef struct mta_argv_iter {
    mta_plugin_t* plugin;
    mta_scope_t* scope;
    mta_arg_t* args;
    uint8_t count;
    uint8_t index;
    const mta_arg_spec_t* spec;
    const char* fn_name;
} mta_argv_iter_t;

void mta_argv_iter_init(
    mta_argv_iter_t* it,
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    mta_arg_t* args,
    uint8_t count,
    const mta_arg_spec_t* spec,
    const char* fn_name
);

/**
 * @brief Read the next argument. Returns 0 on success, 1 when exhausted, 2 on error (raised).
 */
int mta_argv_iter_next(mta_argv_iter_t* it, mta_argv_t* out);

// Raw argument accessors (follow variable references through `scope`).
mta_arg_type_t
mta_arg_get_type(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index);
int32_t mta_arg_get_int(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index);
float mta_arg_get_float(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index);
const char*
mta_arg_get_string(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index);

/**
 * @brief Copy a string or byte-array argument into buf (at most max bytes).
 * @return Number of bytes copied, or -1 if the argument is neither a string nor an array.
 */
int mta_arg_copy_bytes(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t index, uint8_t* buf, int max
);

// Coerce a single resolved value.
int32_t mta_arg_as_int(mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* arg);
const char* mta_arg_as_string(mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* arg);

// Return values: store into the scope's "_" variable and mark the call as having returned.
int mta_return_void(mta_plugin_t* plugin, mta_scope_t* scope);
int mta_return_int(mta_plugin_t* plugin, mta_scope_t* scope, int32_t value);
int mta_return_float(mta_plugin_t* plugin, mta_scope_t* scope, float value);
int mta_return_string(mta_plugin_t* plugin, mta_scope_t* scope, const char* value);

/**
 * @brief Mark the scope as having produced a return value (used by the mta_return_* helpers).
 */
void mta_scope_mark_returned(mta_scope_t* scope);

// ---------------------------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------------------------

mta_scope_t* mta_scope_create(void);
void mta_scope_destroy(mta_scope_t* scope);
void mta_scope_set_user_data(mta_scope_t* scope, void* user_data);
void* mta_scope_get_user_data(mta_scope_t* scope);

/**
 * @brief Seed a scope with the plugin's global variables (initial values / arrays).
 */
void mta_init_scope_from_plugin_vars(mta_plugin_t* plugin, mta_scope_t* scope);

// ---------------------------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------------------------

/**
 * @brief Load a plugin from its parsed JSON. On failure *plugin is left untouched (NULL).
 * The JSON tree is not retained; everything needed is copied.
 */
void mta_load_plugin(mta_plugin_t** plugin, cJSON* root);
void mta_plugin_free(mta_plugin_t* plugin);

const char* mta_plugin_get_name(mta_plugin_t* plugin);
const char* mta_plugin_get_display_name(mta_plugin_t* plugin);
const char* mta_plugin_get_type(mta_plugin_t* plugin);
const char* mta_plugin_get_description(mta_plugin_t* plugin);
cJSON* mta_plugin_get_permissions(mta_plugin_t* plugin);
const char* mta_plugin_get_match_ble_name(mta_plugin_t* plugin);
const char* mta_plugin_get_match_ble_name_prefix(mta_plugin_t* plugin);

/**
 * @brief Does the plugin declare `permission` (exact match or "<group>:*")?
 * NULL permission always passes.
 */
bool mta_check_permission(mta_plugin_t* plugin, const char* permission);

// ---------------------------------------------------------------------------------------------
// Plugin config
// ---------------------------------------------------------------------------------------------

typedef enum mta_config_type {
    MTA_CONFIG_TYPE_BOOL = 0,
    MTA_CONFIG_TYPE_INT = 1,
    MTA_CONFIG_TYPE_STRING = 2,
} mta_config_type_t;

typedef struct mta_config_value {
    mta_config_type_t type;
    union {
        bool bool_value;
        int int_value;
        const char* string_value;
    } value;
} mta_config_value_t;

typedef struct mta_config_field_info {
    const char* field_name;
    mta_config_type_t type;
    const char* label;
    const char* description;
    int min;
    int max;
    bool has_min;
    bool has_max;
    mta_config_value_t default_value;
    mta_config_value_t current_value;
} mta_config_field_info_t;

typedef void (*mta_config_field_iterator_t)(const mta_config_field_info_t* field, void* user_data);

/**
 * @brief Apply user config (may be NULL) on top of the schema defaults.
 */
void mta_plugin_set_config(mta_plugin_t* plugin, cJSON* user_config);
bool mta_plugin_has_config_schema(mta_plugin_t* plugin);
bool mta_plugin_get_config_value(
    mta_plugin_t* plugin, const char* key, mta_config_value_t* out_value
);
bool mta_plugin_set_config_value(
    mta_plugin_t* plugin, const char* key, const mta_config_value_t* value
);
void mta_plugin_iterate_config_fields(
    mta_plugin_t* plugin, mta_config_field_iterator_t iterator, void* user_data
);

/**
 * @brief Serialize the current config values. Caller owns the returned tree.
 */
cJSON* mta_plugin_serialize_config(mta_plugin_t* plugin);

typedef bool (*mta_config_save_handler_t)(mta_plugin_t* plugin, void* user_data);
void mta_set_config_save_handler(mta_config_save_handler_t handler, void* user_data);
bool mta_invoke_config_save(mta_plugin_t* plugin);

// ---------------------------------------------------------------------------------------------
// Invocation
// ---------------------------------------------------------------------------------------------

/**
 * @brief Captured return value of an event or function invocation.
 */
typedef struct mta_result {
    mta_arg_type_t type;
    union {
        int32_t i;
        float f;
        char* s; // owned (strdup), release with mta_result_free
        mta_array_t* arr;
    } val;
    int status; // value of the "!" variable, or -1 if the invocation did not run
} mta_result_t;

void mta_result_free(mta_result_t* result);

/**
 * @brief Invoke an event handler on a plugin in the given (persistent) scope.
 *
 * Events are serialized per plugin; an event raised from inside a handler running on the same
 * task is queued and dispatched after the current handler completes.
 *
 * @return 0 on success (or no-op), -1 if the event is not handled / not permitted / errored.
 */
int mta_event_invoke_with_args_scope(
    mta_plugin_t* plugin,
    const char* event,
    mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* scope,
    mta_result_t* result
);

/**
 * @brief Call a plugin function by name in a fresh scope seeded with the plugin globals.
 */
int mta_call_plugin_function_scope(
    mta_plugin_t* plugin,
    const char* fn_name,
    mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* caller_scope,
    mta_result_t* result
);

// ---------------------------------------------------------------------------------------------
// Observability hooks
// ---------------------------------------------------------------------------------------------

typedef enum mta_action_event {
    MTA_ACTION_ENTER = 0,
    MTA_ACTION_EXIT = 1,
    MTA_ACTION_EVENT_ENTER = 2,
    MTA_ACTION_EVENT_EXIT = 3,
    MTA_ACTION_FUNCTION_ENTER = 4,
    MTA_ACTION_FUNCTION_EXIT = 5,
    MTA_ACTION_CONDITION = 6,
    MTA_ACTION_LOOP = 7,
    MTA_ACTION_RESERVED = 8,
    MTA_ACTION_ERROR = 9,
} mta_action_event_t;

typedef void (*mta_action_observer_t)(
    mta_plugin_t* plugin, mta_action_event_t event, const char* name, int value, void* user_data
);

void mta_set_action_observer(mta_action_observer_t observer, void* user_data);

/**
 * @brief Fallback for calls that match no builtin, host, or plugin function.
 */
typedef int (*mta_unresolved_function_handler_t)(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const char* fn_name,
    mta_arg_t* args,
    uint8_t arg_count,
    void* user_data
);

void mta_set_unresolved_function_handler(
    mta_unresolved_function_handler_t handler, void* user_data
);

// ---------------------------------------------------------------------------------------------
// Driver instances (BLE / accessory drivers: one persistent scope per connected device)
// ---------------------------------------------------------------------------------------------

typedef struct mta_io_backend {
    int (*write)(void* device, const void* data, size_t len, bool wait_response);
    int (*read)(void* device, void* buf, size_t len);
    void (*close)(void* device);
} mta_io_backend_t;

mta_driver_instance_t*
mta_driver_instance_create(mta_plugin_t* plugin, void* device, const mta_io_backend_t* io);

/**
 * @brief Fire "disconnect", close the I/O backend, and free the instance and its scope.
 * Removes the instance from the global list if still present.
 */
void mta_driver_instance_destroy(mta_driver_instance_t* instance);

void mta_driver_instance_add(mta_driver_instance_t* instance);
void mta_driver_instance_remove(mta_driver_instance_t* instance);

int mta_driver_instance_dispatch(
    mta_driver_instance_t* instance, const char* event, int arg, mta_result_t* result
);

/**
 * @brief Dispatch an event to every registered instance. Returns the number that handled it.
 */
int mta_driver_instance_dispatch_all(const char* event, int arg);

typedef void (*mta_driver_instance_cb_t)(mta_driver_instance_t* instance, void* arg);
void mta_driver_instance_foreach(mta_driver_instance_cb_t cb, void* arg);

mta_plugin_t* mta_driver_instance_get_plugin(mta_driver_instance_t* instance);
mta_scope_t* mta_driver_instance_get_scope(mta_driver_instance_t* instance);
void* mta_driver_instance_get_device(mta_driver_instance_t* instance);

#ifdef __cplusplus
}
#endif

#endif
