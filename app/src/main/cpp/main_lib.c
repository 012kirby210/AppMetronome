//
// Created by NextU on 23/09/2016.
//

#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>
#include <assert.h>
#include <pthread.h>

// open sl es
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
//#include "../../../../../../../../AppData/Local/Android/sdk/ndk-bundle/platforms/android-15/arch-arm/usr/include/linux/time.h"

#define RINGBUFFER_FRAME_NUMBER 64
#define MSIG_METRONOME SIGRTMIN
#define MSIG_CLOCK SIGRTMIN+1
#define MCLOCKID CLOCK_MONOTONIC

typedef struct ring_buffer {
    char** buffer ;
    int write_offset,read_offset,step,size ;
} RingBuffer;

typedef struct sound_sample{
    char* fractionRawSound ;
    char* measureRawSound ;
    int fractionSoundSize,measureSoundSize,sampleRate ;
    SLDataFormat_PCM pcmFormat ;
} SoundSample ;

typedef struct device_spec {
    int sampleRate, defaultBufferFrame ;
} DeviceSpec;

typedef struct time_parameter {
    int tempoValue ;
    double realTempoValue ;
    int denominateurValue,numerateurValue ;
    double tickTimeInS,frameTimeInS ;
    int tickTimeInFrame,tickTimeInOctet,measureTimeInOctet ;
    int beatCounter ;
} TimeParameter;

typedef struct java_context{
    JavaVM *vm ;
    jclass mainActivityClass ;
    jobject mainActivityInstance ;
} JavaContext ;

typedef struct opensl_context {
    SLObjectItf engineObject ;
    SLObjectItf outputMixObject ;
    SLObjectItf playerObject ;

    SLEngineItf engineInterface ;
    SLPlayItf playInterface ;
    SLAndroidSimpleBufferQueueItf queueInterface ;
    int isPlaying ;

} OpenSLContext ;

typedef struct timer_context {
    pthread_t metronomeThread, clockThread ;
    pthread_attr_t metronomeAttrThread,clockAttrThread ;
    pthread_cond_t metronomeCondition,clockCondition ;
    pthread_mutex_t metronomeMutex, clockMutex ;
    timer_t metronomeTimer,clockTimer ;
    struct itimerspec metronomeTimerSpec,clockTimerSpec ;
    struct sigaction metronomeSigAction,clockSigAction ;
    struct sigevent metronomeSigEvent,clockSigEvent ;
    int metronomeTimerDisarmed,clockTimerDisarmed ;

} timerContext ;

typedef struct thread_context{
    pthread_attr_t refill_thread_attr ;
    pthread_t refill_thread_id ;
} ThreadContext ;

typedef struct buffering_Context{
    char* buffer[2] ;
    int bufferSelector,bufferSizeInOctet ;
    int weakTimeOffset,strongTimeOffset ;
} BufferingContext ;

typedef struct native_context {
    OpenSLContext mOpenSlContext ;
    TimeParameter mTimeParameter ;
    SoundSample mSoundSample ;
    DeviceSpec mDeviceSpec ;
    RingBuffer mRingBuffer ;
    ThreadContext mThreadContext ;
    BufferingContext mBufferingContext ;
    JavaContext mJavaContext ;
    timerContext mTimerContext ;
    pthread_mutex_t antiCorruptLock ;

} NativeContext ;

NativeContext appContext ;

// vite fait :
char* next_buffer_to_enqueue ;
int current_offset ;

static const char* kTag = "Main_lib , native_code" ;
#define LOGI(...) \
 ((void) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__))
#define LOGW(...) \
 ((void) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__))
#define LOGE(...) \
 ((void) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__))

// Helpers functions :
void computeTimingContext(JNIEnv *env,jobject instance) {
    // get the time parameter , compute the timing context
    jclass mainActivity_class = (*env)->GetObjectClass(env, instance);
    jfieldID numerateur_id = (*env)->GetFieldID(env, mainActivity_class, "numerateur_value", "I");
    jfieldID denominateur_id = (*env)->GetFieldID(env, mainActivity_class, "denominateur_value",
                                                  "I");
    jfieldID tempo_id = (*env)->GetFieldID(env, mainActivity_class, "tempo", "I");
    jint numerateur_value = (*env)->GetIntField(env, instance, numerateur_id);
    jint denominateur_value = (*env)->GetIntField(env, instance, denominateur_id);
    jint tempo_value = (*env)->GetIntField(env, instance, tempo_id);

    //appContext.mJavaContext.mainActivityClass = (*env)->NewGlobalRef(env, mainActivity_class);
    //appContext.mJavaContext.mainActivityInstance = (*env)->NewGlobalRef(env, instance);

    appContext.mTimeParameter.numerateurValue = numerateur_value;
    appContext.mTimeParameter.denominateurValue = denominateur_value;
    appContext.mTimeParameter.tempoValue = tempo_value;

    appContext.mTimeParameter.frameTimeInS = 1.0 / appContext.mDeviceSpec.sampleRate;
    appContext.mTimeParameter.tickTimeInS = 60.0 / tempo_value;
    appContext.mTimeParameter.tickTimeInS *= 4;
    appContext.mTimeParameter.tickTimeInS /= appContext.mTimeParameter.denominateurValue;
    LOGI("valeur denominateur : %d", appContext.mTimeParameter.denominateurValue);
    double intermediate_value =
            appContext.mTimeParameter.tickTimeInS / appContext.mTimeParameter.frameTimeInS;
    appContext.mTimeParameter.tickTimeInFrame = (int) intermediate_value;
    // 2 octet for 2 channels as in datasourcelocator stand for pcm stereo 16 bits
    appContext.mTimeParameter.tickTimeInOctet = appContext.mTimeParameter.tickTimeInFrame * 2 * 2;
    appContext.mTimeParameter.realTempoValue =  (60.0 / (appContext.mTimeParameter.tickTimeInFrame * appContext.mTimeParameter.frameTimeInS));
    appContext.mTimeParameter.realTempoValue *= 4 ;
    appContext.mTimeParameter.realTempoValue /= appContext.mTimeParameter.denominateurValue ;

    appContext.mTimeParameter.measureTimeInOctet = numerateur_value*appContext.mTimeParameter.tickTimeInOctet ;

}

