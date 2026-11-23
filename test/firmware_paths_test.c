/**
 * Host tests for the firmware-only runtime paths (not present in the WASM build used by the
 * differential tests): registered host functions + permissions, missing-return detection,
 * "to" capture with host functions, event permissions, the per-plugin event lock with the
 * re-entrant pending-event queue, and driver instances (driven with the real Lovense plugin).
 *
 * Expected behavior is taken from the official v2.1.0-rc.1 firmware image.
 *
 * build: make -C test fwtest   run: test/build/fwtest <eom-plugins>/drivers/lovense/plugin.json
 */
#include "cJSON.h"
#include "mt_actions_internal.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int _fails = 0, _checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        _checks++;                                                                                 \
        if (!(cond)) {                                                                             \
            _fails++;                                                                              \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                          \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

// ---- event log -----------------------------------------------------------------------------

static char _log[16384];
static pthread_mutex_t _log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char* fmt, ...) {
    va_list ap;
    char line[256];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&_log_lock);
    size_t used = strlen(_log), n = strlen(line);
    if (used + n + 2 < sizeof(_log)) {
        memcpy(_log + used, line, n);
        _log[used + n] = '\n';
        _log[used + n + 1] = '\0';
    }
    pthread_mutex_unlock(&_log_lock);
}

static void reset_log(void) { _log[0] = '\0'; }

static const char* KINDS[] = { "enter", "exit", "evt-enter", "evt-exit", "fn-enter",
                               "fn-exit", "cond", "loop", "note", "error" };

static void observer(mta_plugin_t* p, mta_action_event_t k, const char* name, int v, void* ud) {
    logf_("%s %s %d", KINDS[k], name, v);
}

static void reporter(mta_plugin_t* p, const char* msg, int kind, void* ud) {
    logf_("ERROR %d %s", kind, msg);
}

// ---- helpers -------------------------------------------------------------------------------

static mta_plugin_t* load(const char* json) {
    cJSON* root = cJSON_Parse(json);
    mta_plugin_t* p = NULL;
    mta_load_plugin(&p, root);
    cJSON_Delete(root);
    return p;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char* b = malloc(n + 1);
    b[fread(b, 1, n, f)] = 0;
    fclose(f);
    return b;
}

static mta_scope_t* scope_for(mta_plugin_t* p) {
    mta_scope_t* s = mta_scope_create();
    mta_init_scope_from_plugin_vars(p, s);
    return s;
}

static int global_int(mta_plugin_t* p, mta_scope_t* s, const char* name) {
    mta_arg_t* v = mta_scope_get_var(s, mta_string_intern(&p->strings, name));
    return v ? v->val.i : -12345;
}

static int fire(mta_plugin_t* p, mta_scope_t* s, const char* evt, int arg, mta_result_t* r) {
    mta_arg_t a = { 0 };
    a.val.i = arg;
    return mta_event_invoke_with_args_scope(p, evt, &a, 1, s, r);
}

// ---- host functions ------------------------------------------------------------------------

static int h_notify(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    logf_("HOST notify(%s)", n ? mta_arg_get_string(p, s, a, 0) : "");
    return mta_return_void(p, s);
}

static int h_lock(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    logf_("HOST lock_controls");
    return mta_return_void(p, s);
}

static int h_no_return(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    logf_("HOST no_return");
    return 0; // forgot mta_return_*
}

static int h_answer(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    return mta_return_int(p, s, 42);
}

static int h_record(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    char buf[128] = "HOST record(";
    for (uint8_t i = 0; i < n; i++) {
        char t[16];
        snprintf(t, sizeof(t), "%s%d", i ? "," : "", mta_arg_get_int(p, s, a, i));
        strcat(buf, t);
    }
    logf_("%s) argc=%d", buf, n);
    return mta_return_void(p, s);
}

static int _reenter_count = 1;
static int _reenter_rets[8];
static int _reenter_status[8];

