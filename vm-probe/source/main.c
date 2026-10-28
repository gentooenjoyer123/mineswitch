#include <errno.h>
#include <ffi.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_SEC UINT64_C(1000000000)
#define PROBE_VERSION "0.1.0"

static FILE *network_log;
static unsigned passed;
static unsigned failed;
static bool must_exit_process;

static void report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);

    if (network_log) {
        va_start(args, format);
        vfprintf(network_log, format, args);
        va_end(args);
        fflush(network_log);
    }
}

static void check(const char *name, bool success) {
    if (success) {
        ++passed;
    } else {
        ++failed;
    }
    report("%s: %s\n", name, success ? "PASS" : "FAIL");
}

static bool check_errno(const char *name, int error) {
    check(name, error == 0);
    if (error != 0) {
        report("  error=%d (%s)\n", error, strerror(error));
    }
    return error == 0;
}

static uint64_t elapsed_ns(uint64_t start) {
    return armTicksToNs(armGetSystemTick() - start);
}

static int deadline_after_ms(struct timespec *deadline, unsigned ms) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
        return errno;
    }
    deadline->tv_sec += ms / 1000;
    deadline->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        ++deadline->tv_sec;
    }
    return 0;
}

static int lock_with_deadline(pthread_mutex_t *mutex, unsigned ms) {
    uint64_t start = armGetSystemTick();
    int error;
    do {
        error = pthread_mutex_trylock(mutex);
        if (error != EBUSY) {
            return error;
        }
        svcSleepThread(NS_PER_MS);
    } while (elapsed_ns(start) < ms * NS_PER_MS);
    return ETIMEDOUT;
}

/* These are ordinary C functions. No JVM, JNIEnv or JNI is present. */
static int64_t ffi_integer_target(int64_t a, int32_t b, void *payload) {
    return a * 3 + b + *(const int64_t *)payload;
}

static double ffi_double_target(double a, double b, int64_t multiplier) {
    return a * (double)multiplier + b;
}

static int64_t ffi_stack_target(int64_t a, int64_t b, int64_t c, int64_t d,
                                int64_t e, int64_t f, int64_t g, int64_t h,
                                int64_t i, int64_t j) {
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j;
}

static void probe_ffi(void) {
    report("libffi 3.4.8: static ffi_call ABI checks\n");
    ffi_cif cif;
    ffi_status status;

    int64_t integer_a = INT64_C(0x100000002);
    int32_t integer_b = -19;
    int64_t payload = 55;
    void *payload_pointer = &payload;
    int64_t integer_result = 0;
    ffi_type *integer_types[] = {
        &ffi_type_sint64, &ffi_type_sint32, &ffi_type_pointer
    };
    void *integer_args[] = {&integer_a, &integer_b, &payload_pointer};
    status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_sint64,
                          integer_types);
    check("ffi.integer_pointer.prepare", status == FFI_OK);
    if (status == FFI_OK) {
        ffi_call(&cif, FFI_FN(ffi_integer_target), &integer_result, integer_args);
        check("ffi.integer_pointer.call", integer_result == INT64_C(12884901930));
        report("  integer result=%" PRId64 " expected=12884901930\n", integer_result);
    }

    double double_a = 1.25;
    double double_b = -6.5;
    int64_t multiplier = 3;
    double double_result = 0;
    ffi_type *double_types[] = {
        &ffi_type_double, &ffi_type_double, &ffi_type_sint64
    };
    void *double_args[] = {&double_a, &double_b, &multiplier};
    status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_double, double_types);
    check("ffi.double_integer.prepare", status == FFI_OK);
    if (status == FFI_OK) {
        ffi_call(&cif, FFI_FN(ffi_double_target), &double_result, double_args);
        check("ffi.double_integer.call", double_result == -2.75);
        report("  double result=%.2f expected=-2.75\n", double_result);
    }

    int64_t stack_values[10];
    ffi_type *stack_types[10];
    void *stack_args[10];
    for (unsigned i = 0; i < 10; ++i) {
        stack_values[i] = (int64_t)i + 1;
        stack_types[i] = &ffi_type_sint64;
        stack_args[i] = &stack_values[i];
    }
    int64_t stack_result = 0;
    status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 10, &ffi_type_sint64, stack_types);
    check("ffi.stack_arguments.prepare", status == FFI_OK);
    if (status == FFI_OK) {
        ffi_call(&cif, FFI_FN(ffi_stack_target), &stack_result, stack_args);
        check("ffi.stack_arguments.call", stack_result == 385);
        report("  ten-argument result=%" PRId64 " expected=385\n", stack_result);
    }
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_key_t tls_key;
    atomic_uint native_handle;
    atomic_bool waiting;
    atomic_uint tls_destructors;
    atomic_bool destructor_value_ok;
    bool release_worker; /* Protected by mutex. */
    uint32_t message;    /* Protected by mutex. */
    int tls_set_error;
    int lock_error;
    int wait_error;
    int timeout_error;
    int unlock_error;
    bool tls_initially_empty;
    bool tls_roundtrip_ok;
    bool handshake_ok;
    uint64_t timeout_elapsed_ns;
} ThreadProbe;