void fillBufferToEnqueue(int bufferToFill){

    int i,bufferSize,tickTimeInOctet,measureTimeInOctet ;
    bufferSize = appContext.mBufferingContext.bufferSizeInOctet ;
    tickTimeInOctet = appContext.mTimeParameter.tickTimeInOctet ;
    measureTimeInOctet = appContext.mTimeParameter.measureTimeInOctet ;

    short measureSoundCell,fractionSoundCell,resultCell ;

    for( i=0 ; i<bufferSize ; i+=2 , appContext.mBufferingContext.weakTimeOffset+=2, appContext.mBufferingContext.strongTimeOffset+=2){
        if(appContext.mBufferingContext.weakTimeOffset<appContext.mSoundSample.fractionSoundSize){
            if(appContext.mBufferingContext.strongTimeOffset<appContext.mSoundSample.measureSoundSize){
                measureSoundCell = appContext.mSoundSample.measureRawSound[appContext.mBufferingContext.strongTimeOffset];
                measureSoundCell += appContext.mSoundSample.measureRawSound[appContext.mBufferingContext.strongTimeOffset+1]<<8;
                fractionSoundCell = appContext.mSoundSample.fractionRawSound[appContext.mBufferingContext.weakTimeOffset] ;
                fractionSoundCell+= appContext.mSoundSample.fractionRawSound[appContext.mBufferingContext.weakTimeOffset+1]<<8 ;
                resultCell = (measureSoundCell + fractionSoundCell)/2 ;

                /*appContext.mBufferingContext.buffer[bufferToFill][i] = (char) ((appContext.mSoundSample.measureRawSound[appContext.mBufferingContext.strongTimeOffset]
                        + appContext.mSoundSample.fractionRawSound[appContext.mBufferingContext.weakTimeOffset])/2) ;*/
                appContext.mBufferingContext.buffer[bufferToFill][i] = (char) (resultCell & 0xFF) ;
                appContext.mBufferingContext.buffer[bufferToFill][i+1] = (char) (resultCell>>8) ;
            }else{
                appContext.mBufferingContext.buffer[bufferToFill][i] = appContext.mSoundSample.fractionRawSound[appContext.mBufferingContext.weakTimeOffset] ;
                appContext.mBufferingContext.buffer[bufferToFill][i+1] = appContext.mSoundSample.fractionRawSound[appContext.mBufferingContext.weakTimeOffset+1] ;
            }
        }
        else{
            if(appContext.mBufferingContext.strongTimeOffset<appContext.mSoundSample.measureSoundSize){
                appContext.mBufferingContext.buffer[bufferToFill][i] = appContext.mSoundSample.measureRawSound[appContext.mBufferingContext.strongTimeOffset] ;
                appContext.mBufferingContext.buffer[bufferToFill][i+1] = appContext.mSoundSample.measureRawSound[appContext.mBufferingContext.strongTimeOffset+1] ;
            }else{
                appContext.mBufferingContext.buffer[bufferToFill][i] = 0 ;
                appContext.mBufferingContext.buffer[bufferToFill][i+1] = 0 ;
            }
        }
        appContext.mBufferingContext.weakTimeOffset %= tickTimeInOctet ;
        appContext.mBufferingContext.strongTimeOffset %= measureTimeInOctet ;
    }
}

// Signal handlers :
void metronomeTimerCallback(int sig, siginfo_t* si, void* uc){
    pthread_mutex_lock(&appContext.mTimerContext.metronomeMutex) ;
    if(pthread_cond_signal(&appContext.mTimerContext.metronomeCondition)!=0){
        LOGE("Error while notifying metronome condition") ;
    }
    pthread_mutex_unlock(&appContext.mTimerContext.metronomeMutex) ;
}

void clockTimerCallback(int sig, siginfo_t* si, void* uc){
    pthread_mutex_lock(&appContext.mTimerContext.clockMutex) ;
    if(pthread_cond_signal(&appContext.mTimerContext.clockCondition)!=0){
        LOGE("Error while signaling clock condition") ;
    }
    pthread_mutex_unlock(&appContext.mTimerContext.clockMutex) ;
}

void setTimers(void) {

    double intermediate_value;
    intermediate_value = appContext.mTimeParameter.tickTimeInFrame*appContext.mTimeParameter.frameTimeInS ;
    intermediate_value /= 1;
    int tickTimeInS_integerized = (int) intermediate_value;
    double rest_in_sec = appContext.mTimeParameter.tickTimeInS - tickTimeInS_integerized;
    rest_in_sec *= 1000000000;
    int rest_in_sec_integerized = (int) rest_in_sec;
    LOGI("number of sec : %d , number of nano sec : %d", tickTimeInS_integerized,
         rest_in_sec_integerized);

    appContext.mTimerContext.metronomeTimerSpec.it_interval.tv_sec = tickTimeInS_integerized;
    appContext.mTimerContext.metronomeTimerSpec.it_interval.tv_nsec = rest_in_sec_integerized;
    appContext.mTimerContext.metronomeTimerSpec.it_value.tv_sec = 0;
    appContext.mTimerContext.metronomeTimerSpec.it_value.tv_nsec = 1;

    appContext.mTimerContext.clockTimerSpec.it_value.tv_sec = 0;
    appContext.mTimerContext.clockTimerSpec.it_value.tv_nsec = 1;
    appContext.mTimerContext.clockTimerSpec.it_interval.tv_sec = 1;
    appContext.mTimerContext.clockTimerSpec.it_interval.tv_nsec = 0;

    LOGI("Timers are sets") ;
}

