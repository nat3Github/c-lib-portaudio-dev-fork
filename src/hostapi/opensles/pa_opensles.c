#include <dlfcn.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "pa_android.h"

PaError PaOpenSLES_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

static JavaVM *paJavaVM;
static jobject paJavaContext;

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilAllocationGroup *allocations;
    SLObjectItf engineObject;
    SLEngineItf engine;
    SLObjectItf outputMix;
    int nativeSampleRate;
    int nativeFramesPerBuffer;
}
PaOpenSLESHostApiRepresentation;

typedef struct
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;
    SLObjectItf player;
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf queue;
    short *buffers;
    unsigned long framesPerHostBuffer;
    int bufferCount;
    int nextBuffer;
    int channelCount;
    double sampleRate;
    volatile int active;
    volatile int stopped;
}
PaOpenSLESStream;

static JNIEnv *AttachJava( int *attached )
{
    JNIEnv *env = NULL;
    *attached = 0;
    if( !paJavaVM )
    {
        typedef jint (*GetCreatedJavaVMs)( JavaVM **, jsize, jsize * );
        GetCreatedJavaVMs fn = (GetCreatedJavaVMs)dlsym( RTLD_DEFAULT, "JNI_GetCreatedJavaVMs" );
        void *lib = fn ? NULL : dlopen( "libnativehelper.so", RTLD_NOW );
        jsize count = 0;
        if( lib )
            fn = (GetCreatedJavaVMs)dlsym( lib, "JNI_GetCreatedJavaVMs" );
        if( !fn || fn( &paJavaVM, 1, &count ) != JNI_OK || count < 1 )
            paJavaVM = NULL;
    }
    if( !paJavaVM )
        return NULL;
    jint r = (*paJavaVM)->GetEnv( paJavaVM, (void **)&env, JNI_VERSION_1_6 );
    if( r == JNI_EDETACHED )
    {
        if( (*paJavaVM)->AttachCurrentThread( paJavaVM, &env, NULL ) != JNI_OK )
            return NULL;
        *attached = 1;
    }
    else if( r != JNI_OK )
    {
        return NULL;
    }
    return env;
}

static void DetachJava( int attached )
{
    if( attached )
        (*paJavaVM)->DetachCurrentThread( paJavaVM );
}

void PaAndroid_SetJavaContext( void *javaVM, void *context )
{
    int attached;
    JNIEnv *env;
    paJavaVM = (JavaVM *)javaVM;
    env = AttachJava( &attached );
    if( !env )
        return;
    if( paJavaContext )
        (*env)->DeleteGlobalRef( env, paJavaContext );
    paJavaContext = context ? (*env)->NewGlobalRef( env, (jobject)context ) : NULL;
    DetachJava( attached );
}

static jobject JavaContext( JNIEnv *env )
{
    jclass cls;
    jmethodID m;
    jobject app;
    if( paJavaContext )
        return (*env)->NewLocalRef( env, paJavaContext );
    cls = (*env)->FindClass( env, "android/app/ActivityThread" );
    if( (*env)->ExceptionCheck( env ) || !cls )
        return NULL;
    m = (*env)->GetStaticMethodID( env, cls, "currentApplication", "()Landroid/app/Application;" );
    app = ( !(*env)->ExceptionCheck( env ) && m ) ? (*env)->CallStaticObjectMethod( env, cls, m ) : NULL;
    (*env)->DeleteLocalRef( env, cls );
    return app;
}

static int AudioManagerProperty( JNIEnv *env, jobject audioManager, jmethodID getProperty, const char *name )
{
    int value = 0;
    jstring key = (*env)->NewStringUTF( env, name );
    jstring result = (jstring)(*env)->CallObjectMethod( env, audioManager, getProperty, key );
    if( !(*env)->ExceptionCheck( env ) && result )
    {
        const char *chars = (*env)->GetStringUTFChars( env, result, NULL );
        if( chars )
        {
            value = atoi( chars );
            (*env)->ReleaseStringUTFChars( env, result, chars );
        }
        (*env)->DeleteLocalRef( env, result );
    }
    (*env)->DeleteLocalRef( env, key );
    return value;
}

