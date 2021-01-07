#include <jni.h>
#include <string>
#include <android/log.h>


// open sl es
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

static const char* kTag = "native_lib : native_code" ;
#define LOGI(...) \
 ((void) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__))
#define LOGW(...) \
 ((void) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__))
#define LOGE(...) \
 ((void) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__))


jstring
Java_com_example_nextu_cmakeappmetro_MainActivity_stringFromJNI(
        JNIEnv *env,
        jobject instance ) {
    std::string hello = "Hello from C++ , bouh";
    LOGI("logging from native") ;
    return env->NewStringUTF(hello.c_str());
}

jint JNI_OnLoad(JavaVM* vm, void* reserved){
    JNIEnv* env ;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6) != JNI_OK) {
        return -1 ;
    }

    return JNI_VERSION_1_6 ;
}