int createTimers(void){

    int returned_value = 0 ;

    // register an action for MSIG_METRONOME signal
    appContext.mTimerContext.metronomeSigAction.sa_flags = SA_SIGINFO ;
    appContext.mTimerContext.metronomeSigAction.sa_sigaction = metronomeTimerCallback ;
    sigemptyset(&appContext.mTimerContext.metronomeSigAction.sa_mask) ;
    if(sigaction(MSIG_METRONOME,&appContext.mTimerContext.metronomeSigAction,NULL)){
        LOGE("Error while assigning signal to metronomeSigaction") ;
        returned_value = 1 ;
    }

    // Event throw by the metronome's timer :
    appContext.mTimerContext.metronomeSigEvent.sigev_notify = SIGEV_SIGNAL ;
    appContext.mTimerContext.metronomeSigEvent.sigev_signo = MSIG_METRONOME ;
    appContext.mTimerContext.metronomeSigEvent.sigev_value.sival_ptr = &appContext.mTimerContext.metronomeTimer ;

    if(timer_create(MCLOCKID,&appContext.mTimerContext.metronomeSigEvent,&appContext.mTimerContext.metronomeTimer)!=0){
        LOGE("Error while creating the metronome timer") ;
        returned_value = 2 ;
    }


    // register an action for MSIG_CLOCK signal :
    appContext.mTimerContext.clockSigAction.sa_flags = SA_SIGINFO ;
    appContext.mTimerContext.clockSigAction.sa_sigaction = clockTimerCallback ;
    sigemptyset(&appContext.mTimerContext.clockSigAction.sa_mask) ;
    if(sigaction(MSIG_CLOCK,&appContext.mTimerContext.clockSigAction,NULL)){
        LOGE("Error while assigning the signal to cock sig action") ;
        returned_value = 3 ;
    }

    // Event thrown by the clock's timer :
    appContext.mTimerContext.clockSigEvent.sigev_notify = SIGEV_SIGNAL ;
    appContext.mTimerContext.clockSigEvent.sigev_signo = MSIG_CLOCK ;
    appContext.mTimerContext.clockSigEvent.sigev_value.sival_ptr = &appContext.mTimerContext.clockTimer ;

    if(timer_create(MCLOCKID,&appContext.mTimerContext.clockSigEvent,&appContext.mTimerContext.clockTimer)!=0){
        LOGE("Error while creating clock timer") ;
        returned_value = 4 ;
    }


    LOGI("Timers are created ") ;

    return returned_value ;
}

int destroyTimers(){
    int returned_value = 0 ;

    if(timer_delete(appContext.mTimerContext.metronomeTimer)!=0){
        LOGE("Error while deleting the metronome timer") ;
        returned_value = 1 ;
    }
    if(timer_delete(appContext.mTimerContext.clockTimer)!=0){
        LOGE("Error while deleting the clock timer") ;
        returned_value = 2 ;
    }

    return returned_value ;
}

void startTimers(){
    if(timer_settime(appContext.mTimerContext.metronomeTimer,0,&appContext.mTimerContext.metronomeTimerSpec,NULL) == -1)
    {
        LOGE("Error while starting the metronome timer") ;
    }
    appContext.mTimerContext.metronomeTimerDisarmed = 0 ;

    if(timer_settime(appContext.mTimerContext.clockTimer,0,&appContext.mTimerContext.clockTimerSpec,NULL) == -1){
        LOGE("Error while starting the clock timer") ;
    }
    appContext.mTimerContext.clockTimerDisarmed = 0 ;
}

void stopTimers(){
    appContext.mTimerContext.metronomeTimerSpec.it_value.tv_sec = 0 ;
    appContext.mTimerContext.metronomeTimerSpec.it_value.tv_nsec = 0 ;
    appContext.mTimerContext.clockTimerSpec.it_value.tv_sec = 0 ;
    appContext.mTimerContext.clockTimerSpec.it_value.tv_nsec = 0 ;

    if(timer_settime(appContext.mTimerContext.metronomeTimer,0,&appContext.mTimerContext.metronomeTimerSpec,NULL) != 0){
        LOGE("Error while disarming metronome timer") ;
    }
    appContext.mTimerContext.metronomeTimerDisarmed = 1 ;

    if(timer_settime(appContext.mTimerContext.clockTimer,0,&appContext.mTimerContext.clockTimerSpec,NULL) != 0){
        LOGE("Error while disarming clock timer") ;
    }
    appContext.mTimerContext.clockTimerDisarmed = 1 ;
}

void metronomeThreadRoutine(void*) ;
void clockThreadRoutine(void*) ;

int createWorkflowCoherencyInfrastructure(){

    int returned_value = 0 ;

    if(pthread_mutex_init(&appContext.mTimerContext.clockMutex,NULL)!=0){
        LOGE("Something went wrong in the clock mutex init") ;
        returned_value = 1 ;
    }

    if(pthread_mutex_init(&appContext.mTimerContext.metronomeMutex,NULL)!=0){
        LOGE("Something went wrong in the metronome mutex init") ;
        returned_value = 2 ;
    }

    if(pthread_attr_init(&appContext.mTimerContext.clockAttrThread)!=0){
        LOGE("Something went wrong in the clock thread attribute init") ;
        returned_value = 3 ;
    }

    if(pthread_attr_init(&appContext.mTimerContext.metronomeAttrThread)!=0){
        LOGE("Something went wrong in the metronome thread attribute init") ;
        returned_value = 4 ;
    }

    if(pthread_attr_setdetachstate(&appContext.mTimerContext.clockAttrThread,PTHREAD_CREATE_JOINABLE)!=0){
        LOGE("Something went wrong while setting the detached state of the clock thread") ;
        returned_value = 5 ;
    }

    if(pthread_attr_setdetachstate(&appContext.mTimerContext.metronomeAttrThread,PTHREAD_CREATE_JOINABLE)!=0){
        LOGE("Something went wrong while setting the detached state of the metronome thread") ;
        returned_value = 6 ;
    }

    if(pthread_cond_init(&appContext.mTimerContext.clockCondition,NULL)!=0){
        LOGE("Something went wrong in clock condition init") ;
        returned_value = 7 ;
    }

    if(pthread_cond_init(&appContext.mTimerContext.metronomeCondition,NULL)!=0){
        LOGE("Something went wrong in metronom condition init") ;
        returned_value = 8 ;
    }

    return returned_value ;
}