/* Globals survive until the process exits, including a failed worker timeout. */
static ThreadProbe thread_probe;
static uint32_t main_tls_token;
static uint32_t worker_tls_token;

static void tls_destructor(void *value) {
    atomic_store_explicit(&thread_probe.destructor_value_ok,
                          value == &worker_tls_token, memory_order_relaxed);
    atomic_fetch_add_explicit(&thread_probe.tls_destructors, 1, memory_order_relaxed);
}

static void *pthread_worker(void *argument) {
    ThreadProbe *probe = argument;
    Thread *native_thread = threadGetSelf();
    if (native_thread) {
        atomic_store_explicit(&probe->native_handle, native_thread->handle,
                              memory_order_release);
    }

    probe->tls_initially_empty = pthread_getspecific(probe->tls_key) == NULL;
    probe->tls_set_error = pthread_setspecific(probe->tls_key, &worker_tls_token);
    probe->tls_roundtrip_ok = pthread_getspecific(probe->tls_key) == &worker_tls_token;
    probe->lock_error = lock_with_deadline(&probe->mutex, 1000);
    if (probe->lock_error != 0) {
        return probe;
    }

    struct timespec deadline;
    probe->wait_error = deadline_after_ms(&deadline, 1500);
    if (probe->wait_error == 0) {
        atomic_store_explicit(&probe->waiting, true, memory_order_release);
        while (!probe->release_worker) {
            probe->wait_error = pthread_cond_timedwait(&probe->condition,
                                                       &probe->mutex, &deadline);
            if (probe->wait_error != 0) {
                break;
            }
        }
        probe->handshake_ok = probe->wait_error == 0 && probe->release_worker &&
                              probe->message == UINT32_C(0x6d63766d);
    }

    /* No further signals are sent. Verify a real timed-out wait and relock. */
    probe->timeout_error = deadline_after_ms(&deadline, 200);
    if (probe->timeout_error == 0) {
        uint64_t start = armGetSystemTick();
        do {
            probe->timeout_error = pthread_cond_timedwait(&probe->condition,
                                                          &probe->mutex, &deadline);
        } while (probe->timeout_error == 0 && elapsed_ns(start) < NS_PER_SEC);
        probe->timeout_elapsed_ns = elapsed_ns(start);
    }
    probe->unlock_error = pthread_mutex_unlock(&probe->mutex);
    return probe;
}

static bool wait_until_ready(ThreadProbe *probe) {
    uint64_t start = armGetSystemTick();
    do {
        if (atomic_load_explicit(&probe->waiting, memory_order_acquire)) {
            return true;
        }
        svcSleepThread(NS_PER_MS);
    } while (elapsed_ns(start) < NS_PER_SEC);
    return false;
}

