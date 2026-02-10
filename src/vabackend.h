#ifndef VABACKEND_H
#define VABACKEND_H

#include <ffnvcodec/dynlink_loader.h>
#include <va/va_backend.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <stdint.h>
#include <va/va_drmcommon.h>
#include <ffnvcodec/nvEncodeAPI.h>

#include <pthread.h>
#include "list.h"
#include "direct/nv-driver.h"
#include "common.h"

#define SURFACE_QUEUE_SIZE 16
#define MAX_IMAGE_COUNT 64
#define MAX_PROFILES 32
#define NVENC_MAX_IO_BUFFERS 4

typedef struct {
    void        *buf;
    uint64_t    size;
    uint64_t    allocated;
} AppendableBuffer;

typedef enum
{
    OBJECT_TYPE_CONFIG,
    OBJECT_TYPE_CONTEXT,
    OBJECT_TYPE_SURFACE,
    OBJECT_TYPE_BUFFER,
    OBJECT_TYPE_IMAGE
} ObjectType;

typedef struct Object_t
{
    ObjectType      type;
    VAGenericID     id;
    void            *obj;
} *Object;

typedef struct
{
    unsigned int    elements;
    size_t          size;
    VABufferType    bufferType;
    void            *ptr;
    size_t          offset;
    bool            ownsPtr;
    VACodedBufferSegment *codedSeg;
    uint8_t         *codedData;
    size_t          codedCapacity;
    VASurfaceID     surfaceId;
    int             commitOnUnmap;
    int             mappedSurfaceLock;
} NVBuffer;

struct _NVContext;
struct _BackingImage;

typedef struct
{
    uint32_t                width;
    uint32_t                height;
    cudaVideoSurfaceFormat  format;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    int                     pictureIdx;
    struct _NVContext       *context;
    int                     progressiveFrame;
    int                     topFieldFirst;
    int                     secondField;
    int                     order_hint; //needed for AV1
    struct _BackingImage    *backingImage;
    int                     resolving;
    pthread_mutex_t         mutex;
    pthread_cond_t          cond;
    bool                    decodeFailed;
    // host-side NV12 staging for VAAPI encode path
    uint8_t                 *hostData;
    size_t                  hostPitch;
    size_t                  hostYSize;
    size_t                  hostTotalSize;
    uint32_t                contentX;
    uint32_t                contentY;
    uint32_t                contentWidth;
    uint32_t                contentHeight;
    int                     contentValid;
    VASurfaceStatus         status;
} NVSurface;

typedef enum
{
    NV_FORMAT_NONE,
    NV_FORMAT_NV12,
    NV_FORMAT_P010,
    NV_FORMAT_P012,
    NV_FORMAT_P016,
    NV_FORMAT_444P,
    NV_FORMAT_Q416
} NVFormat;

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    NVFormat    format;
    uint32_t    numPlanes;
    uint32_t    pitches[3];
    uint32_t    offsets[3];
    NVBuffer    *imageBuffer;
} NVImage;

typedef struct {
    CUexternalMemory extMem;
    CUmipmappedArray mipmapArray;
} NVCudaImage;

typedef struct _BackingImage {
    NVSurface   *surface;
    EGLImage    image;
    CUarray     arrays[3];
    uint32_t    width;
    uint32_t    height;
    int         fourcc;
    int         fds[4];
    int         offsets[4];
    int         strides[4];
    uint64_t    mods[4];
    uint32_t    size[4];
    //direct backend only
    NVCudaImage cudaImages[3];
    NVFormat    format;
} BackingImage;

struct _NVDriver;

typedef struct {
    const char *name;
    bool (*initExporter)(struct _NVDriver *drv);
    void (*releaseExporter)(struct _NVDriver *drv);
    bool (*exportCudaPtr)(struct _NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch);
    void (*detachBackingImageFromSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*realiseSurface)(struct _NVDriver *drv, NVSurface *surface);
    bool (*fillExportDescriptor)(struct _NVDriver *drv, NVSurface *surface, VADRMPRIMESurfaceDescriptor *desc);
    void (*destroyAllBackingImage)(struct _NVDriver *drv);
} NVBackend;

typedef struct _NVDriver
{
    CudaFunctions           *cu;
    CuvidFunctions          *cv;
    void                    *nvencLib;
    NV_ENCODE_API_FUNCTION_LIST nvencFuncs;
    bool                    nvencAvailable;
    CUcontext               cudaContext;
    CUvideoctxlock          vidLock;
    Array/*<Object>*/       objects;
    pthread_mutex_t         objectCreationMutex;
    VAGenericID             nextObjId;
    bool                    useCorrectNV12Format;
    bool                    supports16BitSurface;
    bool                    supports444Surface;
    int                     cudaGpuId;
    int                     drmFd;
    pthread_mutex_t         exportMutex;
    pthread_mutex_t         imagesMutex;
    Array/*<NVEGLImage>*/   images;
    const NVBackend         *backend;
    //fields for direct backend
    NVDriverContext         driverContext;
    //fields for egl backend
    EGLDeviceEXT            eglDevice;
    EGLDisplay              eglDisplay;
    EGLContext              eglContext;
    EGLStreamKHR            eglStream;
    CUeglStreamConnection   cuStreamConnection;
    int                     numFramesPresented;
    int                     profileCount;
    VAProfile               profiles[MAX_PROFILES];
} NVDriver;