int destroyWorkflowInfrastructure(){
    int returned_value = 0 ;
    if(pthread_mutex_destroy(&appContext.mTimerContext.metronomeMutex)!=0){
        LOGE("Error while destroying the metronome mutex") ;
        returned_value = 1 ;
    }

    if(pthread_mutex_destroy(&appContext.mTimerContext.clockMutex)!=0){
        LOGE("Error while destroying the clock mutex") ;
        returned_value = 2 ;
    }

    if(pthread_attr_destroy(&appContext.mTimerContext.metronomeAttrThread)!=0){
        LOGE("Error while destroying the metronome attr thread") ;
        returned_value = 3 ;
    }

    if(pthread_attr_destroy(&appContext.mTimerContext.clockAttrThread)!=0){
        LOGE("Error while destroying the clock attr thread") ;
        returned_value = 4 ;
    }

    if(pthread_cond_destroy(&appContext.mTimerContext.metronomeCondition)!=0){
        LOGE("Error while destroying the metronome condition") ;
        returned_value = 5 ;
    }

    if(pthread_cond_destroy(&appContext.mTimerContext.clockCondition)!=0){
        LOGE("Error while destroying the clock condition") ;
        returned_value = 6 ;
    }

    return returned_value ;
}
// Thread routine :

void metronomeThreadRoutine(void* arg){

    // Get an env from attaching the current thread to the vm hosting the app :
    JNIEnv* env ;
    (*appContext.mJavaContext.vm)->AttachCurrentThread(appContext.mJavaContext.vm,&env,NULL) ;
    jmethodID update_measure_id = (*env)->GetMethodID(env,appContext.mJavaContext.mainActivityClass,"updateMeasure","()V") ;

    while(appContext.mOpenSlContext.isPlaying == 1){
        pthread_mutex_lock(&appContext.mTimerContext.metronomeMutex) ;
        pthread_cond_wait(&appContext.mTimerContext.metronomeCondition,&appContext.mTimerContext.metronomeMutex) ;
        if(appContext.mTimerContext.metronomeTimerDisarmed==0)
            (*env)->CallVoidMethod(env,appContext.mJavaContext.mainActivityInstance,update_measure_id) ;
        LOGI("Metronome send me") ;
        pthread_mutex_unlock(&appContext.mTimerContext.metronomeMutex) ;
    }

    // detach current thread from vm :
    (*appContext.mJavaContext.vm)->DetachCurrentThread(appContext.mJavaContext.vm) ;

    // Cleaning metronome workflow infrastructure :
    /*if(pthread_mutex_destroy(&appContext.mTimerContext.metronomeMutex)!=0){
        LOGE("Error while destroying metronome mutex") ;
    }

    if(pthread_attr_destroy(&appContext.mTimerContext.metronomeAttrThread) != 0){
        LOGE("Error while destroying metronome thread attr") ;
    }

    if(pthread_cond_destroy(&appContext.mTimerContext.metronomeCondition) !=0){
        LOGE("Error while destroying metronome condition") ;
    }*/

}

void clockThreadRoutine(void* arg){

    // Associate get an env from attaching the current thread to the vm :
    JNIEnv* env ;
    (*appContext.mJavaContext.vm)->AttachCurrentThread(appContext.mJavaContext.vm,&env,NULL) ;

    // mainActivity instance (jobject) and jclass has been declared as global ref while MainActivity$onCreate
    // called JNI function createMemoryStructure
    jmethodID update_chono_id = (*env)->GetMethodID(env,appContext.mJavaContext.mainActivityClass,"updateClockTime","()V") ;

    while(appContext.mOpenSlContext.isPlaying == 1){
        pthread_mutex_lock(&appContext.mTimerContext.clockMutex) ;
        pthread_cond_wait(&appContext.mTimerContext.clockCondition,&appContext.mTimerContext.clockMutex) ;
        if(appContext.mTimerContext.clockTimerDisarmed==0)
            (*env)->CallVoidMethod(env,appContext.mJavaContext.mainActivityInstance,update_chono_id) ;
        LOGI("Clock send me ") ;
        pthread_mutex_unlock(&appContext.mTimerContext.clockMutex) ;
    }

    // Detach current thread from vm :
    (*appContext.mJavaContext.vm)->DetachCurrentThread(appContext.mJavaContext.vm) ;

    // Cleaning clock workflow infrastructure :
    /*if(pthread_mutex_destroy(&appContext.mTimerContext.clockMutex) !=0 ){
        LOGE("Error while destroying the clock mutex") ;
    }

    if(pthread_attr_destroy(&appContext.mTimerContext.clockAttrThread) != 0 ){
        LOGE("Error while destroying the clock attr thread") ;
    }

    if(pthread_cond_destroy(&appContext.mTimerContext.clockCondition) !=0){
        LOGE("Error while destroying the clokc condition") ;
    }*/
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm , void* reserved){
    JNIEnv *env ;

    // memory initialization
    memset(&appContext,0,sizeof(NativeContext)) ;

    // create the anti corruption lock to avoid play/stop furious "wreck it" call's
    if(pthread_mutex_init(&appContext.antiCorruptLock,NULL)!=0){
        LOGE("Error while settinng the antiCorruptLock mutex") ;
    }

    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK){
        LOGI("Erreur NJI version != 1.6") ;
        return -1 ;
    }

    appContext.mJavaContext.vm = vm ;

    return JNI_VERSION_1_6 ;

}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm , void* reserved){
    // when the class loader containing the JNI lib is garbage collected
    pthread_mutex_destroy(&appContext.antiCorruptLock) ;
}

JNIEXPORT void JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_setTempo(JNIEnv* env, jobject instance,jint tempo_value){
    appContext.mTimeParameter.tempoValue = tempo_value ;
    LOGI("tempo value : %d",tempo_value) ;
}

