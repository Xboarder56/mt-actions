/**
 * Driver instances: one persistent scope per connected device bound to a driver plugin.
 */
#include "mt_actions_internal.h"
#include <stdlib.h>

static mta_driver_instance_t* _instances = NULL;

mta_driver_instance_t*
mta_driver_instance_create(mta_plugin_t* plugin, void* device, const mta_io_backend_t* io) {
    if (plugin == NULL) return NULL;

    mta_driver_instance_t* instance = calloc(1, sizeof(mta_driver_instance_t));
    if (instance == NULL) return NULL;

    instance->scope = mta_scope_create();

    if (instance->scope == NULL) {
        free(instance);
        return NULL;
    }

    mta_init_scope_from_plugin_vars(plugin, instance->scope);
    mta_scope_set_user_data(instance->scope, instance);

    instance->plugin = plugin;
    instance->device = device;
    instance->io = io;
    instance->next = NULL;
    return instance;
}

void mta_driver_instance_add(mta_driver_instance_t* instance) {
    if (instance == NULL) return;

    for (mta_driver_instance_t* it = _instances; it != NULL; it = it->next) {
        if (it == instance) return;
    }

    instance->next = _instances;
    _instances = instance;
}

void mta_driver_instance_remove(mta_driver_instance_t* instance) {
    if (instance == NULL) return;

    mta_driver_instance_t** link = &_instances;

    for (mta_driver_instance_t* it = _instances; it != NULL; it = it->next) {
        if (instance == it) {
            *link = instance->next;
            instance->next = NULL;
            return;
        }

        link = &it->next;
    }
}

void mta_driver_instance_destroy(mta_driver_instance_t* instance) {
    if (instance == NULL) return;

    mta_driver_instance_remove(instance);

    if (instance->plugin != NULL && instance->scope != NULL) {
        mta_arg_t arg = { 0 };
        mta_event_invoke_with_args_scope(
            instance->plugin, "disconnect", &arg, 1, instance->scope, NULL
        );
    }

    if (instance->io != NULL && instance->io->close != NULL) {
        instance->io->close(instance->device);
    }

    if (instance->scope != NULL) mta_scope_destroy(instance->scope);
    free(instance);
}

int mta_driver_instance_dispatch(
    mta_driver_instance_t* instance, const char* event, int arg, mta_result_t* result
) {
    if (instance == NULL || instance->plugin == NULL || instance->scope == NULL || event == NULL) {
        if (result != NULL) {
            result->type = MTA_ARG_INT;
            result->val.i = 0;
            result->status = -1;
        }

        return -1;
    }

    mta_arg_t value = { 0 };
    value.val.i = arg;
    return mta_event_invoke_with_args_scope(instance->plugin, event, &value, 1, instance->scope, result);
}

int mta_driver_instance_dispatch_all(const char* event, int arg) {
    int handled = 0;
    if (event == NULL) return 0;

    mta_driver_instance_t* instance = _instances;

    while (instance != NULL) {
        mta_driver_instance_t* next = instance->next;
        if (mta_driver_instance_dispatch(instance, event, arg, NULL) == 0) handled++;
        instance = next;
    }

    return handled;
}

void mta_driver_instance_foreach(mta_driver_instance_cb_t cb, void* arg) {
    if (cb == NULL) return;

    mta_driver_instance_t* instance = _instances;

    while (instance != NULL) {
        mta_driver_instance_t* next = instance->next;
        cb(instance, arg);
        instance = next;
    }
}

mta_plugin_t* mta_driver_instance_get_plugin(mta_driver_instance_t* instance) {
    if (instance == NULL) return NULL;
    return instance->plugin;
}

mta_scope_t* mta_driver_instance_get_scope(mta_driver_instance_t* instance) {
    if (instance == NULL) return NULL;
    return instance->scope;
}

void* mta_driver_instance_get_device(mta_driver_instance_t* instance) {
    if (instance == NULL) return NULL;
    return instance->device;
}
