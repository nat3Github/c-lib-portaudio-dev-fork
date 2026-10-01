#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <aaudio/AAudio.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

PaError PaAAudio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

typedef aaudio_data_callback_result_t (*PaAAudioDataCallback)( AAudioStream *, void *, void *, int32_t );
typedef void (*PaAAudioErrorCallback)( AAudioStream *, void *, aaudio_result_t );

static struct
{
    aaudio_result_t (*createStreamBuilder)( AAudioStreamBuilder ** );
    void (*setDirection)( AAudioStreamBuilder *, aaudio_direction_t );
    void (*setPerformanceMode)( AAudioStreamBuilder *, aaudio_performance_mode_t );
    void (*setSharingMode)( AAudioStreamBuilder *, aaudio_sharing_mode_t );
    void (*setFormat)( AAudioStreamBuilder *, aaudio_format_t );
    void (*setChannelCount)( AAudioStreamBuilder *, int32_t );
    void (*setSampleRate)( AAudioStreamBuilder *, int32_t );
    void (*setDataCallback)( AAudioStreamBuilder *, PaAAudioDataCallback, void * );
    void (*setErrorCallback)( AAudioStreamBuilder *, PaAAudioErrorCallback, void * );
    aaudio_result_t (*openStream)( AAudioStreamBuilder *, AAudioStream ** );
    aaudio_result_t (*deleteBuilder)( AAudioStreamBuilder * );
    aaudio_result_t (*requestStart)( AAudioStream * );
    aaudio_result_t (*requestStop)( AAudioStream * );
    aaudio_result_t (*close)( AAudioStream * );
    aaudio_result_t (*waitForStateChange)( AAudioStream *, aaudio_stream_state_t, aaudio_stream_state_t *, int64_t );
    int32_t (*getSampleRate)( AAudioStream * );
    int32_t (*getFramesPerBurst)( AAudioStream * );
    int32_t (*getBufferCapacityInFrames)( AAudioStream * );
    aaudio_result_t (*setBufferSizeInFrames)( AAudioStream *, int32_t );
    int32_t (*getXRunCount)( AAudioStream * );
} aa;

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilAllocationGroup *allocations;
    int32_t framesPerBurst;
}
PaAAudioHostApiRepresentation;

typedef struct
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;
    AAudioStream *stream;
    int channelCount;
    double sampleRate;
    PaTime suggestedLatency;
    int32_t bufferFrames;
    int32_t xruns;
    pthread_mutex_t lock;
    pthread_t reopenThread;
    int reopenJoinable;
    volatile int reopenRunning;
    volatile int active;
    volatile int stopped;
}
PaAAudioStream;