// Method to create the engine
JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_createEngine(JNIEnv *env, jobject instance){

    SLresult result ;
    result = slCreateEngine(&appContext.mOpenSlContext.engineObject,0,NULL,0,NULL,NULL) ;
    assert(result == SL_RESULT_SUCCESS) ;


    result = (*appContext.mOpenSlContext.engineObject)->Realize(appContext.mOpenSlContext.engineObject,SL_BOOLEAN_FALSE) ;
    assert(result == SL_RESULT_SUCCESS) ;

    result = (*appContext.mOpenSlContext.engineObject)->GetInterface(appContext.mOpenSlContext.engineObject,SL_IID_ENGINE,&appContext.mOpenSlContext.engineInterface) ;
    assert(result == SL_RESULT_SUCCESS) ;

    // Set the speakers :
    result = (*appContext.mOpenSlContext.engineInterface)->CreateOutputMix(appContext.mOpenSlContext.engineInterface,
                                                                           &appContext.mOpenSlContext.outputMixObject,
                                                                           0,NULL,NULL) ;
    assert(result == SL_RESULT_SUCCESS) ;

    result = (*appContext.mOpenSlContext.outputMixObject)->Realize(appContext.mOpenSlContext.outputMixObject,SL_BOOLEAN_FALSE) ;
    assert(result == SL_RESULT_SUCCESS ) ;

    LOGI("all went well, engine created") ;

    return JNI_TRUE ;
}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_releaseEngine(JNIEnv* env, jobject instance){

    if(appContext.mOpenSlContext.outputMixObject != NULL){
        (*appContext.mOpenSlContext.outputMixObject)->Destroy(appContext.mOpenSlContext.outputMixObject) ;
        appContext.mOpenSlContext.outputMixObject = NULL ;
    }

    if(appContext.mOpenSlContext.playerObject!=NULL){
        (*appContext.mOpenSlContext.playerObject)->Destroy(appContext.mOpenSlContext.playerObject) ;
        appContext.mOpenSlContext.playerObject = NULL ;
        appContext.mOpenSlContext.playInterface = NULL ;
        appContext.mOpenSlContext.queueInterface = NULL ;
    }

    if(appContext.mOpenSlContext.engineObject!=NULL){
        (*appContext.mOpenSlContext.engineObject)->Destroy(appContext.mOpenSlContext.engineObject) ;
        appContext.mOpenSlContext.engineObject = NULL ;
        appContext.mOpenSlContext.engineInterface = NULL ;
    }

    return JNI_TRUE ;
}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_setFractionFile(JNIEnv* env, jobject instance,jbyteArray byte_file){
    SLresult result ;
    jsize file_length = ((*env)->GetArrayLength(env,byte_file)) -44;
    //file_length -= 100 ;
    appContext.mSoundSample.fractionSoundSize = file_length ;
    appContext.mSoundSample.fractionRawSound = (char*) malloc(sizeof(char)*file_length) ;
    memset(appContext.mSoundSample.fractionRawSound,0,sizeof(char)*file_length);

    int i ;
    jbyte *cell = (*env)->GetByteArrayElements(env,byte_file,NULL) ;
    for(i=0; i<file_length ; i++){
        appContext.mSoundSample.fractionRawSound[i] = cell[i+44] ;
    }
    (*env)->ReleaseByteArrayElements(env,byte_file,cell,JNI_ABORT) ;

    // to do : separate the set pcmFormat action , acquire the default sample rate the device handle,
    // determine the right file to load consequently


    appContext.mSoundSample.sampleRate = 44100 ;
    appContext.mSoundSample.pcmFormat.formatType = SL_DATAFORMAT_PCM ;
    appContext.mSoundSample.pcmFormat.numChannels = 2 ;
    appContext.mSoundSample.pcmFormat.samplesPerSec = 44100000 ;
    appContext.mSoundSample.pcmFormat.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16 ;
    appContext.mSoundSample.pcmFormat.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT ;
    appContext.mSoundSample.pcmFormat.bitsPerSample = 16 ;
    appContext.mSoundSample.pcmFormat.endianness = SL_BYTEORDER_LITTLEENDIAN ;

    LOGI("File set") ;

    return JNI_TRUE ;
}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_setMeasureFile(JNIEnv* env,jobject instance,jbyteArray byte_file){
    SLresult result ;
    jsize file_length = ((*env)->GetArrayLength(env,byte_file)) -44;
    //file_length -= 100 ;
    appContext.mSoundSample.measureSoundSize = file_length ;
    appContext.mSoundSample.measureRawSound = (char*) malloc(sizeof(char)*file_length) ;
    memset(appContext.mSoundSample.measureRawSound,0,sizeof(char)*file_length);

    int i ;
    jbyte *cell = (*env)->GetByteArrayElements(env,byte_file,NULL) ;
    for(i=0; i<file_length ; i++){
        appContext.mSoundSample.measureRawSound[i] = cell[i+44] ;
    }
    (*env)->ReleaseByteArrayElements(env,byte_file,cell,JNI_ABORT) ;

    appContext.mSoundSample.sampleRate = 44100 ;
    appContext.mSoundSample.pcmFormat.formatType = SL_DATAFORMAT_PCM ;
    appContext.mSoundSample.pcmFormat.numChannels = 2 ;
    appContext.mSoundSample.pcmFormat.samplesPerSec = 44100000 ;
    appContext.mSoundSample.pcmFormat.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16 ;
    appContext.mSoundSample.pcmFormat.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT ;
    appContext.mSoundSample.pcmFormat.bitsPerSample = 16 ;
    appContext.mSoundSample.pcmFormat.endianness = SL_BYTEORDER_LITTLEENDIAN ;

    LOGI("File set") ;

    return JNI_TRUE ;
}

void SLAPIENTRY endOfBufferCallback(SLAndroidSimpleBufferQueueItf android_queue_interface,void* pContext){

    SLresult result ;
    LOGI("in bufqueue callback") ;
    int select = appContext.mRingBuffer.read_offset % RINGBUFFER_FRAME_NUMBER ;

    result = (*appContext.mOpenSlContext.queueInterface)->Enqueue(appContext.mOpenSlContext.queueInterface,
                                                                  appContext.mRingBuffer.buffer[select],
                                                                  appContext.mDeviceSpec.defaultBufferFrame*2) ;
    assert(result == SL_RESULT_SUCCESS) ;
    appContext.mRingBuffer.read_offset++ ;
}

