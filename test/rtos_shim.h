/** Minimal FreeRTOS mutex/task shim (pthreads) for host-testing mt-actions RTOS paths. */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef void* TaskHandle_t;
typedef int BaseType_t;
typedef unsigned int TickType_t;
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1

typedef struct shim_mutex {
    pthread_mutex_t m;
    pthread_t owner;
    int held;
} * SemaphoreHandle_t;

static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)pthread_self(); }
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL);
    return s;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)t;
    pthread_mutex_lock(&s->m);
    s->owner = pthread_self();
    s->held = 1;
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    s->held = 0;
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
static inline TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s) {
    return s->held ? (TaskHandle_t)s->owner : NULL;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) {
    pthread_mutex_destroy(&s->m);
    free(s);
}
static inline void mta_shim_log(const char* lvl, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("LOG %s (%s) ", lvl, tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