static int LoadAAudio( void )
{
    char sdk[PROP_VALUE_MAX] = { 0 };
    __system_property_get( "ro.build.version.sdk", sdk );
    if( atoi( sdk ) < 27 )
        return 0;
    void *lib = dlopen( "libaaudio.so", RTLD_NOW );
    if( !lib )
        return 0;
#define PA_AAUDIO_LOAD( field, name ) if( !( *(void **)&aa.field = dlsym( lib, name ) ) ) return 0
    PA_AAUDIO_LOAD( createStreamBuilder, "AAudio_createStreamBuilder" );
    PA_AAUDIO_LOAD( setDirection, "AAudioStreamBuilder_setDirection" );
    PA_AAUDIO_LOAD( setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode" );
    PA_AAUDIO_LOAD( setSharingMode, "AAudioStreamBuilder_setSharingMode" );
    PA_AAUDIO_LOAD( setFormat, "AAudioStreamBuilder_setFormat" );
    PA_AAUDIO_LOAD( setChannelCount, "AAudioStreamBuilder_setChannelCount" );
    PA_AAUDIO_LOAD( setSampleRate, "AAudioStreamBuilder_setSampleRate" );
    PA_AAUDIO_LOAD( setDataCallback, "AAudioStreamBuilder_setDataCallback" );
    PA_AAUDIO_LOAD( setErrorCallback, "AAudioStreamBuilder_setErrorCallback" );
    PA_AAUDIO_LOAD( openStream, "AAudioStreamBuilder_openStream" );
    PA_AAUDIO_LOAD( deleteBuilder, "AAudioStreamBuilder_delete" );
    PA_AAUDIO_LOAD( requestStart, "AAudioStream_requestStart" );
    PA_AAUDIO_LOAD( requestStop, "AAudioStream_requestStop" );
    PA_AAUDIO_LOAD( close, "AAudioStream_close" );
    PA_AAUDIO_LOAD( waitForStateChange, "AAudioStream_waitForStateChange" );
    PA_AAUDIO_LOAD( getSampleRate, "AAudioStream_getSampleRate" );
    PA_AAUDIO_LOAD( getFramesPerBurst, "AAudioStream_getFramesPerBurst" );
    PA_AAUDIO_LOAD( getBufferCapacityInFrames, "AAudioStream_getBufferCapacityInFrames" );
    PA_AAUDIO_LOAD( setBufferSizeInFrames, "AAudioStream_setBufferSizeInFrames" );
    PA_AAUDIO_LOAD( getXRunCount, "AAudioStream_getXRunCount" );
#undef PA_AAUDIO_LOAD
    return 1;
}

static AAudioStream *OpenOutput( aaudio_sharing_mode_t sharing, int32_t channels, int32_t sampleRate,
                                 PaAAudioDataCallback dataCallback, PaAAudioErrorCallback errorCallback, void *user )
{
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *stream = NULL;
    if( aa.createStreamBuilder( &builder ) != AAUDIO_OK )
        return NULL;
    aa.setDirection( builder, AAUDIO_DIRECTION_OUTPUT );
    aa.setPerformanceMode( builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY );
    aa.setSharingMode( builder, sharing );
    aa.setFormat( builder, AAUDIO_FORMAT_PCM_FLOAT );
    aa.setChannelCount( builder, channels );
    aa.setSampleRate( builder, sampleRate );
    if( dataCallback )
        aa.setDataCallback( builder, dataCallback, user );
    if( errorCallback )
        aa.setErrorCallback( builder, errorCallback, user );
    if( aa.openStream( builder, &stream ) != AAUDIO_OK )
        stream = NULL;
    aa.deleteBuilder( builder );
    return stream;
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

PaError PaAAudio_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaAAudioHostApiRepresentation *aaudioHostApi = NULL;
    PaDeviceInfo *deviceInfo;
    AAudioStream *probe;
    double sampleRate;

    *hostApi = NULL;
    if( !LoadAAudio() )
        return paNoError;
    probe = OpenOutput( AAUDIO_SHARING_MODE_SHARED, 2, AAUDIO_UNSPECIFIED, NULL, NULL, NULL );
    if( !probe )
        return paNoError;

    aaudioHostApi = (PaAAudioHostApiRepresentation *)PaUtil_AllocateZeroInitializedMemory( sizeof(PaAAudioHostApiRepresentation) );
    if( !aaudioHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }
    aaudioHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !aaudioHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    sampleRate = aa.getSampleRate( probe );
    aaudioHostApi->framesPerBurst = aa.getFramesPerBurst( probe );
    if( aaudioHostApi->framesPerBurst <= 0 )
        aaudioHostApi->framesPerBurst = 192;

    *hostApi = &aaudioHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paAAudio;
    (*hostApi)->info.name = "AAudio";
    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.deviceCount = 0;

    (*hostApi)->deviceInfos = (PaDeviceInfo **)PaUtil_GroupAllocateZeroInitializedMemory(
            aaudioHostApi->allocations, sizeof(PaDeviceInfo *) );
    deviceInfo = (PaDeviceInfo *)PaUtil_GroupAllocateZeroInitializedMemory(
            aaudioHostApi->allocations, sizeof(PaDeviceInfo) );
    if( !(*hostApi)->deviceInfos || !deviceInfo )
    {
        result = paInsufficientMemory;
        goto error;
    }
    deviceInfo->structVersion = 2;
    deviceInfo->hostApi = hostApiIndex;
    deviceInfo->name = "AAudio Output";
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 2;
    deviceInfo->defaultSampleRate = sampleRate;
    deviceInfo->defaultLowOutputLatency = 2.0 * aaudioHostApi->framesPerBurst / sampleRate;
    deviceInfo->defaultHighOutputLatency = 8.0 * aaudioHostApi->framesPerBurst / sampleRate;
    (*hostApi)->deviceInfos[0] = deviceInfo;
    (*hostApi)->info.deviceCount = 1;

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &aaudioHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, StopStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );
    aa.close( probe );
    return result;

error:
    aa.close( probe );
    *hostApi = NULL;
    if( aaudioHostApi )
    {
        if( aaudioHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( aaudioHostApi->allocations );
            PaUtil_DestroyAllocationGroup( aaudioHostApi->allocations );
        }
        PaUtil_FreeMemory( aaudioHostApi );
    }
    return result;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAAudioHostApiRepresentation *aaudioHostApi = (PaAAudioHostApiRepresentation *)hostApi;
    if( aaudioHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( aaudioHostApi->allocations );
        PaUtil_DestroyAllocationGroup( aaudioHostApi->allocations );
    }
    PaUtil_FreeMemory( aaudioHostApi );
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

static aaudio_data_callback_result_t DataCallback( AAudioStream *s, void *user, void *audioData, int32_t numFrames )
{
    PaAAudioStream *stream = (PaAAudioStream *)user;
    PaStreamCallbackTimeInfo timeInfo;
    PaStreamCallbackFlags flags = 0;
    int callbackResult = paContinue;
    unsigned long framesProcessed;
    int32_t xruns = aa.getXRunCount( s );

    if( xruns > stream->xruns )
        flags |= paOutputUnderflow;
    stream->xruns = xruns;
    timeInfo.currentTime = PaUtil_GetTime();
    timeInfo.inputBufferAdcTime = 0;
    timeInfo.outputBufferDacTime = timeInfo.currentTime + stream->bufferFrames / stream->sampleRate;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, flags );
    PaUtil_SetOutputFrameCount( &stream->bufferProcessor, (unsigned long)numFrames );
    PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0, audioData, 0 );
    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );
    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

    if( callbackResult == paContinue )
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    stream->active = 0;
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    return AAUDIO_CALLBACK_RESULT_STOP;
}