void SLAPIENTRY anotherCallback(SLAndroidSimpleBufferQueueItf interface,void* pContext){
    SLresult result ;

    /*if(appContext.mOpenSlContext.isPlaying == 1){
        appContext.mTimeParameter.beatCounter++ ;
        // enqueue the other buffer , and compute the next to enqueue
        result = (*appContext.mOpenSlContext.queueInterface)->Enqueue(appContext.mOpenSlContext.queueInterface,
                                                                      appContext.mBufferingContext.buffer[appContext.mBufferingContext.bufferSelector],
                                                                      appContext.mBufferingContext.bufferSizeInOctet) ;
        assert(result == SL_RESULT_SUCCESS) ;
        appContext.mBufferingContext.bufferSelector += 1 ;
        appContext.mBufferingContext.bufferSelector %= 2 ;

        fillBufferToEnqueue(appContext.mBufferingContext.bufferSelector) ;

    }*/
    if(appContext.mOpenSlContext.isPlaying==1){
        fillBufferToEnqueue(0) ;
        result = (*appContext.mOpenSlContext.queueInterface)->Enqueue(appContext.mOpenSlContext.queueInterface,
                                                                      appContext.mBufferingContext.buffer[0],
                                                                      appContext.mBufferingContext.bufferSizeInOctet) ;
        assert(result == SL_RESULT_SUCCESS) ;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_createPlayer(JNIEnv* env, jobject instance){
    jclass main_activity_class = (*env)->GetObjectClass(env,instance) ;
    jfieldID numerateur_id = (*env)->GetFieldID(env,main_activity_class,"numerateur_value","I") ;
    jfieldID  denominateur_id = (*env)->GetFieldID(env,main_activity_class,"denominateur_value","I") ;
    jfieldID  default_frame_buffer_size_id = (*env)->GetFieldID(env,main_activity_class,"default_frame_buffer_size","I") ;
    jfieldID  default_sample_rate_id = (*env)->GetFieldID(env,main_activity_class,"default_sample_rate","I") ;

    jint numerateur_value = (*env)->GetIntField(env,instance,numerateur_id) ;
    jint denominateur_value = (*env)->GetIntField(env,instance,denominateur_id) ;
    jint default_frame_buffer_size = (*env)->GetIntField(env,instance,default_frame_buffer_size_id) ;
    jint default_sample_rate = (*env)->GetIntField(env,instance,default_sample_rate_id) ;

    SLresult result ;

    appContext.mDeviceSpec.defaultBufferFrame = default_frame_buffer_size ;
    appContext.mDeviceSpec.sampleRate = default_sample_rate ;

    if(appContext.mSoundSample.fractionRawSound != 0) {
        SLDataLocator_AndroidSimpleBufferQueue android_buffer_queue = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,2 } ;
        SLDataSource data_source = { &android_buffer_queue , &appContext.mSoundSample.pcmFormat };
        SLDataLocator_OutputMix locator_output_mix = { SL_DATALOCATOR_OUTPUTMIX, appContext.mOpenSlContext.outputMixObject };
        SLDataSink audio_sink = { &locator_output_mix, NULL };

        const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE } ;
        const SLboolean  reqs[] = { SL_BOOLEAN_TRUE } ;


        result = (*appContext.mOpenSlContext.engineInterface)->CreateAudioPlayer(appContext.mOpenSlContext.engineInterface,
                                                                                 &appContext.mOpenSlContext.playerObject,
                                                                                 &data_source,
                                                                                 &audio_sink,
                                                                                 1,ids,reqs) ;
        assert(result == SL_RESULT_SUCCESS) ;

        result = (*appContext.mOpenSlContext.playerObject)->Realize(appContext.mOpenSlContext.playerObject,SL_BOOLEAN_FALSE) ;
        assert(result == SL_RESULT_SUCCESS) ;

        result = (*appContext.mOpenSlContext.playerObject)->GetInterface(appContext.mOpenSlContext.playerObject,SL_IID_PLAY,&appContext.mOpenSlContext.playInterface) ;
        assert(result == SL_RESULT_SUCCESS) ;

        result = (*appContext.mOpenSlContext.playerObject)->GetInterface(appContext.mOpenSlContext.playerObject,SL_IID_ANDROIDSIMPLEBUFFERQUEUE,&appContext.mOpenSlContext.queueInterface) ;
        assert(result == SL_RESULT_SUCCESS) ;

        result = (*appContext.mOpenSlContext.queueInterface)->RegisterCallback(appContext.mOpenSlContext.queueInterface,endOfBufferCallback,NULL) ;
        assert(result == SL_RESULT_SUCCESS) ;

        result = (*appContext.mOpenSlContext.queueInterface)->RegisterCallback(appContext.mOpenSlContext.queueInterface,anotherCallback,NULL) ;
        assert(result == SL_RESULT_SUCCESS) ;

        /*result = (*appContext.mOpenSlContext.playInterface)->SetPlayState(appContext.mOpenSlContext.playInterface,SL_PLAYSTATE_PLAYING) ;
        assert(result == SL_RESULT_SUCCESS) ;*/

        LOGI("player created, playing state is on.") ;

        return JNI_TRUE ;
    }else{
        LOGI("file is not set yet") ;

    }

    return JNI_FALSE ;

}

JNIEXPORT void JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_setDeviceSpec(JNIEnv* env, jobject instance, jint sampling_rate, jint default_frame){
    appContext.mDeviceSpec.sampleRate = sampling_rate ;
    appContext.mDeviceSpec.defaultBufferFrame = default_frame ;
}

JNIEXPORT void JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_createRingBuffer(JNIEnv* env, jobject instance){

    int i = 0 ;
    appContext.mRingBuffer.buffer = (char**) malloc(sizeof(char*)*RINGBUFFER_FRAME_NUMBER) ;
    for( i=0 ; i<RINGBUFFER_FRAME_NUMBER ; i++){
        appContext.mRingBuffer.buffer[i] = (char*) malloc(sizeof(char)*appContext.mDeviceSpec.defaultBufferFrame*2) ;
        memset(appContext.mRingBuffer.buffer[i],0,appContext.mDeviceSpec.defaultBufferFrame*2*sizeof(char)) ;
    }

    appContext.mRingBuffer.size = RINGBUFFER_FRAME_NUMBER*appContext.mDeviceSpec.defaultBufferFrame*2 ;
    appContext.mRingBuffer.step = 0 ;
    appContext.mRingBuffer.read_offset = 0 ;
    appContext.mRingBuffer.write_offset = 0 ;

}