struct _NVCodec;

typedef enum
{
    NV_CONTEXT_DECODE = 0,
    NV_CONTEXT_ENCODE = 1
} NVContextMode;

typedef struct _NVContext
{
    NVDriver            *drv;
    NVContextMode       mode;
    VAProfile           profile;
    VAEntrypoint        entrypoint;
    uint32_t            width;
    uint32_t            height;
    CUvideodecoder      decoder;
    NVSurface           *renderTarget;
    void                *lastSliceParams;
    unsigned int        lastSliceParamsCount;
    AppendableBuffer    bitstreamBuffer;
    AppendableBuffer    sliceOffsets;
    CUVIDPICPARAMS      pPicParams;
    const struct _NVCodec *codec;
    int                 currentPictureId;
    pthread_t           resolveThread;
    pthread_mutex_t     resolveMutex;
    pthread_cond_t      resolveCondition;
    NVSurface*          surfaceQueue[SURFACE_QUEUE_SIZE];
    int                 surfaceQueueReadIdx;
    int                 surfaceQueueWriteIdx;
    volatile bool       exiting;
    pthread_mutex_t     surfaceCreationMutex;
    int                 surfaceCount;
    bool                firstKeyframeValid;
    // encode state (valid when mode == NV_CONTEXT_ENCODE)
    void                *nvencEncoder;
    NV_ENC_INPUT_PTR    nvencInputBuffers[NVENC_MAX_IO_BUFFERS];
    NV_ENC_OUTPUT_PTR   nvencOutputBuffers[NVENC_MAX_IO_BUFFERS];
    uint32_t            nvencIoBufferCount;
    uint32_t            nvencIoBufferIndex;
    GUID                encCodecGuid;
    GUID                encProfileGuid;
    GUID                encPresetGuid;
    int                 encIsHevc;
    NV_ENC_CONFIG       encConfig;
    NV_ENC_INITIALIZE_PARAMS encInitParams;
    int                 encNeedsReconfigure;
    VASurfaceID         encCurrentSurface;
    VABufferID          encCodedBufId;
    int                 encHasCodedBuf;
    int                 encForceIdr;
    uint32_t            encFrameNum;
    uint32_t            encVisibleX;
    uint32_t            encVisibleY;
    uint32_t            encVisibleWidth;
    uint32_t            encVisibleHeight;
    int                 encVisibleValid;
    uint8_t             *encLastFrameData;
    size_t              encLastFrameSize;
    int                 encLastFrameValid;
    uint64_t            encLastReconfigureNs;
    uint32_t            encStartupIdrLeft;
    uint32_t            encNeedMoreInputStreak;
} NVContext;

typedef struct
{
    VAProfile               profile;
    VAEntrypoint            entrypoint;
    cudaVideoSurfaceFormat  surfaceFormat;
    cudaVideoChromaFormat   chromaFormat;
    int                     bitDepth;
    cudaVideoCodec          cudaCodec;
    bool                    isEncode;
    uint32_t                encodeRCMode;
} NVConfig;

typedef void (*HandlerFunc)(NVContext*, NVBuffer* , CUVIDPICPARAMS*);
typedef cudaVideoCodec (*ComputeCudaCodec)(VAProfile);

//padding/alignment is very important to this structure as it's placed in it's own section
//in the executable.
struct _NVCodec {
    ComputeCudaCodec    computeCudaCodec;
    HandlerFunc         handlers[VABufferTypeMax];
    int                 supportedProfileCount;
    const VAProfile     *supportedProfiles;
};

typedef struct _NVCodec NVCodec;

typedef struct
{
    uint32_t bppc; // bytes per pixel per channel
    uint32_t numPlanes;
    uint32_t fourcc;
    bool     is16bits;
    bool     isYuv444;
    NVFormatPlane plane[3];
    VAImageFormat vaFormat;
} NVFormatInfo;

extern const NVFormatInfo formatsInfo[];

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size);
int pictureIdxFromSurfaceId(NVDriver *ctx, VASurfaceID surf);
NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf);
bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line);
void logger(const char *filename, const char *function, int line, const char *msg, ...);
#define CHECK_CUDA_RESULT(err) checkCudaErrors(err, __FILE__, __func__, __LINE__)
#define CHECK_CUDA_RESULT_RETURN(err, ret) if (checkCudaErrors(err, __FILE__, __func__, __LINE__)) { return ret; }
#define cudaVideoCodec_NONE ((cudaVideoCodec) -1)
#define LOG(...) logger(__FILE__, __func__, __LINE__, __VA_ARGS__);
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PTROFF(base, bytes) ((void *)((unsigned char *)(base) + (bytes)))
#define DECLARE_CODEC(name) \
    __attribute__((used)) \
    __attribute__((retain)) \
    __attribute__((section("nvd_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#define DECLARE_DISABLED_CODEC(name) \
    __attribute__((section("nvd_disabled_codecs"))) \
    __attribute__((aligned(__alignof__(NVCodec)))) \
    NVCodec name

#endif // VABACKEND_H
