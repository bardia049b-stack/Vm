/*
 * rvm_jni.c -- the Android shim over the RVM core.
 *
 * Two things cross the thread boundary, and neither uses a lock:
 *
 *   - Console output flows VM thread -> UI thread.  The VM already line-buffers
 *     it, so the callback fires a handful of times a second at most.
 *   - Keystrokes flow UI thread -> VM thread through a single-producer /
 *     single-consumer ring buffer built on C11 atomics.  One writer, one
 *     reader, so release/acquire on the two indices is sufficient and the VM
 *     never blocks on a mutex in the middle of an instruction.
 *
 * The VM thread is a Java thread, so it is already attached to the JVM and
 * GetEnv() always succeeds there; no AttachCurrentThread dance is needed.
 *
 * SPDX-License-Identifier: MIT
 */
#include <android/log.h>
#include <jni.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "rvm.h"
#include "vm/vm.h"

#define LOG_TAG   "rvm"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Exported JNI entry points keep default visibility even under
 * -fvisibility=hidden; everything else in this translation unit does not. */
#define JNI_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------ input ring */

#define RING_CAP  8192 /* must be a power of two */
#define RING_MASK (RING_CAP - 1)

typedef struct {
    u8 buf[RING_CAP];
    /* Written by the producer, read by the consumer, and vice versa.  Padding
     * keeps them off the same cache line so a burst of keys does not ping-pong. */
    _Alignas(64) atomic_size_t head; /* next byte the VM will read   */
    _Alignas(64) atomic_size_t tail; /* next byte the UI will write  */
} input_ring;

static void ring_init(input_ring *r) {
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

/* Returns how many bytes were queued; drops the rest rather than blocking the
 * UI thread on a guest that is not reading. */
static size_t ring_push(input_ring *r, const u8 *src, size_t n) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    size_t free_ = RING_CAP - (tail - head);
    if (n > free_)
        n = free_;
    for (size_t i = 0; i < n; i++)
        r->buf[(tail + i) & RING_MASK] = src[i];
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    return n;
}

/* vm_poll_fn: called by the VM when the guest's serial FIFO has room. */
static int ring_poll(void *ud, u8 *dst, size_t max) {
    input_ring *r = (input_ring *)ud;
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t avail = tail - head;
    if (avail == 0)
        return 0;
    if (avail > max)
        avail = max;
    for (size_t i = 0; i < avail; i++)
        dst[i] = r->buf[(head + i) & RING_MASK];
    atomic_store_explicit(&r->head, head + avail, memory_order_release);
    return (int)avail;
}

/*
 * rvm_log_fn: one line of RVM's own log to RvmNative.onLog(int, String).
 * Without this the logs go to stderr, which on Android is /dev/null, and a
 * boot that goes wrong says nothing at all.
 */