static void probe_pthread(void) {
    ThreadProbe *probe = &thread_probe;
    bool mutex_created = false;
    bool condition_created = false;
    bool key_created = false;
    pthread_condattr_t condition_attributes;
    pthread_attr_t thread_attributes;
    pthread_t thread;

    struct timespec initial_clock;
    if (!check_errno("pthread.clock.monotonic",
                      clock_gettime(CLOCK_MONOTONIC, &initial_clock) == 0 ? 0 : errno)) {
        return;
    }
    if (!check_errno("pthread.mutex.init", pthread_mutex_init(&probe->mutex, NULL))) {
        return;
    }
    mutex_created = true;
    if (!check_errno("pthread.condattr.init", pthread_condattr_init(&condition_attributes))) {
        goto cleanup;
    }
    int error = pthread_condattr_setclock(&condition_attributes, CLOCK_MONOTONIC);
    if (!check_errno("pthread.condattr.monotonic", error)) {
        pthread_condattr_destroy(&condition_attributes);
        goto cleanup;
    }
    error = pthread_cond_init(&probe->condition, &condition_attributes);
    check_errno("pthread.condattr.destroy", pthread_condattr_destroy(&condition_attributes));
    if (!check_errno("pthread.condition.init", error)) {
        goto cleanup;
    }
    condition_created = true;
    if (!check_errno("pthread.tls.key_create", pthread_key_create(&probe->tls_key, tls_destructor))) {
        goto cleanup;
    }
    key_created = true;
    if (!check_errno("pthread.tls.main_set", pthread_setspecific(probe->tls_key, &main_tls_token))) {
        goto cleanup;
    }
    if (!check_errno("pthread.attr.init", pthread_attr_init(&thread_attributes))) {
        goto cleanup;
    }
    error = pthread_attr_setguardsize(&thread_attributes, 4096);
    report("pthread.attr.guard_page: rc=%d (%s); capability only\n", error,
           error == 0 ? "supported by API" : strerror(error));
    error = pthread_attr_setstacksize(&thread_attributes, 128 * 1024);
    if (!check_errno("pthread.attr.stack_128k", error)) {
        pthread_attr_destroy(&thread_attributes);
        goto cleanup;
    }
    error = pthread_create(&thread, &thread_attributes, pthread_worker, probe);
    check_errno("pthread.attr.destroy", pthread_attr_destroy(&thread_attributes));
    if (!check_errno("pthread.create", error)) {
        goto cleanup;
    }

    bool worker_ready = wait_until_ready(probe);
    check("pthread.worker.ready", worker_ready);
    if (worker_ready) {
        error = lock_with_deadline(&probe->mutex, 1000);
        if (error == 0) {
            probe->message = UINT32_C(0x6d63766d);
            probe->release_worker = true;
            int signal_error = pthread_cond_signal(&probe->condition);
            int unlock_error = pthread_mutex_unlock(&probe->mutex);
            check_errno("pthread.mutex.main_lock", error);
            check_errno("pthread.condition.signal", signal_error);
            check_errno("pthread.mutex.main_unlock", unlock_error);
        } else {
            check_errno("pthread.mutex.main_lock", error);
        }
    }

    Handle native_handle = atomic_load_explicit(&probe->native_handle, memory_order_acquire);
    check("pthread.worker.native_handle", native_handle != INVALID_HANDLE);
    Result wait_result = native_handle != INVALID_HANDLE
        ? waitSingleHandle(native_handle, 3 * NS_PER_SEC) : KERNELRESULT(InvalidHandle);
    check("pthread.worker.exit_within_3s", R_SUCCEEDED(wait_result));
    if (R_FAILED(wait_result)) {
        report("  thread wait result=0x%08" PRIx32 "\n", (uint32_t)wait_result);
        report("Worker did not exit; preserving its state and using process exit.\n");
        must_exit_process = true;
        return;
    }

    /* The native exit signal includes TLS destructors. Join cannot wait on work now. */
    void *thread_result = NULL;
    error = pthread_join(thread, &thread_result);
    if (!check_errno("pthread.join", error)) {
        must_exit_process = true;
        return;
    }
    check("pthread.return_value", thread_result == probe);
    check_errno("pthread.worker.tls_set", probe->tls_set_error);
    check("pthread.tls.initial_isolation", probe->tls_initially_empty);
    check("pthread.tls.worker_roundtrip", probe->tls_roundtrip_ok);
    check("pthread.tls.main_unchanged", pthread_getspecific(probe->tls_key) == &main_tls_token);
    check("pthread.tls.destructor", atomic_load(&probe->tls_destructors) == 1 &&
                                    atomic_load(&probe->destructor_value_ok));
    check_errno("pthread.worker.mutex_lock", probe->lock_error);
    check_errno("pthread.worker.condition_wake", probe->wait_error);
    check("pthread.condition.handshake", probe->handshake_ok);
    check("pthread.condition.timeout", probe->timeout_error == ETIMEDOUT);
    check("pthread.condition.timeout_duration", probe->timeout_elapsed_ns >= 150 * NS_PER_MS &&
                                                probe->timeout_elapsed_ns < NS_PER_SEC);
    report("  timeout rc=%d duration=%.2f ms (requested 200 ms)\n", probe->timeout_error,
           (double)probe->timeout_elapsed_ns / NS_PER_MS);
    check_errno("pthread.worker.mutex_unlock", probe->unlock_error);

cleanup:
    if (key_created) {
        check_errno("pthread.tls.main_clear", pthread_setspecific(probe->tls_key, NULL));
        check_errno("pthread.tls.key_delete", pthread_key_delete(probe->tls_key));
    }
    if (condition_created) {
        check_errno("pthread.condition.destroy", pthread_cond_destroy(&probe->condition));
    }
    if (mutex_created) {
        check_errno("pthread.mutex.destroy", pthread_mutex_destroy(&probe->mutex));
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("MineSwitch VM dependency probe %s\nConnecting NXLink...\n", PROBE_VERSION);
    consoleUpdate(NULL);

    Result socket_result = socketInitializeDefault();
    int network_error = 0;
    if (R_SUCCEEDED(socket_result)) {
        int descriptor = nxlinkConnectToHost(false, false);
        if (descriptor >= 0) {
            network_log = fdopen(descriptor, "w");
            if (!network_log) {
                network_error = errno;
                close(descriptor);
            }
        } else {
            network_error = errno;
        }
    }

    report("MINESWITCH_VM_PROBE_BEGIN version=%s\n", PROBE_VERSION);
    report("C runtime dependencies only: no Java, JVM or JNI executed.\n");
    report("socket init=0x%08" PRIx32 "; NXLink=%s errno=%d\n", (uint32_t)socket_result,
           network_log ? "CONNECTED" : "UNAVAILABLE", network_error);
    report("Applet type=%d (application=%d)\n", (int)appletGetAppletType(),
           (int)AppletType_Application);
    probe_ffi();
    probe_pthread();
    report("MINESWITCH_VM_PROBE_CHECKS_COMPLETE passed=%u failed=%u\n", passed, failed);
    report("Press + to exit; automatic %s in 10 seconds.\n",
           must_exit_process ? "process exit" : "return");

    uint64_t start = armGetSystemTick();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) || elapsed_ns(start) >= 10 * NS_PER_SEC) {
            break;
        }
        consoleUpdate(NULL);
    }
    report("MINESWITCH_VM_PROBE_END passed=%u failed=%u exit=%s\n", passed, failed,
           must_exit_process ? "process" : "loader");
    if (network_log) {
        FILE *finished_log = network_log;
        network_log = NULL;
        fclose(finished_log);
    }
    if (R_SUCCEEDED(socket_result)) {
        socketExit();
    }
    consoleExit(NULL);
    if (must_exit_process) {
        /* Do not return to a homebrew loader while a worker may still be alive. */
        svcExitProcess();
    }
    return 0;
}
