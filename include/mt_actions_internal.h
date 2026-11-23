#ifndef __mt_actions_internal_h
#define __mt_actions_internal_h

/**
 * Internal data structures for mt-actions. Hosts normally only need mt_actions.h; this header
 * is exposed for integrations that embed the runtime more deeply (driver glue, tooling).
 *
 * Offsets in comments are for 32-bit targets (ESP32 / wasm32) and match the shipped runtime.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "mt_actions.h"
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#define MTA_HAS_RTOS 1
#define MTA_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define MTA_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#elif defined(MTA_RTOS_SHIM)
// Host testing of the RTOS paths: the shim provides the FreeRTOS semaphore / task API.
#include MTA_RTOS_SHIM
#define MTA_HAS_RTOS 1
#define MTA_LOGW(tag, fmt, ...) mta_shim_log("W", tag, fmt, ##__VA_ARGS__)
#define MTA_LOGD(tag, fmt, ...) ((void)0)
#else
#define MTA_HAS_RTOS 0
#define MTA_LOGW(tag, fmt, ...) ((void)0)
#define MTA_LOGD(tag, fmt, ...) ((void)0)
#endif

/** Maximum re-entrant events queued per plugin while a handler is running. */
#define MTA_PENDING_EVENTS_MAX 4
#define MTA_PENDING_EVENT_ARGS_MAX 4
#define MTA_PENDING_EVENT_NAME_MAX 32

/** Maximum hops when following "$a" -> "$b" -> ... variable references. */
#define MTA_VAR_REF_MAX_HOPS 16

/** Upper bound of `while` iterations before the loop is forcibly terminated. */
#define MTA_WHILE_MAX_ITERATIONS 10000

/** Default / maximum capacity of "name[N]" array variables. */
#define MTA_ARRAY_DEFAULT_CAPACITY 100
#define MTA_ARRAY_MAX_CAPACITY 10000

/** Name of the implicit return-value variable and the status variable. */
#define MTA_RETURN_VAR "_"
#define MTA_STATUS_VAR "!"

typedef enum mta_condition_type {
    MTA_CONDITION_IF = 0,
    MTA_CONDITION_UNLESS = 1,
    MTA_CONDITION_WHILE = 2,
} mta_condition_type_t;

struct mta_condition;

/**
 * @brief One parsed action (call). 0x14 bytes.
 */
typedef struct mta_action {
    uint16_t function;               // +0x00 string id of callee ("if"/"while"/"unless" for control)
    mta_arg_t* args;                 // +0x04
    uint8_t arg_count;               // +0x08
    struct mta_condition* condition; // +0x0c non-NULL for control flow actions
    uint16_t capture;                // +0x10 string id of the "to" capture variable (0 = none)
} mta_action_t;

/**
 * @brief Control flow block. 0x24 bytes.
 */
typedef struct mta_condition {
    mta_condition_type_t type; // +0x00
    mta_action_t* all;         // +0x04 every action must succeed
    mta_action_t* any;         // +0x08 at least one action must succeed
    mta_action_t* none;        // +0x0c no action may succeed
    mta_action_t* then_actions; // +0x10
    mta_action_t* else_actions; // +0x14
    uint16_t all_count;        // +0x18
    uint16_t any_count;        // +0x1a
    uint16_t none_count;       // +0x1c
    uint16_t then_count;       // +0x1e
    uint16_t else_count;       // +0x20
} mta_condition_t;

/**
 * @brief Variable declaration (plugin global or function/event local). Linked list.
 *
 * `name` may carry an array suffix: "buf[16]" declares an int array of capacity 16.
 */
typedef struct mta_var_def {
    union {
        int32_t i;
        float f;
    } value;                  // +0x00
    uint8_t type;             // +0x04 mta_arg_type_t (INT, FLOAT or STRING_REF)
    uint16_t str;             // +0x06 string id when type == STRING_REF
    struct mta_var_def* next; // +0x08
    char name[];              // +0x0c
} mta_var_def_t;