static void ErrorCallback( AAudioStream *s, void *user, aaudio_result_t error );

static PaError OpenDevice( PaAAudioStream *stream )
{
    int32_t burst, capacity, bursts;
    double latencyFrames = stream->suggestedLatency * stream->sampleRate;
    AAudioStream *s = OpenOutput( AAUDIO_SHARING_MODE_EXCLUSIVE, stream->channelCount, (int32_t)stream->sampleRate,
                                  DataCallback, ErrorCallback, stream );
    if( !s )
        s = OpenOutput( AAUDIO_SHARING_MODE_SHARED, stream->channelCount, (int32_t)stream->sampleRate,
                        DataCallback, ErrorCallback, stream );
    if( !s )
        return paUnanticipatedHostError;
    burst = aa.getFramesPerBurst( s );
    if( burst <= 0 )
        burst = 192;
    bursts = latencyFrames > 0 ? (int32_t)( ( latencyFrames + burst - 1 ) / burst ) : 2;
    if( bursts < 1 )
        bursts = 1;
    capacity = aa.getBufferCapacityInFrames( s );
    stream->bufferFrames = bursts * burst;
    if( capacity > 0 && stream->bufferFrames > capacity )
        stream->bufferFrames = capacity;
    aa.setBufferSizeInFrames( s, stream->bufferFrames );
    stream->xruns = aa.getXRunCount( s );
    stream->stream = s;
    return paNoError;
}

static void CloseDevice( PaAAudioStream *stream )
{
    if( !stream->stream )
        return;
    aa.close( stream->stream );
    stream->stream = NULL;
}

static void *Reopen( void *arg )
{
    PaAAudioStream *stream = (PaAAudioStream *)arg;
    pthread_mutex_lock( &stream->lock );
    if( stream->active )
    {
        CloseDevice( stream );
        if( OpenDevice( stream ) != paNoError || aa.requestStart( stream->stream ) != AAUDIO_OK )
            stream->active = 0;
    }
    stream->reopenRunning = 0;
    pthread_mutex_unlock( &stream->lock );
    return NULL;
}