static void QueryNativeParameters( int *sampleRate, int *framesPerBuffer )
{
    int attached;
    JNIEnv *env = AttachJava( &attached );
    jobject context, audioManager = NULL;
    jclass contextClass, managerClass;
    jmethodID getSystemService, getProperty;
    jstring service;
    if( !env )
        return;
    context = JavaContext( env );
    if( context )
    {
        contextClass = (*env)->GetObjectClass( env, context );
        getSystemService = (*env)->GetMethodID( env, contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;" );
        service = (*env)->NewStringUTF( env, "audio" );
        if( !(*env)->ExceptionCheck( env ) && getSystemService )
            audioManager = (*env)->CallObjectMethod( env, context, getSystemService, service );
        if( !(*env)->ExceptionCheck( env ) && audioManager )
        {
            managerClass = (*env)->GetObjectClass( env, audioManager );
            getProperty = (*env)->GetMethodID( env, managerClass, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;" );
            if( !(*env)->ExceptionCheck( env ) && getProperty )
            {
                int rate = AudioManagerProperty( env, audioManager, getProperty, "android.media.property.OUTPUT_SAMPLE_RATE" );
                int frames = AudioManagerProperty( env, audioManager, getProperty, "android.media.property.OUTPUT_FRAMES_PER_BUFFER" );
                if( rate > 0 )
                    *sampleRate = rate;
                if( frames > 0 )
                    *framesPerBuffer = frames;
            }
            (*env)->DeleteLocalRef( env, managerClass );
            (*env)->DeleteLocalRef( env, audioManager );
        }
        (*env)->DeleteLocalRef( env, service );
        (*env)->DeleteLocalRef( env, contextClass );
        (*env)->DeleteLocalRef( env, context );
    }
    if( (*env)->ExceptionCheck( env ) )
        (*env)->ExceptionClear( env );
    DetachJava( attached );
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream **s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError CloseStream( PaStream *stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream *stream );

static void DestroyEngine( PaOpenSLESHostApiRepresentation *slHostApi )
{
    if( slHostApi->outputMix )
        (*slHostApi->outputMix)->Destroy( slHostApi->outputMix );
    if( slHostApi->engineObject )
        (*slHostApi->engineObject)->Destroy( slHostApi->engineObject );
    if( slHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( slHostApi->allocations );
        PaUtil_DestroyAllocationGroup( slHostApi->allocations );
    }
    PaUtil_FreeMemory( slHostApi );
}

PaError PaOpenSLES_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaOpenSLESHostApiRepresentation *slHostApi;
    PaDeviceInfo *deviceInfo;
    double rate, frames;

    *hostApi = NULL;
    slHostApi = (PaOpenSLESHostApiRepresentation *)PaUtil_AllocateZeroInitializedMemory( sizeof(PaOpenSLESHostApiRepresentation) );
    if( !slHostApi )
        return paInsufficientMemory;
    slHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !slHostApi->allocations )
    {
        DestroyEngine( slHostApi );
        return paInsufficientMemory;
    }

    if( slCreateEngine( &slHostApi->engineObject, 0, NULL, 0, NULL, NULL ) != SL_RESULT_SUCCESS
        || (*slHostApi->engineObject)->Realize( slHostApi->engineObject, SL_BOOLEAN_FALSE ) != SL_RESULT_SUCCESS
        || (*slHostApi->engineObject)->GetInterface( slHostApi->engineObject, SL_IID_ENGINE, &slHostApi->engine ) != SL_RESULT_SUCCESS
        || (*slHostApi->engine)->CreateOutputMix( slHostApi->engine, &slHostApi->outputMix, 0, NULL, NULL ) != SL_RESULT_SUCCESS
        || (*slHostApi->outputMix)->Realize( slHostApi->outputMix, SL_BOOLEAN_FALSE ) != SL_RESULT_SUCCESS )
    {
        DestroyEngine( slHostApi );
        return paNoError;
    }

    slHostApi->nativeSampleRate = 48000;
    slHostApi->nativeFramesPerBuffer = 256;
    QueryNativeParameters( &slHostApi->nativeSampleRate, &slHostApi->nativeFramesPerBuffer );
    rate = slHostApi->nativeSampleRate;
    frames = slHostApi->nativeFramesPerBuffer;

    *hostApi = &slHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paOpenSLES;
    (*hostApi)->info.name = "OpenSL ES";
    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.deviceCount = 0;

    (*hostApi)->deviceInfos = (PaDeviceInfo **)PaUtil_GroupAllocateZeroInitializedMemory(
            slHostApi->allocations, sizeof(PaDeviceInfo *) );
    deviceInfo = (PaDeviceInfo *)PaUtil_GroupAllocateZeroInitializedMemory(
            slHostApi->allocations, sizeof(PaDeviceInfo) );
    if( !(*hostApi)->deviceInfos || !deviceInfo )
    {
        *hostApi = NULL;
        DestroyEngine( slHostApi );
        return paInsufficientMemory;
    }
    deviceInfo->structVersion = 2;
    deviceInfo->hostApi = hostApiIndex;
    deviceInfo->name = "OpenSL ES Output";
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 2;
    deviceInfo->defaultSampleRate = rate;
    deviceInfo->defaultLowOutputLatency = 2.0 * frames / rate;
    deviceInfo->defaultHighOutputLatency = 4.0 * frames / rate;
    (*hostApi)->deviceInfos[0] = deviceInfo;
    (*hostApi)->info.deviceCount = 1;

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &slHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, StopStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );
    return paNoError;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    DestroyEngine( (PaOpenSLESHostApiRepresentation *)hostApi );
}