/** Raise "inner" events from inside a running handler (same task). */
static int h_reenter(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    for (int i = 0; i < _reenter_count; i++) {
        mta_arg_t args[6];
        for (int k = 0; k < 6; k++) {
            memset(&args[k], 0, sizeof(args[k]));
            args[k].val.i = (i + 1) * 10 + k;
        }
        mta_result_t r;
        _reenter_rets[i] = mta_event_invoke_with_args_scope(p, "inner", args, 6, s, &r);
        _reenter_status[i] = r.status;
        logf_("HOST reenter queued #%d ret=%d status=%d", i, _reenter_rets[i], r.status);
    }
    return mta_return_void(p, s);
}

static int h_slow(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    logf_("HOST slow begin");
    usleep(200 * 1000);
    logf_("HOST slow end");
    return mta_return_void(p, s);
}

static char _ble_tx[512];
static int h_ble_write(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    uint8_t buf[64];
    int len = mta_arg_copy_bytes(p, s, a, 0, buf, sizeof(buf) - 1);
    if (len < 0) return mta_raise(p, MTA_RT_ERR_TYPE_MISMATCH, "ble_write: bad data");
    buf[len] = 0;
    void* ud = mta_scope_get_user_data(s);
    logf_("HOST ble_write(%s) ud=%s", buf, ud == (void*)0xB1E ? "ctx" : "other");
    if (strlen(_ble_tx) + len + 2 < sizeof(_ble_tx)) {
        strcat(_ble_tx, (char*)buf);
        strcat(_ble_tx, "|");
    }
    return mta_return_void(p, s);
}

static int h_log(mta_plugin_t* p, mta_scope_t* s, mta_arg_t* a, uint8_t n) {
    logf_("HOST log(%s)", n ? mta_arg_get_string(p, s, a, 0) : "");
    return mta_return_void(p, s);
}

// ---- io backend ----------------------------------------------------------------------------

static int _closed = 0;
static void* _closed_device = NULL;
static int io_write(void* dev, const void* d, size_t l, bool w) { return 0; }
static void io_close(void* dev) {
    _closed++;
    _closed_device = dev;
}
static const mta_io_backend_t IO = { .write = io_write, .read = NULL, .close = io_close };

// ---- tests ---------------------------------------------------------------------------------