/**
 * @brief Event handler. 0x18 bytes.
 */
typedef struct mta_event_def {
    uint16_t name;           // +0x00
    mta_action_t* actions;   // +0x04
    uint16_t action_count;   // +0x08
    mta_var_def_t* locals;   // +0x0c
    uint16_t* params;        // +0x10 positional parameter names
    uint8_t param_count;     // +0x14
} mta_event_def_t;

/**
 * @brief Plugin function. 0x1c bytes.
 */
typedef struct mta_function_def {
    uint16_t name;           // +0x00
    mta_action_t* actions;   // +0x04
    uint16_t action_count;   // +0x08
    uint32_t reserved;       // +0x0c
    mta_var_def_t* locals;   // +0x10
    uint16_t* params;        // +0x14
    uint8_t param_count;     // +0x18
} mta_function_def_t;

/**
 * @brief Automatic driver match descriptor. All string fields are string pool ids.
 */
typedef struct mta_match {
    uint16_t vid;
    uint16_t pid;
    uint16_t serial;
    uint16_t ble_name;
    uint16_t ble_name_prefix;
} mta_match_t;

/**
 * @brief Interned string pool. Ids are indices; id 0 is always "".
 */
typedef struct mta_string_pool {
    char** strings;
    uint16_t count;
    uint16_t capacity;
} mta_string_pool_t;

/**
 * @brief Reserved lookup table (currently unused by the interpreter, kept for ABI layout).
 */
typedef struct mta_hash_table {
    void* buckets;
    uint32_t reserved;
    uint16_t size;
} mta_hash_table_t;

#if MTA_HAS_RTOS
typedef struct mta_pending_event {
    char name[MTA_PENDING_EVENT_NAME_MAX];          // +0x00
    mta_arg_t args[MTA_PENDING_EVENT_ARGS_MAX];     // +0x20
    uint8_t arg_count;                              // +0x40
    mta_scope_t* scope;                             // +0x44
} mta_pending_event_t;
#endif

struct mta_plugin {
    mta_event_def_t* events;        // +0x00
    uint16_t event_count;           // +0x04
    uint16_t event_capacity;        // +0x06
    mta_function_def_t* functions;  // +0x08
    uint16_t function_count;        // +0x0c
    uint16_t function_capacity;     // +0x0e
    mta_hash_table_t event_index;   // +0x10
    mta_hash_table_t function_index; // +0x1c
    mta_string_pool_t strings;      // +0x28
    uint16_t base_string_count;     // +0x30 pool size after load; runtime strings are truncated
    uint16_t display_name;          // +0x32
    mta_match_t* match;             // +0x34
    void* reserved;                 // +0x38
#if MTA_HAS_RTOS
    SemaphoreHandle_t lock;                                  // +0x3c
    mta_pending_event_t pending[MTA_PENDING_EVENTS_MAX];     // +0x40
    uint8_t pending_count;                                   // +0x160
#endif
    mta_var_def_t* globals;         // +0x164 (+0x3c without RTOS)
    char* name;
    char* type;
    char* description;
    cJSON* permissions;
    cJSON* config;
    cJSON* config_schema;
};

struct mta_scope {
    mta_arg_t* vars;            // +0x00
    uint8_t var_count;          // +0x04
    uint8_t var_capacity;       // +0x05
    mta_array_t** owned_arrays; // +0x08
    uint8_t owned_count;        // +0x0c
    uint8_t owned_capacity;     // +0x0d
    void* user_data;            // +0x10
    bool has_returned;          // +0x14
    struct mta_scope* parent;   // +0x18
};

struct mta_driver_instance {
    mta_plugin_t* plugin;          // +0x00
    mta_scope_t* scope;            // +0x04
    void* device;                  // +0x08
    const mta_io_backend_t* io;    // +0x0c
    struct mta_driver_instance* next; // +0x10
};