void refill_ringbuffer_routine(void* arg) {

    int i  ;
    SLuint32 play_state ;
    double tick_time,frame_time;
    int number_of_frames,tick_size_in_octets,buffer_size_in_octet,current_tick_frame ;
    if(appContext.mTimeParameter.tempoValue != 0){
        tick_time = 60.0 / appContext.mTimeParameter.tempoValue ;
    }else{
        tick_time = 0 ;
        LOGE("tick_time : division par zero") ;
    }

    if(appContext.mDeviceSpec.sampleRate != 0){
        frame_time = 1.0 / appContext.mDeviceSpec.sampleRate ;
    } else{
        frame_time = 0 ;
        LOGE("frame time : division par zero") ;
    }

    if( frame_time != 0 ){
        double intermediate = tick_time/frame_time ;
        number_of_frames = (int) intermediate ;
    }else{
        number_of_frames = 0 ;
        LOGE("number_of_frames : division par zéro") ;
    }

    if(number_of_frames != 0 ){
        // 2 frames for 2 decoder (right , left) , each of it 16 bits
        tick_size_in_octets = number_of_frames*4 ;
    }else{
        tick_size_in_octets = 0 ;
        LOGE("number_of_frames : erreur => 0") ;
    }

    if(appContext.mDeviceSpec.defaultBufferFrame != 0){
        buffer_size_in_octet = appContext.mDeviceSpec.defaultBufferFrame*2 ;
    } else{
        buffer_size_in_octet = 0 ;
        LOGE("default buffer frame was 0") ;
    }

    LOGI("in refill buferr") ;

    current_tick_frame = 0 ;
    for( i = 0 ; i<buffer_size_in_octet ; i++){

        if(current_tick_frame<=appContext.mSoundSample.fractionSoundSize){
            appContext.mRingBuffer.buffer[appContext.mRingBuffer.write_offset][i] =
            appContext.mSoundSample.fractionRawSound[current_tick_frame++] ;
        }else{
            appContext.mRingBuffer.buffer[appContext.mRingBuffer.write_offset][i] = 0 ;
            current_tick_frame++ ;
        }
        current_tick_frame %= tick_size_in_octets ;
    }
    appContext.mRingBuffer.write_offset++ ;
    appContext.mRingBuffer.write_offset%=RINGBUFFER_FRAME_NUMBER ;

    LOGI(" tick_time : %lf, frame_time : %lf \nTick number of frames : %d, Tick number of frames in octets : %d \nBuffer size in octet : %d",
         tick_time , frame_time , number_of_frames , tick_size_in_octets , buffer_size_in_octet) ;

    //appContext.mRingBuffer.write_offset++ ;

    int play_tag = 0 ;

    while(appContext.mOpenSlContext.isPlaying == 1 ){
        while(appContext.mRingBuffer.write_offset != appContext.mRingBuffer.read_offset){
            for( i = 0 ; i<buffer_size_in_octet ; i++){

                if(current_tick_frame<=appContext.mSoundSample.fractionSoundSize){
                    appContext.mRingBuffer.buffer[appContext.mRingBuffer.write_offset][i] =
                            appContext.mSoundSample.fractionRawSound[current_tick_frame++] ;
                }else{
                    appContext.mRingBuffer.buffer[appContext.mRingBuffer.write_offset][i] = 0 ;
                    current_tick_frame++ ;
                }
                current_tick_frame %= tick_size_in_octets ;
            }
            appContext.mRingBuffer.write_offset++ ;
            appContext.mRingBuffer.write_offset%=RINGBUFFER_FRAME_NUMBER ;
        }
        //LOGI("buffer is full") ;
        // enqueue the first :
        if(play_tag == 0){
            play_tag = 1 ;
            (*appContext.mOpenSlContext.queueInterface)->Enqueue(appContext.mOpenSlContext.queueInterface,appContext.mRingBuffer.buffer[0],buffer_size_in_octet) ;
            (*appContext.mOpenSlContext.playInterface)->SetPlayState(appContext.mOpenSlContext.playInterface,SL_PLAYSTATE_PLAYING) ;
        }

    }



    LOGI("exiting fill buffer") ;
    SLresult result = (*appContext.mOpenSlContext.playInterface)->SetPlayState(appContext.mOpenSlContext.playInterface,SL_PLAYSTATE_STOPPED) ;
    assert( result == SL_RESULT_SUCCESS) ;
}

JNIEXPORT void JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_createMemoryStructure(JNIEnv* env,jobject instance){

    // Set up the metronome and clock's mutexes, conditions,thread's attributes
    createWorkflowCoherencyInfrastructure() ;

    // setup the buffering memory :
    appContext.mBufferingContext.buffer[0] = (char*) malloc(appContext.mDeviceSpec.defaultBufferFrame*2*sizeof(char)) ;
    memset(appContext.mBufferingContext.buffer[0],0,appContext.mDeviceSpec.defaultBufferFrame*2*sizeof(char)) ;
    appContext.mBufferingContext.buffer[1] = (char*) malloc(appContext.mDeviceSpec.defaultBufferFrame*2*sizeof(char)) ;
    memset(appContext.mBufferingContext.buffer[1],0,appContext.mDeviceSpec.defaultBufferFrame*2*sizeof(char)) ;

    // create the timers
    createTimers() ;

    // create globals ref to future refering
    jclass mainActivity_class = (*env)->GetObjectClass(env,instance);
    appContext.mJavaContext.mainActivityClass = (*env)->NewGlobalRef(env, mainActivity_class);
    appContext.mJavaContext.mainActivityInstance = (*env)->NewGlobalRef(env, instance);

}