static void test_permissions(void) {
    printf("permissions / host dispatch\n");
    mta_plugin_t* p = load("{\"name\":\"perm\",\"permissions\":[\"ui:notify\"],"
                           "\"variables\":{\"x\":0},"
                           "\"events\":{"
                           " \"ok\":[{\"notify\":\"hi\"}],"
                           " \"denied\":[{\"lock_controls\":\"r\"},{\"notify\":\"after\"}],"
                           " \"noret\":[{\"no_return\":[]},{\"notify\":\"after\"}],"
                           " \"cap\":[{\"set\":7},{\"answer\":[],\"to\":\"$x\"}],"
                           " \"arousal_change\":[{\"notify\":\"arousal\"}]}}");
    mta_plugin_t* q = load("{\"name\":\"wild\",\"permissions\":[\"ui:*\",\"arousal:read\"],"
                           "\"events\":{\"denied\":[{\"lock_controls\":\"r\"}],"
                           " \"arousal_change\":[{\"notify\":\"arousal\"}]}}");
    mta_scope_t* s = scope_for(p);
    mta_scope_t* t = scope_for(q);
    mta_result_t r;

    reset_log();
    CHECK(fire(p, s, "ok", 0, &r) == 0, "ok event should succeed");
    CHECK(strstr(_log, "HOST notify(hi)"), "notify should run: %s", _log);

    // A permission failure returns -1 silently: no error report and no action-exit trace.
    reset_log();
    CHECK(fire(p, s, "denied", 0, &r) == -1, "denied event should fail");
    CHECK(!strstr(_log, "HOST lock_controls"), "lock_controls must not run");
    CHECK(!strstr(_log, "ERROR"), "permission denial raises no error: %s", _log);
    CHECK(strstr(_log, "enter lock_controls 0") && !strstr(_log, "exit lock_controls"),
          "enter without exit: %s", _log);
    CHECK(!strstr(_log, "notify(after)"), "event aborts after denial");

    reset_log();
    CHECK(fire(q, t, "denied", 0, &r) == 0, "ui:* grants ui:lock");
    CHECK(strstr(_log, "HOST lock_controls"), "lock_controls should run with ui:*");

    reset_log();
    CHECK(fire(p, s, "noret", 0, &r) == -1, "missing return fails the event");
    CHECK(strstr(_log, "ERROR 3 host function 'no_return' returned success without declaring a return value"),
          "missing-return error: %s", _log);
    CHECK(strstr(_log, "exit no_return -1"), "exit traced with -1");

    reset_log();
    CHECK(fire(p, s, "cap", 0, &r) == 0, "capture event");
    CHECK(global_int(p, s, "x") == 42, "x captured = %d", global_int(p, s, "x"));
    CHECK(r.type == MTA_ARG_INT && r.val.i == 7, "'_' restored after capture, got %d", r.val.i);

    // Event-level permission: arousal_change requires arousal:read.
    reset_log();
    CHECK(fire(p, s, "arousal_change", 50, &r) == -1 && r.status == -1, "event denied");
    CHECK(!strstr(_log, "arousal"), "handler must not run: %s", _log);
    CHECK(fire(q, t, "arousal_change", 50, &r) == 0 && strstr(_log, "notify(arousal)"),
          "permitted plugin receives event");

    mta_scope_destroy(s);
    mta_scope_destroy(t);
    mta_plugin_free(p);
    free(p);
    mta_plugin_free(q);
    free(q);
}

static void test_reentrant_queue(void) {
    printf("re-entrant event queue\n");
    mta_plugin_t* p = load("{\"name\":\"reent\",\"events\":{"
                           " \"outer\":[{\"reenter\":[]},{\"record\":[1]}],"
                           " \"inner\":{\"args\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\"],"
                           "  \"actions\":[{\"record\":[\"$a\",\"$b\",\"$c\",\"$d\"]}]}}}");
    mta_scope_t* s = scope_for(p);
    mta_result_t r;

    // One re-entrant event: queued (returns 0, status -1), runs after outer's actions but
    // before outer's event-exit, with at most 4 args.
    _reenter_count = 1;
    reset_log();
    CHECK(fire(p, s, "outer", 0, &r) == 0, "outer ok");
    CHECK(_reenter_rets[0] == 0 && _reenter_status[0] == -1, "queued: ret 0, status -1");
    const char* rec_outer = strstr(_log, "HOST record(1)");
    const char* rec_inner = strstr(_log, "HOST record(10,11,12,13)");
    const char* outer_exit = strstr(_log, "evt-exit outer");
    CHECK(rec_outer && rec_inner && outer_exit, "all present: %s", _log);
    CHECK(rec_outer < rec_inner && rec_inner < outer_exit, "order outer -> inner -> outer-exit:\n%s", _log);
    CHECK(p->pending_count == 0, "queue drained");

    // Inner handler binds 6 params but only 4 args were queued: e/f unbound, 4 copied.
    // Five re-entrant events: 4 queued, the 5th dropped with a warning.
    _reenter_count = 5;
    reset_log();
    fire(p, s, "outer", 0, &r);
    int inner_runs = 0;
    for (const char* c = _log; (c = strstr(c, "evt-enter inner")); c++) inner_runs++;
    CHECK(inner_runs == 4, "4 queued events ran, got %d\n%s", inner_runs, _log);
    CHECK(strstr(_log, "HOST record(50,51,52,53)") == NULL, "5th event dropped");
    CHECK(strstr(_log, "HOST record(40,41,42,43)") != NULL, "4th event ran");
    for (int i = 0; i < 5; i++) CHECK(_reenter_rets[i] == 0, "re-entrant invoke %d returns 0", i);

    mta_scope_destroy(s);
    mta_plugin_free(p);
    free(p);
}

