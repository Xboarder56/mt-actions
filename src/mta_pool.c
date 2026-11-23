#include "mt_actions_internal.h"
#include <stdlib.h>
#include <string.h>

void mta_string_pool_init(mta_string_pool_t* pool) {
    if (pool == NULL) return;
    pool->strings = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void mta_string_pool_free(mta_string_pool_t* pool) {
    if (pool == NULL) return;

    if (pool->strings != NULL) {
        for (uint16_t i = 0; i < pool->count; i++) {
            free(pool->strings[i]);
        }

        free(pool->strings);
    }

    pool->strings = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void mta_string_pool_truncate(mta_string_pool_t* pool, uint16_t count) {
    if (pool == NULL || count >= pool->count) return;

    for (uint16_t i = count; i < pool->count; i++) {
        free(pool->strings[i]);
        pool->strings[i] = NULL;
    }

    pool->count = count;
}

const char* mta_string_get(mta_string_pool_t* pool, uint16_t id) {
    if (pool == NULL || id >= pool->count) return NULL;
    return pool->strings[id];
}

uint16_t mta_string_intern(mta_string_pool_t* pool, const char* str) {
    if (pool == NULL || str == NULL) return 0;

    for (uint16_t i = 0; i < pool->count; i++) {
        if (pool->strings[i] != NULL && !strcmp(pool->strings[i], str)) return i;
    }

    if (pool->count == pool->capacity) {
        uint16_t capacity = pool->count + 8;
        char** strings = realloc(pool->strings, capacity * sizeof(char*));
        if (strings == NULL) return pool->count;
        pool->strings = strings;
        pool->capacity = capacity;
    }

    char* copy = malloc(strlen(str) + 1);
    if (copy == NULL) return pool->count;
    strcpy(copy, str);

    uint16_t id = pool->count;
    pool->strings[id] = copy;
    pool->count++;
    return id;
}