static PaError CheckParameters( struct PaUtilHostApiRepresentation *hostApi,
                                const PaStreamParameters *inputParameters,
                                const PaStreamParameters *outputParameters )
{
    if( inputParameters )
        return paInvalidChannelCount;
    if( !outputParameters )
        return paInvalidDevice;
    if( outputParameters->sampleFormat & paCustomFormat )
        return paSampleFormatNotSupported;
    if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
        return paInvalidDevice;
    if( outputParameters->channelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
        return paInvalidChannelCount;
    if( outputParameters->hostApiSpecificStreamInfo )
        return paIncompatibleHostApiSpecificStreamInfo;
    return paNoError;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    PaError err = CheckParameters( hostApi, inputParameters, outputParameters );
    (void)sampleRate;
    return err == paNoError ? paFormatIsSupported : err;
}

static void BufferCallback( SLAndroidSimpleBufferQueueItf queue, void *user )
{
    PaOpenSLESStream *stream = (PaOpenSLESStream *)user;
    PaStreamCallbackTimeInfo timeInfo;
    int callbackResult = paContinue;
    unsigned long framesProcessed;
    size_t samples = stream->framesPerHostBuffer * stream->channelCount;
    short *buffer;

    if( !stream->active )
        return;
    buffer = stream->buffers + samples * stream->nextBuffer;
    timeInfo.currentTime = PaUtil_GetTime();
    timeInfo.inputBufferAdcTime = 0;
    timeInfo.outputBufferDacTime = timeInfo.currentTime
        + (double)stream->framesPerHostBuffer * ( stream->bufferCount - 1 ) / stream->sampleRate;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, 0 );
    PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 );
    PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0, buffer, 0 );
    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );
    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

    if( callbackResult != paContinue )
    {
        stream->active = 0;
        if( stream->streamRepresentation.streamFinishedCallback )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
        return;
    }
    (*queue)->Enqueue( queue, buffer, (SLuint32)( samples * sizeof(short) ) );
    stream->nextBuffer = ( stream->nextBuffer + 1 ) % stream->bufferCount;
}

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream **s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result;
    PaOpenSLESHostApiRepresentation *slHostApi = (PaOpenSLESHostApiRepresentation *)hostApi;
    PaOpenSLESStream *stream;
    double latencyFrames;
    SLAndroidConfigurationItf config;
    SLuint32 performanceMode = SL_ANDROID_PERFORMANCE_LATENCY;
    SLDataLocator_AndroidSimpleBufferQueue queueLocator;
    SLDataFormat_PCM format;
    SLDataSource source;
    SLDataLocator_OutputMix mixLocator;
    SLDataSink sink;
    const SLInterfaceID ids[2] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION };
    const SLboolean required[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE };

    result = CheckParameters( hostApi, inputParameters, outputParameters );
    if( result != paNoError )
        return result;
    if( !streamCallback )
        return paNullCallback;
    if( ( streamFlags & paPlatformSpecificFlags ) != 0 )
        return paInvalidFlag;

    stream = (PaOpenSLESStream *)PaUtil_AllocateZeroInitializedMemory( sizeof(PaOpenSLESStream) );
    if( !stream )
        return paInsufficientMemory;
    stream->channelCount = outputParameters->channelCount;
    stream->sampleRate = sampleRate;
    stream->stopped = 1;
    stream->framesPerHostBuffer = (unsigned long)slHostApi->nativeFramesPerBuffer;
    if( (int)sampleRate != slHostApi->nativeSampleRate )
        stream->framesPerHostBuffer = (unsigned long)( stream->framesPerHostBuffer * sampleRate / slHostApi->nativeSampleRate );
    if( stream->framesPerHostBuffer < 64 )
        stream->framesPerHostBuffer = 64;
    latencyFrames = outputParameters->suggestedLatency * sampleRate;
    stream->bufferCount = latencyFrames > 0
        ? (int)( ( latencyFrames + stream->framesPerHostBuffer - 1 ) / stream->framesPerHostBuffer )
        : 2;
    if( stream->bufferCount < 2 )
        stream->bufferCount = 2;

    stream->buffers = (short *)PaUtil_AllocateZeroInitializedMemory(
            sizeof(short) * stream->framesPerHostBuffer * stream->channelCount * stream->bufferCount );
    if( !stream->buffers )
    {
        result = paInsufficientMemory;
        goto error;
    }

    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                           &slHostApi->callbackStreamInterface, streamCallback, userData );
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    queueLocator.locatorType = SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE;
    queueLocator.numBuffers = (SLuint32)stream->bufferCount;
    format.formatType = SL_DATAFORMAT_PCM;
    format.numChannels = (SLuint32)stream->channelCount;
    format.samplesPerSec = (SLuint32)( sampleRate * 1000 );
    format.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
    format.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
    format.channelMask = stream->channelCount == 1 ? SL_SPEAKER_FRONT_CENTER : ( SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT );
    format.endianness = SL_BYTEORDER_LITTLEENDIAN;
    source.pLocator = &queueLocator;
    source.pFormat = &format;
    mixLocator.locatorType = SL_DATALOCATOR_OUTPUTMIX;
    mixLocator.outputMix = slHostApi->outputMix;
    sink.pLocator = &mixLocator;
    sink.pFormat = NULL;

    if( (*slHostApi->engine)->CreateAudioPlayer( slHostApi->engine, &stream->player, &source, &sink, 2, ids, required ) != SL_RESULT_SUCCESS )
    {
        stream->player = NULL;
        result = paUnanticipatedHostError;
        goto error;
    }
    if( (*stream->player)->GetInterface( stream->player, SL_IID_ANDROIDCONFIGURATION, &config ) == SL_RESULT_SUCCESS )
        (*config)->SetConfiguration( config, SL_ANDROID_KEY_PERFORMANCE_MODE, &performanceMode, sizeof(performanceMode) );
    if( (*stream->player)->Realize( stream->player, SL_BOOLEAN_FALSE ) != SL_RESULT_SUCCESS
        || (*stream->player)->GetInterface( stream->player, SL_IID_PLAY, &stream->play ) != SL_RESULT_SUCCESS
        || (*stream->player)->GetInterface( stream->player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &stream->queue ) != SL_RESULT_SUCCESS
        || (*stream->queue)->RegisterCallback( stream->queue, BufferCallback, stream ) != SL_RESULT_SUCCESS )
    {
        result = paUnanticipatedHostError;
        goto error;
    }

    result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              0, paInt16, paInt16,
              stream->channelCount, outputParameters->sampleFormat, paInt16,
              sampleRate, streamFlags, framesPerBuffer,
              stream->framesPerHostBuffer, paUtilFixedHostBufferSize,
              streamCallback, userData );
    if( result != paNoError )
        goto error;

    stream->streamRepresentation.streamInfo.inputLatency = 0;
    stream->streamRepresentation.streamInfo.outputLatency =
            ( PaUtil_GetBufferProcessorOutputLatencyFrames( &stream->bufferProcessor )
              + (double)stream->framesPerHostBuffer * stream->bufferCount ) / sampleRate;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    *s = (PaStream *)stream;
    return paNoError;