struct thread_arg {
    mta_plugin_t* p;
    mta_scope_t* s;
    int ret;
};

static void* thread_fire(void* v) {
    struct thread_arg* t = v;
    mta_result_t r;
    usleep(50 * 1000); // start while the main thread is inside the handler
    logf_("T2 invoke");
    t->ret = fire(t->p, t->s, "slow", 0, &r);
    logf_("T2 done ret=%d status=%d", t->ret, r.status);
    return NULL;
}

static void test_cross_task_lock(void) {
    printf("cross-task serialization\n");
    mta_plugin_t* p = load("{\"name\":\"lock\",\"events\":{\"slow\":[{\"slow\":[]}]}}");
    mta_scope_t* s = scope_for(p);
    struct thread_arg t = { p, s, -99 };
    pthread_t th;
    mta_result_t r;

    reset_log();
    pthread_create(&th, NULL, thread_fire, &t);
    CHECK(fire(p, s, "slow", 0, &r) == 0, "main fire ok");
    pthread_join(th, NULL);

    // Another task blocks on the plugin lock (it is not queued) and runs afterwards.
    const char* end1 = strstr(_log, "HOST slow end");
    const char* begin2 = end1 ? strstr(end1, "HOST slow begin") : NULL;
    CHECK(t.ret == 0, "second task ran (ret %d)", t.ret);
    CHECK(end1 && begin2, "second handler starts after first ends:\n%s", _log);
    CHECK(strstr(_log, "T2 invoke") < end1, "T2 invoked while first was running");

    mta_scope_destroy(s);
    mta_plugin_free(p);
    free(p);
}

static int count_cb_n;
static void count_cb(mta_driver_instance_t* i, void* arg) { count_cb_n++; }