typedef struct mta_system_function_entry {
    const char* name;
    mta_system_function_t fn;
    const char* permission;
} mta_system_function_entry_t;

typedef struct mta_event_entry {
    const char* name;
    const char* permission;
} mta_event_entry_t;

// String pool
void mta_string_pool_init(mta_string_pool_t* pool);
void mta_string_pool_free(mta_string_pool_t* pool);
void mta_string_pool_truncate(mta_string_pool_t* pool, uint16_t count);
const char* mta_string_get(mta_string_pool_t* pool, uint16_t id);
uint16_t mta_string_intern(mta_string_pool_t* pool, const char* str);

// Hash table
void mta_hash_table_init(mta_hash_table_t* table);
void mta_hash_table_free(mta_hash_table_t* table);

// Scopes
void mta_scope_init(mta_scope_t* scope);
void mta_scope_free(mta_scope_t* scope);
void mta_scope_register_owned_array(mta_scope_t* scope, mta_array_t* array);
int mta_scope_set_var(mta_scope_t* scope, uint16_t name, const mta_arg_t* value);
mta_arg_t* mta_scope_get_var(mta_scope_t* scope, uint16_t name);
bool mta_plugin_has_global(mta_plugin_t* plugin, uint16_t name);
void mta_scope_propagate_global(
    mta_scope_t* scope, mta_plugin_t* plugin, uint16_t name, const mta_arg_t* value
);
void mta_dump_scope(mta_plugin_t* plugin, mta_scope_t* scope, const char* label);

// Plugin lifecycle / parsing
void mta_plugin_init(mta_plugin_t* plugin);
void mta_free_action(mta_action_t* action);
void mta_load_variables(mta_plugin_t* plugin, cJSON* vars);
void mta_parse_callable_body(
    mta_plugin_t* plugin,
    cJSON* body,
    mta_action_t** actions,
    uint16_t* action_count,
    mta_var_def_t** locals,
    uint16_t** params,
    uint8_t* param_count
);
uint16_t mta_parse_action_list(mta_plugin_t* plugin, mta_action_t** out, cJSON* json);
mta_condition_t* mta_parse_condition(mta_plugin_t* plugin, cJSON* json, mta_condition_type_t type);
void mta_parse_action_args(mta_plugin_t* plugin, mta_action_t* action, cJSON* args);

// Interpreter
int mta_execute_action(mta_plugin_t* plugin, mta_action_t* action, mta_scope_t* scope);
void mta_init_scope_from_local_vars(mta_plugin_t* plugin, mta_var_def_t* vars, mta_scope_t* scope);
int mta_bind_positional_args(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const uint16_t* params,
    uint8_t param_count,
    const mta_arg_t* args,
    uint8_t arg_count,
    mta_scope_t* caller_scope,
    const char* fn_name
);
void mta_capture_result(mta_plugin_t* plugin, mta_scope_t* scope, mta_result_t* result);
mta_arg_t* mta_resolve_var_ref_hop(
    mta_plugin_t* plugin, mta_scope_t* scope, const mta_arg_t* ref, uint16_t* chain, int* depth
);
int mta_read_one_arg(
    mta_plugin_t* plugin,
    mta_scope_t* scope,
    const mta_arg_t* arg,
    const mta_arg_spec_t* spec,
    mta_argv_t* out,
    const char* fn_name,
    uint8_t index
);

// Builtins
int mta_builtin_eq(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_neq(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_gt(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_lt(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_gte(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_lte(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_add(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_sub(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_mul(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_div(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_mod(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_round(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_inc(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_dec(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_set(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_strcmp(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_strlen(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_charat(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_chr(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_toString(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_concat(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_getbyte(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_setbyte(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_getConfig(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);
int mta_builtin_setConfig(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count);

#ifdef __cplusplus
}
#endif

#endif