static void jni_log(void *ud, rvm_loglevel lvl, const char *line) {
    session *s = ud;
    JNIEnv *env = NULL;
    if (s->onLog == NULL)
        return;
    if ((*s->jvm)->GetEnv(s->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL)
        return;
    jstring js = (*env)->NewStringUTF(env, line);
    if (js == NULL) {
        (*env)->ExceptionClear(env);
        return;
    }
    (*env)->CallStaticVoidMethod(env, s->cls, s->onLog, (jint)lvl, js);
    (*env)->DeleteLocalRef(env, js);
}

/* --------------------------------------------------------------- session */

typedef struct {
    vm vm;
    input_ring ring;

    JavaVM *jvm;
    jclass cls; /* global ref to dev.rvm.app.RvmNative */
    jmethodID onOutput;
    jmethodID onLog; /* NULL when the app did not ask for a debug log */

    /* Reused across calls so a chatty guest does not allocate per line. */
    jbyteArray scratch;
    size_t scratch_len;

} session;

/* vm_write_fn: called on the VM thread with one line-buffered chunk. */
static void jni_write(void *ud, const u8 *buf, size_t n) {
    session *s = (session *)ud;
    if (n == 0 || s->cls == NULL)
        return;

    JNIEnv *env = NULL;
    if ((*s->jvm)->GetEnv(s->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        LOGE("jni_write: VM thread is not attached");
        return;
    }

    if (s->scratch == NULL || s->scratch_len < n) {
        if (s->scratch != NULL)
            (*env)->DeleteGlobalRef(env, s->scratch);
        size_t cap = n < 4096 ? 4096 : n;
        jbyteArray local = (*env)->NewByteArray(env, (jsize)cap);
        if (local == NULL)
            return; /* out of memory: drop the output */
        s->scratch = (jbyteArray)(*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        if (s->scratch == NULL)
            return;
        s->scratch_len = cap;
    }

    (*env)->SetByteArrayRegion(env, s->scratch, 0, (jsize)n, (const jbyte *)buf);
    (*env)->CallStaticVoidMethod(env, s->cls, s->onOutput, s->scratch, (jint)n);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
}

/* --------------------------------------------------------------- helpers */

static char *dup_string(JNIEnv *env, jstring js) {
    if (js == NULL)
        return NULL;
    const char *c = (*env)->GetStringUTFChars(env, js, NULL);
    if (c == NULL)
        return NULL;
    char *out = strdup(c);
    (*env)->ReleaseStringUTFChars(env, js, c);
    return out;
}

static void notify_exit(session *s, int code) {
    JNIEnv *env = NULL;
    if (s->cls == NULL)
        return;
    if ((*s->jvm)->GetEnv(s->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return;
    jmethodID m = (*env)->GetStaticMethodID(env, s->cls, "onVmExit", "(I)V");
    if (m != NULL)
        (*env)->CallStaticVoidMethod(env, s->cls, m, (jint)code);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
}

/* ------------------------------------------------------------------- JNI */

static JavaVM *g_jvm;

JNI_EXPORT jint JNI_OnLoad(JavaVM *jvm, void *reserved) {
    (void)reserved;
    g_jvm = jvm;
    LOGI("librvm_jni loaded");
    return JNI_VERSION_1_6;
}

JNI_EXPORT jlong JNICALL Java_dev_rvm_app_RvmNative_vmCreate(JNIEnv *env, jclass cls,
                                                             jstring kernel, jstring disk,
                                                             jstring initrd, jint ramMib,
                                                             jstring bootargs, jboolean trace) {
    session *s = calloc(1, sizeof *s);
    if (s == NULL)
        return 0;

    ring_init(&s->ring);
    s->jvm = g_jvm;

    s->cls = (jclass)(*env)->NewGlobalRef(env, cls);
    if (s->cls == NULL) {
        free(s);
        return 0;
    }
    s->onLog = (*env)->GetStaticMethodID(env, s->cls, "onLog", "(ILjava/lang/String;)V");
    if (s->onLog == NULL)
        (*env)->ExceptionClear(env); /* older Java side: logs stay off */

    s->onOutput = (*env)->GetStaticMethodID(env, s->cls, "onConsoleOutput", "([BI)V");
    if (s->onOutput == NULL) {
        LOGE("onConsoleOutput([BI)V not found -- is proguard keeping RvmNative?");
        (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, s->cls);
        free(s);
        return 0;
    }

    vm_opts o;
    vm_opts_default(&o);

    /* Owned by the session and freed in vmFree; vm_opts only borrows them. */
    char *k = dup_string(env, kernel);
    char *d = dup_string(env, disk);
    char *i = dup_string(env, initrd);
    char *b = dup_string(env, bootargs);

    o.kernel_path = k;
    o.disk_path = d;
    o.initrd_path = i;
    o.bootargs = b;
    o.dtb_path = NULL; /* always built in-process: dtc does not exist on-device */
    o.ram_size = (u64)(ramMib > 0 ? ramMib : 512) << 20;
    o.create_disk = false;
    /*
     * On Android the flag means "give me the log", not "give me the
     * instruction trace": a full boot trace is gigabytes and would fill the
     * phone's storage before it said anything useful.
     */
    o.trace = false;
    o.log_level = trace ? RVM_LOG_DEBUG : RVM_LOG_WARN;
    if (trace && s->onLog != NULL)
        rvm_log_set_sink(jni_log, s);
    else
        rvm_log_set_sink(NULL, NULL);
    o.write = jni_write;
    o.write_ud = s;
    o.poll = ring_poll;
    o.poll_ud = &s->ring;

    rvm_err e = vm_new(&s->vm, &o);
    if (e != RVM_OK) {
        LOGE("vm_new failed: %d", (int)e);
        free(k);
        free(d);
        free(i);
        free(b);
        (*env)->DeleteGlobalRef(env, s->cls);
        free(s);
        return 0;
    }

    /* The option strings outlive vm_new; keep the pointers so vmFree can
     * release them without reaching into the vm struct. */
    s->vm.opts.kernel_path = k;
    s->vm.opts.disk_path = d;
    s->vm.opts.initrd_path = i;
    s->vm.opts.bootargs = b;

    e = vm_load(&s->vm);
    if (e != RVM_OK) {
        LOGE("vm_load failed: %d", (int)e);
        jni_write(s, (const u8 *)"\r\n[error] failed to load the kernel\r\n", 38);
    }

    LOGI("session %p: ram=%llu MiB kernel=%s disk=%s", (void *)s,
         (unsigned long long)(o.ram_size >> 20), k ? k : "(none)", d ? d : "(none)");
    return (jlong)(intptr_t)s;
}

JNI_EXPORT jint JNICALL Java_dev_rvm_app_RvmNative_vmRun(JNIEnv *env, jclass cls, jlong handle) {
    (void)env;
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    if (s == NULL)
        return -1;
    rvm_err e = vm_run(&s->vm);
    int code = (e == RVM_OK) ? (int)s->vm.exit_code : -(int)e;
    notify_exit(s, code);
    return code;
}

JNI_EXPORT void JNICALL Java_dev_rvm_app_RvmNative_vmStop(JNIEnv *env, jclass cls, jlong handle) {
    (void)env;
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    if (s == NULL)
        return;
    vm_stop(&s->vm, 0); /* flips the atomic vm.running flag the loop polls */
}

JNI_EXPORT void JNICALL Java_dev_rvm_app_RvmNative_vmFree(JNIEnv *env, jclass cls, jlong handle) {
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    if (s == NULL)
        return;
    vm_free(&s->vm);
    free((void *)s->vm.opts.kernel_path);
    free((void *)s->vm.opts.disk_path);
    free((void *)s->vm.opts.initrd_path);
    free((void *)s->vm.opts.bootargs);
    if (s->scratch != NULL)
        (*env)->DeleteGlobalRef(env, s->scratch);
    if (s->cls != NULL)
        (*env)->DeleteGlobalRef(env, s->cls);
    /* The log sink is process-global and the app runs one VM at a time, so
     * dropping it here is what keeps it from calling into a freed session. */
    rvm_log_set_sink(NULL, NULL);
    free(s);
}

JNI_EXPORT void JNICALL Java_dev_rvm_app_RvmNative_vmInput(JNIEnv *env, jclass cls, jlong handle,
                                                           jbyteArray data, jint len) {
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    if (s == NULL || data == NULL || len <= 0)
        return;
    jbyte tmp[256];
    jbyte *p = tmp;
    if ((size_t)len > sizeof tmp) {
        p = (jbyte *)malloc((size_t)len);
        if (p == NULL)
            return;
    }
    (*env)->GetByteArrayRegion(env, data, 0, len, p);
    if (!(*env)->ExceptionCheck(env))
        ring_push(&s->ring, (const u8 *)p, (size_t)len);
    else
        (*env)->ExceptionClear(env);
    if (p != tmp)
        free(p);
}

JNI_EXPORT void JNICALL Java_dev_rvm_app_RvmNative_vmInterrupt(JNIEnv *env, jclass cls,
                                                               jlong handle) {
    (void)env;
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    if (s == NULL)
        return;
    const u8 etx = 3; /* Ctrl-C */
    ring_push(&s->ring, &etx, 1);
}

JNI_EXPORT jlong JNICALL Java_dev_rvm_app_RvmNative_vmInsns(JNIEnv *env, jclass cls, jlong handle) {
    (void)env;
    (void)cls;
    session *s = (session *)(intptr_t)handle;
    return (s == NULL) ? 0 : (jlong)s->vm.insns;
}