JNIEXPORT void JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_releaseMemoryStructure(JNIEnv* env, jobject instance){

    // release the metronome and clock's mutexes, conditions and thread's attributes
    destroyWorkflowInfrastructure() ;

    // delete the buffering memory structure :
    free(appContext.mBufferingContext.buffer[0]) ;
    free(appContext.mBufferingContext.buffer[1]) ;

    // destroy the timers :
    destroyTimers() ;

    // destroy the globals ref :
    (*env)->DeleteGlobalRef(env,appContext.mJavaContext.mainActivityClass) ;
    (*env)->DeleteGlobalRef(env,appContext.mJavaContext.mainActivityInstance) ;

}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_play(JNIEnv* env, jobject instance){

    // Get the lock :
    pthread_mutex_lock(&appContext.antiCorruptLock) ;

    computeTimingContext(env,instance) ;
    // set the real tempo
    jmethodID setRealTempo_method_id = (*env)->GetMethodID(env,appContext.mJavaContext.mainActivityClass,"displayRealTempo","(D)V") ;
    LOGI("rela tempo value there : %lf",appContext.mTimeParameter.realTempoValue) ;
    (*env)->CallVoidMethod(env,appContext.mJavaContext.mainActivityInstance,setRealTempo_method_id,appContext.mTimeParameter.realTempoValue) ;
    // set up the buffering structure
    // 16 bits per frame

    appContext.mBufferingContext.weakTimeOffset = 0 ;
    appContext.mBufferingContext.strongTimeOffset = 0 ;
    appContext.mBufferingContext.bufferSizeInOctet = 2*appContext.mDeviceSpec.defaultBufferFrame ;
    /*appContext.mBufferingContext.buffer[0] = (char*) malloc(appContext.mBufferingContext.bufferSizeInOctet*sizeof(char)) ;
    appContext.mBufferingContext.buffer[1] = (char*) malloc(appContext.mBufferingContext.bufferSizeInOctet*sizeof(char)) ;*/
    appContext.mBufferingContext.bufferSelector = 1 ;

    // fill them up
    fillBufferToEnqueue(0) ;
    //fillBufferToEnqueue(1) ;

    // Set the timers up :
    // 1 thread for the metronome to callback UI update of current tick
    // 1 thread for the clock to callback UI update of the current session's time
    // both thread will sit on a condition which is signaled by timer

    /*if(createTimers()!=0){
        LOGE("play : Exiting...") ;
        return JNI_FALSE ;
    }*/
    setTimers() ;

    /*if(createWorkflowCoherencyInfrastructure()!=0){
        LOGE("workflow coherency returned with errors") ;
        return JNI_FALSE ;
    }*/

    appContext.mOpenSlContext.isPlaying = 1 ;

    if(pthread_create(&appContext.mTimerContext.clockThread,&appContext.mTimerContext.clockAttrThread,
                      clockThreadRoutine,NULL)!=0){
        LOGE("Something went wrong while creating the clock thread") ;
        return JNI_FALSE ;
    }

    if(pthread_create(&appContext.mTimerContext.metronomeThread,&appContext.mTimerContext.metronomeAttrThread,
                      metronomeThreadRoutine,NULL)!=0){
        LOGE("Something went wrong while creating the metronome thread");
        return JNI_FALSE ;
    }

    // each timer thread will take care of cleaning itself up through exiting

    LOGI("Timer structure set") ;

    // launch all of the timing engine, timers + sounds :

    SLresult result ;
    /*result = (*appContext.mOpenSlContext.queueInterface)->Clear(appContext.mOpenSlContext.queueInterface) ;
    assert(result == SL_RESULT_SUCCESS) ;*/




    startTimers() ;

    result = (*appContext.mOpenSlContext.queueInterface)->Enqueue(appContext.mOpenSlContext.queueInterface,
                                                                  appContext.mBufferingContext.buffer[appContext.mBufferingContext.bufferSelector],
                                                                  appContext.mBufferingContext.bufferSizeInOctet) ;
    assert(result == SL_RESULT_SUCCESS) ;
    LOGI("check") ;

    result = (*appContext.mOpenSlContext.playInterface)->SetPlayState(appContext.mOpenSlContext.playInterface,SL_PLAYSTATE_PLAYING) ;
    assert(result == SL_RESULT_SUCCESS) ;

    // release the lock :
    pthread_mutex_unlock(&appContext.antiCorruptLock) ;
    return JNI_TRUE ;
}

JNIEXPORT jboolean JNICALL
Java_com_example_nextu_cmakeappmetro_MainActivity_stop(JNIEnv* env, jobject instance){

    // Get The lock :
    pthread_mutex_lock(&appContext.antiCorruptLock) ;

    appContext.mOpenSlContext.isPlaying = 0 ;

    /*if(pthread_attr_destroy(&appContext.mThreadContext.refill_thread_attr)!=0){
        LOGE("erreur lor s la destruction des attributs du thread") ;
        return JNI_FALSE ;
    }*/

    stopTimers() ;
    /*if(destroyTimers()!=0){
        LOGE("Error while destroying the timers") ;
        return JNI_FALSE ;
    }*/

    // force metronome and clock thread to exiting :

    pthread_cond_signal(&appContext.mTimerContext.metronomeCondition) ;
    pthread_cond_signal(&appContext.mTimerContext.clockCondition) ;
    pthread_join(appContext.mTimerContext.metronomeThread,NULL) ;
    pthread_join(appContext.mTimerContext.clockThread,NULL) ;

    SLresult result ;
    result = (*appContext.mOpenSlContext.queueInterface)->Clear(appContext.mOpenSlContext.queueInterface);
    assert(result == SL_RESULT_SUCCESS) ;
    result = (*appContext.mOpenSlContext.playInterface)->SetPlayState(appContext.mOpenSlContext.playInterface,SL_PLAYSTATE_STOPPED) ;
    assert(result == SL_RESULT_SUCCESS) ;


    //free(appContext.mBufferingContext.buffer[0]) ;
    //free(appContext.mBufferingContext.buffer[1]) ;

    //(*env)->DeleteGlobalRef(env,appContext.mJavaContext.mainActivityClass) ;
    //(*env)->DeleteGlobalRef(env,appContext.mJavaContext.mainActivityInstance) ;

    // Release the lock :
    pthread_mutex_unlock(&appContext.antiCorruptLock) ;

    return JNI_TRUE ;
}




