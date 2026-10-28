#include "common_tools.h"
#include "horizon_ffi_closure.h"
#include "jvm.h"
#include <EGL/egl.h>
#include <string.h>

JNIEXPORT jlong JNICALL Java_org_lwjgl_system_HorizonLibrary_nOpen(JNIEnv* env,jclass clazz,jstring name) {
    (void)clazz;
    if (!name) return 0;
    const char* text=(*env)->GetStringUTFChars(env,name,NULL);
    if (!text) return 0;
    void* handle=JVM_LoadLibrary(text,JNI_FALSE);
    (*env)->ReleaseStringUTFChars(env,name,text);
    return (jlong)(uintptr_t)handle;
}
JNIEXPORT jlong JNICALL Java_org_lwjgl_system_HorizonLibrary_nSymbol(JNIEnv* env,jclass clazz,jlong value,jlong address) {
    (void)env; (void)clazz;
    void* handle=(void*)(uintptr_t)value;
    const char* name=(const char*)(uintptr_t)address;
    if (!handle || !name) return 0;
    void* function=JVM_FindLibraryEntry(handle,name);
    if (!function && handle==JVM_LoadLibrary("GL",JNI_FALSE))
        function=(void*)eglGetProcAddress(name);
    return (jlong)(uintptr_t)function;
}
// Static NRO libraries have no independent filesystem path. SharedLibraryUtil
// documents zero as unavailable; callers still receive real registry handles.
JNIEXPORT jint JNICALL Java_org_lwjgl_system_SharedLibraryUtil_getLibraryPath(JNIEnv* env,jclass clazz,jlong handle,jlong output,jint size) {
    (void)env; (void)clazz; (void)handle; (void)output; (void)size;
    return 0;
}

JNIEXPORT jlongArray JNICALL Java_org_lwjgl_system_HorizonLibrary_nClosureStats(JNIEnv* env,jclass clazz) {
    (void)clazz;
    size_t live,pools; horizon_ffi_closure_counts(&live,&pools);
    jlong values[2]={(jlong)live,(jlong)pools};
    jlongArray result=(*env)->NewLongArray(env,2);
    if (result) (*env)->SetLongArrayRegion(env,result,0,2,values);
    return result;
}