static void test_driver_instances(const char* lovense_path) {
    printf("driver instances (Lovense plugin)\n");
    char* json = read_file(lovense_path);
    CHECK(json != NULL, "read %s", lovense_path);
    if (!json) return;
    mta_plugin_t* p = load(json);
    free(json);

    CHECK(p && !strcmp(mta_plugin_get_type(p), "ble_driver"), "ble_driver type");
    CHECK(!strcmp(mta_plugin_get_match_ble_name_prefix(p), "LVS-"), "ble_name_prefix");
    CHECK(mta_plugin_get_match_ble_name(p) == NULL, "no exact ble_name");

    void* device = (void*)0xD0;
    mta_driver_instance_t* inst = mta_driver_instance_create(p, device, &IO);
    CHECK(inst != NULL, "create");
    mta_scope_t* scope = mta_driver_instance_get_scope(inst);
    CHECK(mta_scope_get_user_data(scope) == inst, "scope user_data defaults to instance");
    CHECK(mta_driver_instance_get_device(inst) == device, "device");
    mta_scope_set_user_data(scope, (void*)0xB1E); // what plugin_driver.c does

    mta_driver_instance_add(inst);
    mta_driver_instance_add(inst);
    count_cb_n = 0;
    mta_driver_instance_foreach(count_cb, NULL);
    CHECK(count_cb_n == 1, "add is idempotent (%d)", count_cb_n);

    _ble_tx[0] = 0;
    reset_log();
    mta_result_t r;
    CHECK(mta_driver_instance_dispatch(inst, "connect", 0, NULL) == 0, "connect");
    CHECK(strstr(_log, "HOST log(lovense: connected)") && strstr(_log, "ud=ctx"), "connect handler:\n%s", _log);
    CHECK(mta_driver_instance_dispatch(inst, "speed_change", 255, &r) == 0, "speed 255");
    CHECK(r.type == MTA_ARG_STRING_REF && r.val.s && !strcmp(r.val.s, "Vibrate:20;"), "result '_' captured");
    mta_result_free(&r); // string results are owned by the caller
    CHECK(mta_driver_instance_dispatch(inst, "speed_change", 128, NULL) == 0, "speed 128");
    CHECK(mta_driver_instance_dispatch(inst, "speed_change", 0, NULL) == 0, "speed 0");
    CHECK(mta_driver_instance_dispatch_all("speed_change", 64) == 1, "dispatch_all handled by 1");
    CHECK(mta_driver_instance_dispatch_all("no_such_event", 1) == 0, "unhandled event");
    CHECK(mta_driver_instance_dispatch(inst, "no_such_event", 1, &r) == -1 && r.status == -1, "dispatch unhandled");
    // max_level default 20: round(speed * 20 / 255)
    CHECK(!strcmp(_ble_tx, "Battery;|Vibrate:20;|Vibrate:10;|Vibrate:0;|Vibrate:5;|"),
          "BLE TX sequence: %s", _ble_tx);

    // Config change takes effect: max_level 5 -> Vibrate:5 at full speed.
    cJSON* cfg = cJSON_Parse("{\"max_level\":5}");
    mta_plugin_set_config(p, cfg);
    cJSON_Delete(cfg);
    _ble_tx[0] = 0;
    mta_driver_instance_dispatch(inst, "speed_change", 255, NULL);
    CHECK(!strcmp(_ble_tx, "Vibrate:5;|"), "config applied: %s", _ble_tx);
    cJSON* ser = mta_plugin_serialize_config(p);
    char* sj = cJSON_PrintUnformatted(ser);
    CHECK(!strcmp(sj, "{\"max_level\":5}"), "serialize_config: %s", sj);
    free(sj);
    cJSON_Delete(ser);

    // String pool must not grow across events (runtime strings are truncated).
    uint16_t pool = p->strings.count;
    for (int i = 0; i < 200; i++) mta_driver_instance_dispatch(inst, "speed_change", i, NULL);
    CHECK(p->strings.count == pool, "string pool stable (%u -> %u)", pool, p->strings.count);

    mta_driver_instance_remove(inst);
    count_cb_n = 0;
    mta_driver_instance_foreach(count_cb, NULL);
    CHECK(count_cb_n == 0, "removed");
    CHECK(mta_driver_instance_dispatch_all("speed_change", 64) == 0, "no instances");

    reset_log();
    _closed = 0;
    mta_driver_instance_destroy(inst);
    CHECK(strstr(_log, "HOST log(lovense: disconnected)"), "disconnect fired on destroy:\n%s", _log);
    CHECK(_closed == 1 && _closed_device == device, "io close called once with device");

    mta_plugin_free(p);
    free(p);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <lovense plugin.json>\n", argv[0]);
        return 2;
    }

    mta_init_builtins();
    mta_register_system_function("notify", h_notify, "ui:notify");
    mta_register_system_function("lock_controls", h_lock, "ui:lock");
    mta_register_system_function("no_return", h_no_return, NULL);
    mta_register_system_function("answer", h_answer, NULL);
    mta_register_system_function("record", h_record, NULL);
    mta_register_system_function("reenter", h_reenter, NULL);
    mta_register_system_function("slow", h_slow, NULL);
    mta_register_system_function("ble_write", h_ble_write, "ble:write");
    mta_register_system_function("log", h_log, NULL);
    mta_register_event("arousal_change", "arousal:read");
    mta_register_event("connect", NULL);
    mta_set_action_observer(observer, NULL);
    mta_set_error_reporter(reporter, NULL);

    test_permissions();
    test_reentrant_queue();
    test_cross_task_lock();
    test_driver_instances(argv[1]);

    printf("%d/%d checks passed\n", _checks - _fails, _checks);
    return _fails ? 1 : 0;
}