static void ErrorCallback( AAudioStream *s, void *user, aaudio_result_t error )
{
    PaAAudioStream *stream = (PaAAudioStream *)user;
    if( error != AAUDIO_ERROR_DISCONNECTED )
        return;
    if( pthread_mutex_trylock( &stream->lock ) != 0 )
        return;
    if( stream->active && !stream->reopenRunning && s == stream->stream )
    {
        if( stream->reopenJoinable )
            pthread_join( stream->reopenThread, NULL );
        stream->reopenJoinable = pthread_create( &stream->reopenThread, NULL, Reopen, stream ) == 0;
        stream->reopenRunning = stream->reopenJoinable;
    }
    pthread_mutex_unlock( &stream->lock );
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
    PaAAudioHostApiRepresentation *aaudioHostApi = (PaAAudioHostApiRepresentation *)hostApi;
    PaAAudioStream *stream;

    result = CheckParameters( hostApi, inputParameters, outputParameters );
    if( result != paNoError )
        return result;
    if( !streamCallback )
        return paNullCallback;
    if( ( streamFlags & paPlatformSpecificFlags ) != 0 )
        return paInvalidFlag;

    stream = (PaAAudioStream *)PaUtil_AllocateZeroInitializedMemory( sizeof(PaAAudioStream) );
    if( !stream )
        return paInsufficientMemory;
    pthread_mutex_init( &stream->lock, NULL );
    stream->channelCount = outputParameters->channelCount;
    stream->sampleRate = sampleRate;
    stream->suggestedLatency = outputParameters->suggestedLatency;
    stream->stopped = 1;

    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                           &aaudioHostApi->callbackStreamInterface, streamCallback, userData );
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    result = OpenDevice( stream );
    if( result != paNoError )
        goto error;

    result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              0, paFloat32, paFloat32,
              stream->channelCount, outputParameters->sampleFormat, paFloat32,
              sampleRate, streamFlags, framesPerBuffer,
              (unsigned long)aaudioHostApi->framesPerBurst, paUtilUnknownHostBufferSize,
              streamCallback, userData );
    if( result != paNoError )
        goto error;

    stream->streamRepresentation.streamInfo.inputLatency = 0;
    stream->streamRepresentation.streamInfo.outputLatency =
            ( PaUtil_GetBufferProcessorOutputLatencyFrames( &stream->bufferProcessor ) + stream->bufferFrames ) / sampleRate;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    *s = (PaStream *)stream;
    return paNoError;

error:
    CloseDevice( stream );
    pthread_mutex_destroy( &stream->lock );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );
    return result;
}

static PaError CloseStream( PaStream *s )
{
    PaAAudioStream *stream = (PaAAudioStream *)s;
    if( stream->reopenJoinable )
        pthread_join( stream->reopenThread, NULL );
    CloseDevice( stream );
    pthread_mutex_destroy( &stream->lock );
    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );
    return paNoError;
}

static PaError StartStream( PaStream *s )
{
    PaAAudioStream *stream = (PaAAudioStream *)s;
    PaError result = paNoError;
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );
    pthread_mutex_lock( &stream->lock );
    if( !stream->stream )
        result = OpenDevice( stream );
    if( result == paNoError )
    {
        stream->active = 1;
        stream->stopped = 0;
        if( aa.requestStart( stream->stream ) != AAUDIO_OK )
        {
            stream->active = 0;
            stream->stopped = 1;
            result = paUnanticipatedHostError;
        }
    }
    pthread_mutex_unlock( &stream->lock );
    return result;
}

static PaError StopStream( PaStream *s )
{
    PaAAudioStream *stream = (PaAAudioStream *)s;
    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
    pthread_mutex_lock( &stream->lock );
    stream->active = 0;
    if( stream->stream && aa.requestStop( stream->stream ) == AAUDIO_OK )
        aa.waitForStateChange( stream->stream, AAUDIO_STREAM_STATE_STOPPING, &next, 1000000000LL );
    stream->stopped = 1;
    pthread_mutex_unlock( &stream->lock );
    if( stream->reopenJoinable )
    {
        pthread_join( stream->reopenThread, NULL );
        stream->reopenJoinable = 0;
    }
    return paNoError;
}

static PaError IsStreamStopped( PaStream *s )
{
    return ( (PaAAudioStream *)s )->stopped;
}

static PaError IsStreamActive( PaStream *s )
{
    return ( (PaAAudioStream *)s )->active;
}

static PaTime GetStreamTime( PaStream *s )
{
    (void)s;
    return PaUtil_GetTime();
}

static double GetStreamCpuLoad( PaStream *s )
{
    return PaUtil_GetCpuLoad( &( (PaAAudioStream *)s )->cpuLoadMeasurer );
}
