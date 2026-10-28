/* Host test adapter only; the target registry finds the production JNI symbol. */
#include <jni.h>
#include <stdatomic.h>
#include <mbedtls/version.h>
#include <stdio.h>

extern jint JNICALL Java_sun_security_provider_SHA5_implCompressBlocksHorizon0(
        JNIEnv *, jclass, jbyteArray, jint, jint, jlongArray);
static _Atomic unsigned long long invocation_count;

static jint JNICALL tracked_compress(JNIEnv *env, jclass cls, jbyteArray input,
                                    jint offset, jint limit, jlongArray state) {
    atomic_fetch_add(&invocation_count, 1);
    return Java_sun_security_provider_SHA5_implCompressBlocksHorizon0(
            env, cls, input, offset, limit, state);
}

JNIEXPORT jlong JNICALL Java_Sha5HostRunner_calls(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return (jlong)atomic_load(&invocation_count);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    jclass sha5;
    JNINativeMethod method = { "implCompressBlocksHorizon0", "([BII[J)I",
                              (void *)&tracked_compress };
    (void)reserved;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) return JNI_ERR;
    sha5 = (*env)->FindClass(env, "sun/security/provider/SHA5");
    if (sha5 == NULL) return JNI_ERR;
    if ((*env)->RegisterNatives(env, sha5, &method, 1) != JNI_OK) return JNI_ERR;
    (*env)->DeleteLocalRef(env, sha5);
    fprintf(stderr, "SHA5_HOST_MBEDTLS_VERSION=%s\n", MBEDTLS_VERSION_STRING);
    return JNI_VERSION_1_8;
}