error:
    if( stream->player )
        (*stream->player)->Destroy( stream->player );
    if( stream->buffers )
        PaUtil_FreeMemory( stream->buffers );
    PaUtil_FreeMemory( stream );
    return result;
}

static PaError CloseStream( PaStream *s )
{
    PaOpenSLESStream *stream = (PaOpenSLESStream *)s;
    (*stream->player)->Destroy( stream->player );
    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream->buffers );
    PaUtil_FreeMemory( stream );
    return paNoError;
}

static PaError StartStream( PaStream *s )
{
    PaOpenSLESStream *stream = (PaOpenSLESStream *)s;
    size_t samples = stream->framesPerHostBuffer * stream->channelCount;
    int i;
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );
    (*stream->queue)->Clear( stream->queue );
    memset( stream->buffers, 0, sizeof(short) * samples * stream->bufferCount );
    stream->nextBuffer = 0;
    stream->active = 1;
    stream->stopped = 0;
    for( i = 0; i < stream->bufferCount; ++i )
        (*stream->queue)->Enqueue( stream->queue, stream->buffers + samples * i, (SLuint32)( samples * sizeof(short) ) );
    if( (*stream->play)->SetPlayState( stream->play, SL_PLAYSTATE_PLAYING ) != SL_RESULT_SUCCESS )
    {
        stream->active = 0;
        stream->stopped = 1;
        return paUnanticipatedHostError;
    }
    return paNoError;
}

static PaError StopStream( PaStream *s )
{
    PaOpenSLESStream *stream = (PaOpenSLESStream *)s;
    stream->active = 0;
    (*stream->play)->SetPlayState( stream->play, SL_PLAYSTATE_STOPPED );
    (*stream->queue)->Clear( stream->queue );
    stream->stopped = 1;
    return paNoError;
}

static PaError IsStreamStopped( PaStream *s )
{
    return ( (PaOpenSLESStream *)s )->stopped;
}

static PaError IsStreamActive( PaStream *s )
{
    return ( (PaOpenSLESStream *)s )->active;
}

static PaTime GetStreamTime( PaStream *s )
{
    (void)s;
    return PaUtil_GetTime();
}

static double GetStreamCpuLoad( PaStream *s )
{
    return PaUtil_GetCpuLoad( &( (PaOpenSLESStream *)s )->cpuLoadMeasurer );
}
