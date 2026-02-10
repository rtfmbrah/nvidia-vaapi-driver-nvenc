#define _GNU_SOURCE

#include "vabackend.h"
#include "backend-common.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <drm_fourcc.h>

#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>

#include <time.h>
#include <dlfcn.h>

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<pthread_np.h>)
#include <pthread_np.h>
#define gettid pthread_getthreadid_np
#define HAVE_GETTID 1
#endif

#ifndef HAVE_GETTID
#include <sys/syscall.h>
/* Bionic and glibc >= 2.30 declare gettid() system call wrapper in unistd.h and
 * has a definition for it */
#ifdef __BIONIC__
#define HAVE_GETTID 1
#elif !defined(__GLIBC_PREREQ)
#define HAVE_GETTID 0
#elif !__GLIBC_PREREQ(2,30)
#define HAVE_GETTID 0
#else
#define HAVE_GETTID 1
#endif
#endif

static pid_t nv_gettid(void)
{
#if HAVE_GETTID
    return gettid();
#else
    return syscall(__NR_gettid);
#endif
}

static pthread_mutex_t concurrency_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t instances;
static uint32_t max_instances;

static CudaFunctions *cu;
static CuvidFunctions *cv;

extern const NVCodec __start_nvd_codecs[];
extern const NVCodec __stop_nvd_codecs[];

static FILE *LOG_OUTPUT;

static int gpu = -1;
static enum {
    EGL, DIRECT
} backend = DIRECT;
static int g_disableDecoder = 0;

#define NVENC_CODED_BUF_SIZE (4 * 1024 * 1024)
#define NVENC_PITCH_ALIGN 64
#define NVENC_HEIGHT_ALIGN 16

static int g_encStrictMode = 0;
static uint32_t g_encForceIdrEvery = 0;
static uint32_t g_encStartupIdrFrames = 4;
static uint32_t g_encReconfigureMinMs = 800;
static int g_encVisibleReconfigure = 0;
static int g_warnedVp9Av1NoRtformat = 0;
static pthread_mutex_t g_encDebugMutex = PTHREAD_MUTEX_INITIALIZER;
static int g_encDebugInitDone = 0;
static int g_encDebugEnabled = 0;
static uint32_t g_encDebugMaxFrames = 120;
static uint32_t g_encDebugStartFrame = 0;
static uint32_t g_encDebugFrameIndex = 0;
static char g_encDebugDir[PATH_MAX] = "/tmp/nvd-enc-debug";

static void nvencDestroyIoBuffers(NVDriver *drv, NVContext *nvCtx);

const NVFormatInfo formatsInfo[] =
{
    [NV_FORMAT_NONE] = {0},
    [NV_FORMAT_NV12] = {1, 2, DRM_FORMAT_NV12,     false, false, {{1, DRM_FORMAT_R8,       {0,0}}, {2, DRM_FORMAT_RG88,   {1,1}}},                            {VA_FOURCC_NV12, VA_LSB_FIRST,   12, 0,0,0,0,0}},
    [NV_FORMAT_P010] = {2, 2, DRM_FORMAT_P010,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P010, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P012] = {2, 2, DRM_FORMAT_P012,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P012, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_P016] = {2, 2, DRM_FORMAT_P016,     true,  false, {{1, DRM_FORMAT_R16,      {0,0}}, {2, DRM_FORMAT_RG1616, {1,1}}},                            {VA_FOURCC_P016, VA_LSB_FIRST,   24, 0,0,0,0,0}},
    [NV_FORMAT_444P] = {1, 3, DRM_FORMAT_YUV444,   false, true,  {{1, DRM_FORMAT_R8,       {0,0}}, {1, DRM_FORMAT_R8,     {0,0}}, {1, DRM_FORMAT_R8, {0,0}}}, {VA_FOURCC_444P, VA_LSB_FIRST,   24, 0,0,0,0,0}},
#if VA_CHECK_VERSION(1, 20, 0)
    [NV_FORMAT_Q416] = {2, 3, DRM_FORMAT_INVALID,  true,  true,  {{1, DRM_FORMAT_R16,      {0,0}}, {1, DRM_FORMAT_R16,    {0,0}}, {1, DRM_FORMAT_R16,{0,0}}}, {VA_FOURCC_Q416, VA_LSB_FIRST,   48, 0,0,0,0,0}},
#endif
};

static NVFormat nvFormatFromVaFormat(uint32_t fourcc) {
    for (uint32_t i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].vaFormat.fourcc == fourcc) {
            return i;
        }
    }
    return NV_FORMAT_NONE;
}

static uint64_t fnv1a64_append(uint64_t h, const uint8_t *data, size_t len)
{
    const uint64_t prime = 1099511628211ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t) data[i];
        h *= prime;
    }
    return h;
}

static uint32_t parseEnvU32(const char *name, uint32_t fallback)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(v, &end, 10);
    if (end == v || *end != '\0') {
        return fallback;
    }
    if (parsed > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t) parsed;
}

static uint64_t getMonotonicNs(void)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return ((uint64_t) tp.tv_sec * 1000000000ULL) + (uint64_t) tp.tv_nsec;
}

static int envFlagEnabled(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0) {
        return 0;
    }
    return 1;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((format(gnu_printf, 2, 3)))
#endif
static void nvencHealthLog(const char *level, const char *msg, ...)
{
    va_list args;
    fprintf(stderr, "[vaapi-nvenc] %s: ", level);
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static void nvencDebugInitOnce(void)
{
    pthread_mutex_lock(&g_encDebugMutex);
    if (g_encDebugInitDone) {
        pthread_mutex_unlock(&g_encDebugMutex);
        return;
    }
    g_encDebugInitDone = 1;

    const char *enabled = getenv("NVD_ENC_DEBUG");
    if (enabled == NULL || enabled[0] == '\0' || strcmp(enabled, "0") == 0) {
        pthread_mutex_unlock(&g_encDebugMutex);
        return;
    }
    g_encDebugEnabled = 1;
    g_encDebugMaxFrames = parseEnvU32("NVD_ENC_DEBUG_MAX_FRAMES", 120);
    g_encDebugStartFrame = parseEnvU32("NVD_ENC_DEBUG_START_FRAME", 0);

    const char *dir = getenv("NVD_ENC_DEBUG_DIR");
    if (dir != NULL && dir[0] != '\0') {
        strncpy(g_encDebugDir, dir, sizeof(g_encDebugDir) - 1);
        g_encDebugDir[sizeof(g_encDebugDir) - 1] = '\0';
    }

    if (mkdir(g_encDebugDir, 0775) != 0 && errno != EEXIST) {
        LOG("ENCDBG: mkdir(%s) failed: %s", g_encDebugDir, strerror(errno));
        g_encDebugEnabled = 0;
    } else {
        LOG("ENCDBG enabled dir=%s start_frame=%u max_frames=%u",
            g_encDebugDir, g_encDebugStartFrame, g_encDebugMaxFrames);
    }
    pthread_mutex_unlock(&g_encDebugMutex);
}

static void nvencDebugDumpNV12(const char *path, const uint8_t *y, const uint8_t *uv, size_t pitch, uint32_t width, uint32_t height)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        LOG("ENCDBG: failed to open %s: %s", path, strerror(errno));
        return;
    }
    for (uint32_t row = 0; row < height; row++) {
        fwrite(y + ((size_t) row * pitch), 1, width, f);
    }
    for (uint32_t row = 0; row < (height / 2U); row++) {
        fwrite(uv + ((size_t) row * pitch), 1, width, f);
    }
    fclose(f);
}

static void nvencDebugDumpBytes(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        LOG("ENCDBG: failed to open %s: %s", path, strerror(errno));
        return;
    }
    fwrite(data, 1, size, f);
    fclose(f);
}

static int nvencDebugBuildPath(char *out, size_t outSize, const char *leaf)
{
    size_t dirLen = strnlen(g_encDebugDir, outSize);
    size_t leafLen = strlen(leaf);
    if (dirLen == 0 || dirLen >= outSize) {
        return 0;
    }
    if (dirLen + 1 + leafLen + 1 > outSize) {
        return 0;
    }
    memcpy(out, g_encDebugDir, dirLen);
    out[dirLen] = '/';
    memcpy(out + dirLen + 1, leaf, leafLen + 1);
    return 1;
}

__attribute__ ((constructor))
static void init() {
    char *nvdLog = getenv("NVD_LOG");
    if (nvdLog != NULL) {
        if (strcmp(nvdLog, "1") == 0) {
            LOG_OUTPUT = stdout;
        } else {
            LOG_OUTPUT = fopen(nvdLog, "a");
            if (LOG_OUTPUT == NULL) {
                LOG_OUTPUT = stdout;
            }
        }
    }

    char *nvdGpu = getenv("NVD_GPU");
    if (nvdGpu != NULL) {
        gpu = atoi(nvdGpu);
    }

    char *nvdMaxInstances = getenv("NVD_MAX_INSTANCES");
    if (nvdMaxInstances != NULL) {
        max_instances = atoi(nvdMaxInstances);
    }

    char *nvdBackend = getenv("NVD_BACKEND");
    if (nvdBackend != NULL) {
        if (strncmp(nvdBackend, "direct", 6) == 0) {
            backend = DIRECT;
        } else if (strncmp(nvdBackend, "egl", 6) == 0) {
            backend = EGL;
        }
    }

    g_disableDecoder = envFlagEnabled("NVD_DISABLE_DECODER");
    if (g_disableDecoder) {
        LOG("NVD_DISABLE_DECODER enabled");
    }

    const char *nvdEncStrict = getenv("NVD_ENC_STRICT");
    if (nvdEncStrict != NULL && nvdEncStrict[0] != '\0' && strcmp(nvdEncStrict, "0") != 0) {
        g_encStrictMode = 1;
    }
    if (g_encStrictMode) {
        LOG("NVD_ENC_STRICT enabled");
    }

    g_encForceIdrEvery = parseEnvU32("NVD_ENC_FORCE_IDR_EVERY", 0);
    if (g_encForceIdrEvery > 0) {
        LOG("NVD_ENC_FORCE_IDR_EVERY=%u", g_encForceIdrEvery);
    }
    g_encStartupIdrFrames = parseEnvU32("NVD_ENC_STARTUP_IDR_FRAMES", 16);
    LOG("NVD_ENC_STARTUP_IDR_FRAMES=%u", g_encStartupIdrFrames);
    g_encReconfigureMinMs = parseEnvU32("NVD_ENC_RECONFIG_MIN_MS", 800);
    LOG("NVD_ENC_RECONFIG_MIN_MS=%u", g_encReconfigureMinMs);
    g_encVisibleReconfigure = envFlagEnabled("NVD_ENC_VISIBLE_RECONFIG");
    LOG("NVD_ENC_VISIBLE_RECONFIG=%d", g_encVisibleReconfigure);

#ifdef __linux__
    //try to detect the Firefox sandbox and skip loading CUDA if detected
    int fd = open("/proc/version", O_RDONLY);
    if (fd < 0) {
        LOG("ERROR: Potential Firefox sandbox detected, failing to init!");
        LOG("If running in Firefox, set env var MOZ_DISABLE_RDD_SANDBOX=1 to disable sandbox.");
        //exit here so we don't init CUDA, unless an env var has been set to force us to init even though we've detected a sandbox
        if (getenv("NVD_FORCE_INIT") == NULL) {
            return;
        }
    } else {
        //we're not in a sandbox
        //continue as normal
        close(fd);
    }
#endif

    //initialise the CUDA and NVDEC functions
    int ret = cuda_load_functions(&cu, NULL);
    if (ret != 0) {
        cu = NULL;
        LOG("Failed to load CUDA functions");
        return;
    }
    ret = cuvid_load_functions(&cv, NULL);
    if (ret != 0) {
        cv = NULL;
        LOG("Failed to load NVDEC functions");
        return;
    }

    //Not really much we can do here to abort the loading of the library
    CHECK_CUDA_RESULT(cu->cuInit(0));
}

__attribute__ ((destructor))
static void cleanup() {
    if (cv != NULL) {
        cuvid_free_functions(&cv);
    }
    if (cu != NULL) {
        cuda_free_functions(&cu);
    }
}


#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(gnu_printf) || (defined(__GNUC__) && !defined(__clang__))
__attribute((format(gnu_printf, 4, 5)))
#endif
void logger(const char *filename, const char *function, int line, const char *msg, ...) {
    if (LOG_OUTPUT == 0) {
        return;
    }

    va_list argList;
    char formattedMessage[1024];

    va_start(argList, msg);
    vsnprintf(formattedMessage, 1024, msg, argList);
    va_end(argList);

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    fprintf(LOG_OUTPUT, "%10ld.%09ld [%d-%d] %s:%4d %24s %s\n", (long)tp.tv_sec, tp.tv_nsec, getpid(), nv_gettid(), filename, line, function, formattedMessage);
    fflush(LOG_OUTPUT);
}

bool checkCudaErrors(CUresult err, const char *file, const char *function, const int line) {
    if (CUDA_SUCCESS != err) {
        const char *errStr = NULL;
        cu->cuGetErrorString(err, &errStr);
        logger(file, function, line, "CUDA ERROR '%s' (%d)\n", errStr, err);
        return true;
    }
    return false;
}

void appendBuffer(AppendableBuffer *ab, const void *buf, uint64_t size) {
  if (ab->buf == NULL) {
      ab->allocated = size*2;
      ab->buf = memalign(16, ab->allocated);
      ab->size = 0;
  } else if (ab->size + size > ab->allocated) {
      while (ab->size + size > ab->allocated) {
        ab->allocated += ab->allocated >> 1;
      }
      void *nb = memalign(16, ab->allocated);
      memcpy(nb, ab->buf, ab->size);
      free(ab->buf);
      ab->buf = nb;
  }
  memcpy(PTROFF(ab->buf, ab->size), buf, size);
  ab->size += size;
}

static void freeBuffer(AppendableBuffer *ab) {
  if (ab->buf != NULL) {
      free(ab->buf);
      ab->buf = NULL;
      ab->size = 0;
      ab->allocated = 0;
  }
}

static Object allocateObject(NVDriver *drv, ObjectType type, size_t allocatePtrSize) {
    Object newObj = (Object) calloc(1, sizeof(struct Object_t));

    newObj->type = type;

    if (allocatePtrSize > 0) {
        newObj->obj = calloc(1, allocatePtrSize);
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    newObj->id = (++drv->nextObjId);
    add_element(&drv->objects, newObj);
    pthread_mutex_unlock(&drv->objectCreationMutex);

    return newObj;
}

static Object getObject(NVDriver *drv, ObjectType type, VAGenericID id) {
    Object ret = NULL;
    if (id != VA_INVALID_ID) {
        pthread_mutex_lock(&drv->objectCreationMutex);
        ARRAY_FOR_EACH(Object, o, &drv->objects)
            if (o->id == id && o->type == type) {
                ret = o;
                break;
            }
        END_FOR_EACH
        pthread_mutex_unlock(&drv->objectCreationMutex);
    }
    return ret;
}

static void* getObjectPtr(NVDriver *drv, ObjectType type, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        Object o = getObject(drv, type, id);
        if (o != NULL) {
            return o->obj;
        }
    }
    return NULL;
}

static Object getObjectByPtr(NVDriver *drv, ObjectType type, void *ptr) {
    Object ret = NULL;
    if (ptr != NULL) {
        pthread_mutex_lock(&drv->objectCreationMutex);
        ARRAY_FOR_EACH(Object, o, &drv->objects)
            if (o->obj == ptr && o->type == type) {
                ret = o;
                break;
            }
        END_FOR_EACH
        pthread_mutex_unlock(&drv->objectCreationMutex);
    }
    return ret;
}

static void deleteObject(NVDriver *drv, VAGenericID id) {
    if (id == VA_INVALID_ID) {
        return;
    }

    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        if (o->id == id) {
            remove_element_at(&drv->objects, o_idx);
            free(o->obj);
            free(o);
            //we've found the object, no need to continue
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

static bool destroyContext(NVDriver *drv, NVContext *nvCtx) {
    if (nvCtx->mode == NV_CONTEXT_ENCODE) {
        if (nvCtx->encFrameNum > 0 && nvCtx->encOutputFrameCount == 0) {
            nvencHealthLog("WARN",
                           "HW encode context closed without output frames (%ux%u); client may fall back to software",
                           (unsigned int) nvCtx->width, (unsigned int) nvCtx->height);
        }
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);
        if (nvCtx->nvencEncoder != NULL) {
            nvencDestroyIoBuffers(drv, nvCtx);
            drv->nvencFuncs.nvEncDestroyEncoder(nvCtx->nvencEncoder);
            nvCtx->nvencEncoder = NULL;
        }
        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), false);
        if (nvCtx->encLastFrameData != NULL) {
            free(nvCtx->encLastFrameData);
            nvCtx->encLastFrameData = NULL;
            nvCtx->encLastFrameSize = 0;
            nvCtx->encLastFrameValid = 0;
        }
        return true;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), false);

    LOG("Signaling resolve thread to exit");
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;
    nvCtx->exiting = true;
    pthread_cond_signal(&nvCtx->resolveCondition);
    LOG("Waiting for resolve thread to exit");
    int ret = pthread_timedjoin_np(nvCtx->resolveThread, NULL, &timeout);
    LOG("Finished waiting for resolve thread with %d", ret);

    freeBuffer(&nvCtx->sliceOffsets);
    freeBuffer(&nvCtx->bitstreamBuffer);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), false);

    return true;
}

static void deleteAllObjects(NVDriver *drv) {
    pthread_mutex_lock(&drv->objectCreationMutex);
    ARRAY_FOR_EACH(Object, o, &drv->objects)
        LOG("Found object %d or type %d", o->id, o->type);
        if (o->type == OBJECT_TYPE_CONTEXT) {
            destroyContext(drv, (NVContext*) o->obj);
        } else if (o->type == OBJECT_TYPE_BUFFER) {
            NVBuffer *buf = (NVBuffer*) o->obj;
            if (buf->ptr != NULL && buf->ownsPtr) {
                free(buf->ptr);
            }
            if (buf->codedData != NULL) {
                free(buf->codedData);
            }
        } else if (o->type == OBJECT_TYPE_SURFACE) {
            NVSurface *surface = (NVSurface*) o->obj;
            if (surface->hostData != NULL) {
                free(surface->hostData);
                surface->hostData = NULL;
            }
        }
        deleteObject(drv, o->id);
    END_FOR_EACH
    pthread_mutex_unlock(&drv->objectCreationMutex);
}

NVSurface* nvSurfaceFromSurfaceId(NVDriver *drv, VASurfaceID surf) {
    Object obj = getObject(drv, OBJECT_TYPE_SURFACE, surf);
    if (obj != NULL) {
        NVSurface *suf = (NVSurface*) obj->obj;
        return suf;
    }
    return NULL;
}

int pictureIdxFromSurfaceId(NVDriver *drv, VASurfaceID surfId) {
    NVSurface *surf = nvSurfaceFromSurfaceId(drv, surfId);
    if (surf != NULL) {
        return surf->pictureIdx;
    }
    return -1;
}

static cudaVideoCodec vaToCuCodec(VAProfile profile) {
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        cudaVideoCodec cvc = c->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    return cudaVideoCodec_NONE;
}

static bool doesGPUSupportCodec(cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, uint32_t *width, uint32_t *height)
{
    CUVIDDECODECAPS videoDecodeCaps = {
        .eCodecType      = codec,
        .eChromaFormat   = chromaFormat,
        .nBitDepthMinus8 = bitDepth - 8
    };

    CHECK_CUDA_RESULT_RETURN(cv->cuvidGetDecoderCaps(&videoDecodeCaps), false);

    if (width != NULL) {
        *width = videoDecodeCaps.nMaxWidth;
    }
    if (height != NULL) {
        *height = videoDecodeCaps.nMaxHeight;
    }
    return (videoDecodeCaps.bIsSupported == 1);
}

static const char *nvencStatusStr(NVENCSTATUS status)
{
    switch (status) {
        case NV_ENC_SUCCESS: return "SUCCESS";
        case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NO_ENCODE_DEVICE";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "UNSUPPORTED_DEVICE";
        case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "INVALID_ENCODERDEVICE";
        case NV_ENC_ERR_INVALID_DEVICE: return "INVALID_DEVICE";
        case NV_ENC_ERR_DEVICE_NOT_EXIST: return "DEVICE_NOT_EXIST";
        case NV_ENC_ERR_INVALID_PTR: return "INVALID_PTR";
        case NV_ENC_ERR_INVALID_EVENT: return "INVALID_EVENT";
        case NV_ENC_ERR_INVALID_PARAM: return "INVALID_PARAM";
        case NV_ENC_ERR_INVALID_CALL: return "INVALID_CALL";
        case NV_ENC_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "ENCODER_NOT_INITIALIZED";
        case NV_ENC_ERR_UNSUPPORTED_PARAM: return "UNSUPPORTED_PARAM";
        case NV_ENC_ERR_LOCK_BUSY: return "LOCK_BUSY";
        case NV_ENC_ERR_NOT_ENOUGH_BUFFER: return "NOT_ENOUGH_BUFFER";
        case NV_ENC_ERR_INVALID_VERSION: return "INVALID_VERSION";
        case NV_ENC_ERR_MAP_FAILED: return "MAP_FAILED";
        case NV_ENC_ERR_NEED_MORE_INPUT: return "NEED_MORE_INPUT";
        case NV_ENC_ERR_ENCODER_BUSY: return "ENCODER_BUSY";
        case NV_ENC_ERR_GENERIC: return "GENERIC";
        default: return "UNKNOWN";
    }
}

static VAStatus nvencToVaStatus(NVENCSTATUS status)
{
    switch (status) {
        case NV_ENC_SUCCESS:
            return VA_STATUS_SUCCESS;
        case NV_ENC_ERR_OUT_OF_MEMORY:
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        case NV_ENC_ERR_INVALID_PARAM:
        case NV_ENC_ERR_INVALID_PTR:
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        case NV_ENC_ERR_NO_ENCODE_DEVICE:
        case NV_ENC_ERR_UNSUPPORTED_DEVICE:
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        default:
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }
}

static bool isEncodeProfile(VAProfile profile)
{
    switch (profile) {
        case VAProfileH264ConstrainedBaseline:
        case VAProfileH264Main:
        case VAProfileH264High:
        case VAProfileHEVCMain:
        case VAProfileHEVCMain10:
            return true;
        default:
            return false;
    }
}

static void appendProfileIfMissing(NVDriver *drv, VAProfile profile)
{
    for (int i = 0; i < drv->profileCount; i++) {
        if (drv->profiles[i] == profile) {
            return;
        }
    }

    if (drv->profileCount >= MAX_PROFILES) {
        LOG("Profile list full, cannot append profile %d", profile);
        return;
    }

    drv->profiles[drv->profileCount++] = profile;
}

static bool profileToNvencGuids(VAProfile profile, GUID *codecGuid, GUID *profileGuid, int *isHevc)
{
    switch (profile) {
        case VAProfileH264ConstrainedBaseline:
            *codecGuid = NV_ENC_CODEC_H264_GUID;
            *profileGuid = NV_ENC_H264_PROFILE_BASELINE_GUID;
            *isHevc = 0;
            return true;
        case VAProfileH264Main:
            *codecGuid = NV_ENC_CODEC_H264_GUID;
            *profileGuid = NV_ENC_H264_PROFILE_MAIN_GUID;
            *isHevc = 0;
            return true;
        case VAProfileH264High:
            *codecGuid = NV_ENC_CODEC_H264_GUID;
            *profileGuid = NV_ENC_H264_PROFILE_HIGH_GUID;
            *isHevc = 0;
            return true;
        case VAProfileHEVCMain:
            *codecGuid = NV_ENC_CODEC_HEVC_GUID;
            *profileGuid = NV_ENC_HEVC_PROFILE_MAIN_GUID;
            *isHevc = 1;
            return true;
        case VAProfileHEVCMain10:
            *codecGuid = NV_ENC_CODEC_HEVC_GUID;
            *profileGuid = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
            *isHevc = 1;
            return true;
        default:
            return false;
    }
}

static bool loadNvencFunctions(NVDriver *drv)
{
    typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST *);

    drv->nvencLib = dlopen("libnvidia-encode.so.1", RTLD_NOW);
    if (!drv->nvencLib) {
        LOG("Failed to load libnvidia-encode.so.1: %s", dlerror());
        return false;
    }

    PFN_NvEncodeAPICreateInstance createInstance =
        (PFN_NvEncodeAPICreateInstance) dlsym(drv->nvencLib, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        LOG("Failed to find NvEncodeAPICreateInstance: %s", dlerror());
        dlclose(drv->nvencLib);
        drv->nvencLib = NULL;
        return false;
    }

    memset(&drv->nvencFuncs, 0, sizeof(drv->nvencFuncs));
    drv->nvencFuncs.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = createInstance(&drv->nvencFuncs);
    if (status != NV_ENC_SUCCESS) {
        LOG("NvEncodeAPICreateInstance failed: %s", nvencStatusStr(status));
        dlclose(drv->nvencLib);
        drv->nvencLib = NULL;
        return false;
    }

    drv->nvencAvailable = true;
    LOG("NVENC API loaded");
    return true;
}

static bool ensureSurfaceHostBuffer(NVSurface *surface)
{
    if (!surface) {
        return false;
    }
    if (surface->hostData) {
        return true;
    }

    uint32_t aw = ROUND_UP(surface->width, NVENC_PITCH_ALIGN);
    uint32_t ah = ROUND_UP(surface->height, NVENC_HEIGHT_ALIGN);
    size_t pitch = aw;
    size_t ySize = pitch * ah;
    size_t total = ySize + pitch * (ah / 2);
    uint8_t *data = calloc(1, total);
    if (!data) {
        return false;
    }

    surface->hostData = data;
    surface->hostPitch = pitch;
    surface->hostYSize = ySize;
    surface->hostTotalSize = total;
    surface->status = VASurfaceReady;
    return true;
}

static void nvencDestroyIoBuffers(NVDriver *drv, NVContext *nvCtx)
{
    if (drv == NULL || nvCtx == NULL || nvCtx->nvencEncoder == NULL) {
        return;
    }

    for (uint32_t i = 0; i < NVENC_MAX_IO_BUFFERS; i++) {
        if (nvCtx->nvencInputBuffers[i] != NULL) {
            drv->nvencFuncs.nvEncDestroyInputBuffer(nvCtx->nvencEncoder, nvCtx->nvencInputBuffers[i]);
            nvCtx->nvencInputBuffers[i] = NULL;
        }
        if (nvCtx->nvencOutputBuffers[i] != NULL) {
            drv->nvencFuncs.nvEncDestroyBitstreamBuffer(nvCtx->nvencEncoder, nvCtx->nvencOutputBuffers[i]);
            nvCtx->nvencOutputBuffers[i] = NULL;
        }
    }
    nvCtx->nvencIoBufferCount = 0;
    nvCtx->nvencIoBufferIndex = 0;
}

static int setU32IfChanged(uint32_t *dst, uint32_t value)
{
    if (*dst == value) {
        return 0;
    }
    *dst = value;
    return 1;
}

static void nvencSetBitrate(NVContext *nvCtx, uint32_t bitsPerSecond)
{
    if (bitsPerSecond == 0) {
        return;
    }
    NV_ENC_RC_PARAMS *rc = &nvCtx->encConfig.rcParams;
    if (rc->rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        return;
    }
    uint32_t avg = bitsPerSecond;
    uint32_t max = bitsPerSecond;
    if (rc->rateControlMode == NV_ENC_PARAMS_RC_CBR) {
        max = bitsPerSecond;
    } else {
        max = rc->maxBitRate < bitsPerSecond ? bitsPerSecond : rc->maxBitRate;
    }
    int changed = 0;
    changed |= setU32IfChanged(&rc->averageBitRate, avg);
    changed |= setU32IfChanged(&rc->maxBitRate, max);
    if (changed) {
        nvCtx->encNeedsReconfigure = 1;
    }
}

static void nvencSetFrameRate(NVContext *nvCtx, uint32_t fpsNum, uint32_t fpsDen)
{
    if (fpsNum == 0) {
        return;
    }
    if (fpsDen == 0) {
        fpsDen = 1;
    }
    int changed = 0;
    changed |= setU32IfChanged(&nvCtx->encInitParams.frameRateNum, fpsNum);
    changed |= setU32IfChanged(&nvCtx->encInitParams.frameRateDen, fpsDen);
    if (changed) {
        nvCtx->encNeedsReconfigure = 1;
    }
}

static void nvencSetGop(NVContext *nvCtx, uint32_t gopLength, uint32_t idrPeriod)
{
    if (gopLength > 0) {
        if (setU32IfChanged(&nvCtx->encConfig.gopLength, gopLength)) {
            nvCtx->encNeedsReconfigure = 1;
        }
    }
    if (!nvCtx->encIsHevc) {
        if (idrPeriod > 0) {
            if (setU32IfChanged(&nvCtx->encConfig.encodeCodecConfig.h264Config.idrPeriod, idrPeriod)) {
                nvCtx->encNeedsReconfigure = 1;
            }
        }
    } else {
        if (idrPeriod > 0) {
            if (setU32IfChanged(&nvCtx->encConfig.encodeCodecConfig.hevcConfig.idrPeriod, idrPeriod)) {
                nvCtx->encNeedsReconfigure = 1;
            }
        }
    }
}

static void nvencSetVisibleRect(NVContext *nvCtx, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) {
        nvCtx->encVisibleValid = 0;
        return;
    }
    x &= ~1U;
    y &= ~1U;
    w &= ~1U;
    h &= ~1U;
    if (w == 0 || h == 0) {
        nvCtx->encVisibleValid = 0;
        return;
    }
    if (x >= (uint32_t) nvCtx->width || y >= (uint32_t) nvCtx->height) {
        nvCtx->encVisibleValid = 0;
        return;
    }
    if (x + w > (uint32_t) nvCtx->width) {
        w = (uint32_t) nvCtx->width - x;
    }
    if (y + h > (uint32_t) nvCtx->height) {
        h = (uint32_t) nvCtx->height - y;
    }
    w &= ~1U;
    h &= ~1U;
    if (w == 0 || h == 0) {
        nvCtx->encVisibleValid = 0;
        return;
    }
    nvCtx->encVisibleX = x;
    nvCtx->encVisibleY = y;
    nvCtx->encVisibleWidth = w;
    nvCtx->encVisibleHeight = h;
    nvCtx->encVisibleValid = 1;
}

static void nvencMaybeUseVisibleResolution(NVContext *nvCtx)
{
    if (!g_encVisibleReconfigure) {
        return;
    }
    if (!nvCtx->encVisibleValid) {
        return;
    }
    if (nvCtx->encVisibleWidth == 0 || nvCtx->encVisibleHeight == 0) {
        return;
    }
    if (nvCtx->encVisibleX != 0 || nvCtx->encVisibleY != 0) {
        return;
    }

    uint32_t visW = nvCtx->encVisibleWidth;
    uint32_t visH = nvCtx->encVisibleHeight;
    if (visW == nvCtx->width && visH == nvCtx->height) {
        return;
    }
    if (visW > nvCtx->encInitParams.maxEncodeWidth || visH > nvCtx->encInitParams.maxEncodeHeight) {
        return;
    }

    nvCtx->encInitParams.encodeWidth = visW;
    nvCtx->encInitParams.encodeHeight = visH;
    nvCtx->encInitParams.darWidth = visW;
    nvCtx->encInitParams.darHeight = visH;
    nvCtx->encNeedsReconfigure = 1;
    LOG("NVENC visible reconfigure requested: %dx%d -> %ux%u",
        nvCtx->width, nvCtx->height, visW, visH);
}

static void nvencUpdateVisibleRectFromH264Seq(NVContext *nvCtx, const VAEncSequenceParameterBufferH264 *seq)
{
    if (!seq) {
        return;
    }

    uint32_t codedW = (uint32_t) seq->picture_width_in_mbs * 16U;
    uint32_t codedH = (uint32_t) seq->picture_height_in_mbs * 16U;

    uint32_t chroma = seq->seq_fields.bits.chroma_format_idc;
    uint32_t frameMbsOnly = seq->seq_fields.bits.frame_mbs_only_flag ? 1U : 0U;
    uint32_t cropUnitX = 1U;
    uint32_t cropUnitY = frameMbsOnly ? 1U : 2U;

    switch (chroma) {
        case 0: /* monochrome */
            cropUnitX = 1U;
            cropUnitY = frameMbsOnly ? 1U : 2U;
            break;
        case 1: /* 4:2:0 */
            cropUnitX = 2U;
            cropUnitY = frameMbsOnly ? 2U : 4U;
            break;
        case 2: /* 4:2:2 */
            cropUnitX = 2U;
            cropUnitY = frameMbsOnly ? 1U : 2U;
            break;
        case 3: /* 4:4:4 */
        default:
            cropUnitX = 1U;
            cropUnitY = frameMbsOnly ? 1U : 2U;
            break;
    }

    uint32_t cropL = 0, cropR = 0, cropT = 0, cropB = 0;
    if (seq->frame_cropping_flag) {
        cropL = seq->frame_crop_left_offset * cropUnitX;
        cropR = seq->frame_crop_right_offset * cropUnitX;
        cropT = seq->frame_crop_top_offset * cropUnitY;
        cropB = seq->frame_crop_bottom_offset * cropUnitY;
    }

    uint32_t visX = cropL;
    uint32_t visY = cropT;
    uint32_t visW = codedW;
    uint32_t visH = codedH;
    if (visW > cropL + cropR) {
        visW -= (cropL + cropR);
    }
    if (visH > cropT + cropB) {
        visH -= (cropT + cropB);
    }

    nvencSetVisibleRect(nvCtx, visX, visY, visW, visH);
    if (nvCtx->encVisibleValid) {
        nvencMaybeUseVisibleResolution(nvCtx);
    }
}

static void nvencUpdateVisibleRectFromHevcSeq(NVContext *nvCtx, const VAEncSequenceParameterBufferHEVC *seq)
{
    if (!seq) {
        return;
    }
    if (seq->pic_width_in_luma_samples == 0 || seq->pic_height_in_luma_samples == 0) {
        return;
    }
    nvencSetVisibleRect(nvCtx, 0, 0, seq->pic_width_in_luma_samples, seq->pic_height_in_luma_samples);
    if (nvCtx->encVisibleValid) {
        nvencMaybeUseVisibleResolution(nvCtx);
    }
}

static VAStatus nvencReconfigureIfNeeded(NVDriver *drv, NVContext *nvCtx)
{
    if (!nvCtx->encNeedsReconfigure) {
        return VA_STATUS_SUCCESS;
    }
    if (!drv->nvencFuncs.nvEncReconfigureEncoder) {
        nvCtx->encNeedsReconfigure = 0;
        return VA_STATUS_SUCCESS;
    }

    uint32_t targetWidth = nvCtx->encInitParams.encodeWidth;
    uint32_t targetHeight = nvCtx->encInitParams.encodeHeight;
    uint32_t targetDarWidth = nvCtx->encInitParams.darWidth;
    uint32_t targetDarHeight = nvCtx->encInitParams.darHeight;
    bool resolutionChange = (targetWidth != (uint32_t) nvCtx->width) ||
                            (targetHeight != (uint32_t) nvCtx->height) ||
                            (targetDarWidth != (uint32_t) nvCtx->width) ||
                            (targetDarHeight != (uint32_t) nvCtx->height);

    /*
     * WebRTC may push misc RC/FPS updates very frequently. Reconfiguring every
     * frame can stall encoding. Keep resolution changes immediate, but debounce
     * non-resolution reconfigures.
     */
    if (!resolutionChange && g_encReconfigureMinMs > 0) {
        uint64_t now = getMonotonicNs();
        uint64_t minIntervalNs = (uint64_t) g_encReconfigureMinMs * 1000000ULL;
        if (nvCtx->encLastReconfigureNs != 0 &&
            now > nvCtx->encLastReconfigureNs &&
            (now - nvCtx->encLastReconfigureNs) < minIntervalNs) {
            return VA_STATUS_SUCCESS;
        }
    }

    NV_ENC_RECONFIGURE_PARAMS reconfig;
    memset(&reconfig, 0, sizeof(reconfig));
    reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    reconfig.reInitEncodeParams = nvCtx->encInitParams;
    reconfig.reInitEncodeParams.encodeConfig = &nvCtx->encConfig;

    uint32_t oldWidth = (uint32_t) nvCtx->width;
    uint32_t oldHeight = (uint32_t) nvCtx->height;
    NVENCSTATUS nvs = drv->nvencFuncs.nvEncReconfigureEncoder(nvCtx->nvencEncoder, &reconfig);
    if (nvs != NV_ENC_SUCCESS) {
        LOG("nvEncReconfigureEncoder failed: %s", nvencStatusStr(nvs));
        /*
         * Avoid failing every frame on repeating app-side control buffers.
         * Keep previous active encoder configuration and continue.
         */
        nvCtx->encNeedsReconfigure = 0;
        return VA_STATUS_SUCCESS;
    }
    nvCtx->encInitParams = reconfig.reInitEncodeParams;
    nvCtx->encInitParams.encodeConfig = &nvCtx->encConfig;
    nvCtx->width = (int) nvCtx->encInitParams.encodeWidth;
    nvCtx->height = (int) nvCtx->encInitParams.encodeHeight;
    nvCtx->encLastReconfigureNs = getMonotonicNs();
    if (oldWidth != (uint32_t) nvCtx->width || oldHeight != (uint32_t) nvCtx->height) {
        nvCtx->encStartupIdrLeft = g_encStartupIdrFrames;
    }
    if (oldWidth != (uint32_t) nvCtx->width || oldHeight != (uint32_t) nvCtx->height) {
        LOG("NVENC reconfigured: %ux%u -> %dx%d", oldWidth, oldHeight, nvCtx->width, nvCtx->height);
    }
    nvCtx->encNeedsReconfigure = 0;
    return VA_STATUS_SUCCESS;
}

static void* resolveSurfaces(void *param) {
    NVContext *ctx = (NVContext*) param;
    NVDriver *drv = ctx->drv;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), NULL);

    LOG("[RT] Resolve thread for %p started", ctx);
    while (!ctx->exiting) {
        //wait for frame on queue
        pthread_mutex_lock(&ctx->resolveMutex);
        while (ctx->surfaceQueueReadIdx == ctx->surfaceQueueWriteIdx) {
            pthread_cond_wait(&ctx->resolveCondition, &ctx->resolveMutex);
            if (ctx->exiting) {
                pthread_mutex_unlock(&ctx->resolveMutex);
                goto out;
            }
        }
        pthread_mutex_unlock(&ctx->resolveMutex);
        //find the last item
        //LOG("Reading from queue: %d %d", ctx->surfaceQueueReadIdx, ctx->surfaceQueueWriteIdx);
        NVSurface *surface = ctx->surfaceQueue[ctx->surfaceQueueReadIdx++];
        if (ctx->surfaceQueueReadIdx >= SURFACE_QUEUE_SIZE) {
            ctx->surfaceQueueReadIdx = 0;
        }

        CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
        unsigned int pitch = 0;

        //map frame
        CUVIDPROCPARAMS procParams = {
            .progressive_frame = surface->progressiveFrame,
            .top_field_first = surface->topFieldFirst,
            .second_field = surface->secondField
        };

        //LOG("Mapping surface %d", surface->pictureIdx);
        if (surface->decodeFailed || CHECK_CUDA_RESULT(cv->cuvidMapVideoFrame(ctx->decoder, surface->pictureIdx, &deviceMemory, &pitch, &procParams))) {
            pthread_mutex_lock(&surface->mutex);
            surface->decodeFailed = true;
            surface->status = VASurfaceReady;
            surface->resolving = 0;
            pthread_cond_signal(&surface->cond);
            pthread_mutex_unlock(&surface->mutex);
            continue;
        }
        //LOG("Mapped surface %d to %p (%d)", surface->pictureIdx, (void*)deviceMemory, pitch);

        //update cuarray
        bool exported = drv->backend->exportCudaPtr(drv, deviceMemory, surface, pitch);
        //LOG("Surface %d exported", surface->pictureIdx);
        //unmap frame

        CHECK_CUDA_RESULT(cv->cuvidUnmapVideoFrame(ctx->decoder, deviceMemory));

        if (!exported) {
            pthread_mutex_lock(&surface->mutex);
            surface->decodeFailed = true;
            surface->status = VASurfaceReady;
            surface->resolving = 0;
            pthread_cond_signal(&surface->cond);
            pthread_mutex_unlock(&surface->mutex);
        }
    }
out:
    //release the decoder here to prevent multiple threads attempting it
    if (ctx->decoder != NULL) {
        CUresult result = cv->cuvidDestroyDecoder(ctx->decoder);
        ctx->decoder = NULL;
        if (result != CUDA_SUCCESS) {
            LOG("cuvidDestroyDecoder failed: %d", result);
        }
    }
    LOG("[RT] Resolve thread for %p exiting", ctx);
    return NULL;
}


static VAStatus nvQueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //now filter out the codecs we don't support
    for (int i = 0; i < drv->profileCount; i++) {
        profile_list[i] = drv->profiles[i];
    }

    *num_profiles = drv->profileCount;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigProfiles2(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    int profiles = 0;
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile0;
    }

    if (drv->supports16BitSurface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain10;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain12;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
        }
    }

    if (drv->supports444Surface) {
        if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileHEVCMain444;
        }
        if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
        }
        if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
            profile_list[profiles++] = VAProfileAV1Profile1;
        }

#if VA_CHECK_VERSION(1, 20, 0)
        if (drv->supports16BitSurface) {
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_10;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileHEVCMain444_12;
            }
            if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
                profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, 4:2:2, 4:4:0, 4:4:4
            }
        }
#endif
    }

    // Nvidia decoder doesn't support 422 chroma layout
#if 0
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_422, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_12;
    }
#endif

    //now filter out the codecs we don't support
    for (int i = 0; i < profiles; i++) {
        if (vaToCuCodec(profile_list[i]) == cudaVideoCodec_NONE) {
            for (int x = i; x < profiles-1; x++) {
                profile_list[x] = profile_list[x+1];
            }
            profiles--;
            i--;
        }
    }

    *num_profiles = profiles;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    int n = 0;
    if (!g_disableDecoder && vaToCuCodec(profile) != cudaVideoCodec_NONE) {
        entrypoint_list[n++] = VAEntrypointVLD;
    }
    if (drv->nvencAvailable && isEncodeProfile(profile)) {
        entrypoint_list[n++] = VAEntrypointEncSlice;
        entrypoint_list[n++] = VAEntrypointEncSliceLP;
    }
    *num_entrypoints = n;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,	/* in/out */
        int num_attribs
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    if (g_disableDecoder && entrypoint == VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if ((entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncSliceLP) && drv->nvencAvailable) {
        if (!isEncodeProfile(profile)) {
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }
        for (int i = 0; i < num_attribs; i++) {
            switch (attrib_list[i].type) {
                case VAConfigAttribRTFormat:
                    attrib_list[i].value = VA_RT_FORMAT_YUV420;
                    break;
                case VAConfigAttribRateControl:
                    attrib_list[i].value = VA_RC_CBR | VA_RC_VBR | VA_RC_CQP;
                    break;
                case VAConfigAttribEncMaxRefFrames:
                    attrib_list[i].value = 4;
                    break;
                case VAConfigAttribMaxPictureWidth:
                case VAConfigAttribMaxPictureHeight:
                    attrib_list[i].value = 8192;
                    break;
                case VAConfigAttribEncPackedHeaders:
                    attrib_list[i].value = VA_ENC_PACKED_HEADER_SEQUENCE |
                                           VA_ENC_PACKED_HEADER_PICTURE |
                                           VA_ENC_PACKED_HEADER_SLICE;
                    break;
                default:
                    attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                    break;
            }
        }
        return VA_STATUS_SUCCESS;
    }

    if (vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    //LOG("Got here with profile: %d == %d", profile, vaToCuCodec(profile));

    for (int i = 0; i < num_attribs; i++)
    {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
        {
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            switch (profile) {
            case VAProfileHEVCMain12:
            case VAProfileVP9Profile2:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain10:
            case VAProfileAV1Profile0:
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
                break;

            case VAProfileHEVCMain444_12:
            case VAProfileVP9Profile3:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
                // Fall-through
            case VAProfileHEVCMain444_10:
            case VAProfileAV1Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
                // Fall-through
            case VAProfileHEVCMain444:
            case VAProfileVP9Profile1:
                attrib_list[i].value |= VA_RT_FORMAT_YUV444;
                break;
            default:
                //do nothing
                break;
            }

            if (!drv->supports16BitSurface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
            if (!drv->supports444Surface) {
                attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
            }
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureWidth)
        {
            uint32_t width = 0;
            if (!doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &width, NULL) || width == 0) {
                width = 8192;
            }
            attrib_list[i].value = width;
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            uint32_t height = 0;
            if (!doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &height) || height == 0) {
                height = 8192;
            }
            attrib_list[i].value = height;
        }
        else
        {
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("got profile: %d with %d attributes", profile, num_attribs);

    if (entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncSliceLP) {
        if (!drv->nvencAvailable) {
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }
        if (!isEncodeProfile(profile)) {
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
        NVConfig *cfg = (NVConfig*) obj->obj;
        cfg->profile = profile;
        cfg->entrypoint = entrypoint;
        cfg->isEncode = true;
        cfg->encodeRCMode = VA_RC_CBR;
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = (profile == VAProfileHEVCMain10) ? 10 : 8;
        cfg->cudaCodec = (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10)
                             ? cudaVideoCodec_HEVC
                             : cudaVideoCodec_H264;

        for (int i = 0; i < num_attribs; i++) {
            if (attrib_list[i].type == VAConfigAttribRateControl) {
                cfg->encodeRCMode = attrib_list[i].value;
            }
        }

        *config_id = obj->id;
        return VA_STATUS_SUCCESS;
    }

    cudaVideoCodec cudaCodec = vaToCuCodec(profile);

    if (cudaCodec == cudaVideoCodec_NONE) {
        //we don't support this yet
        LOG("Profile not supported: %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (entrypoint != VAEntrypointVLD) {
        LOG("Entrypoint not supported: %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (g_disableDecoder) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;
    cfg->isEncode = false;

    //this will contain all the attributes the client cares about
    //for (int i = 0; i < num_attribs; i++) {
    //  LOG("got config attrib: %d %d %d", i, attrib_list[i].type, attrib_list[i].value);
    //}

    cfg->cudaCodec = cudaCodec;
    cfg->chromaFormat = cudaVideoChromaFormat_420;
    cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
    cfg->bitDepth = 8;

    if (drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile2:
        case VAProfileAV1Profile0:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (num_attribs > 0 && attrib_list[0].type == VAConfigAttribRTFormat) {
                switch(attrib_list[0].value) {
                case VA_RT_FORMAT_YUV420_12:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 12;
                    break;
                case VA_RT_FORMAT_YUV420_10:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                    break;
                default:
                    break;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile2) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
                    cfg->bitDepth = 10;
                } else {
                    if (!g_warnedVp9Av1NoRtformat) {
                        LOG("Unable to determine surface type for VP9/AV1 codec due to no RTFormat specified.");
                        g_warnedVp9Av1NoRtformat = 1;
                    }
                }
            }
        default:
            break;
        }
    }

    if (drv->supports444Surface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444:
        case VAProfileVP9Profile1:
        case VAProfileAV1Profile1:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 8;
            break;
        default:
            break;
        }
    }

    if (drv->supports444Surface && drv->supports16BitSurface) {
        switch(cfg->profile) {
        case VAProfileHEVCMain444_10:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 10;
            break;
        case VAProfileHEVCMain444_12:
            cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
            cfg->chromaFormat = cudaVideoChromaFormat_444;
            cfg->bitDepth = 12;
            break;
        case VAProfileVP9Profile3:
        case VAProfileAV1Profile1:
            // If the user provides an RTFormat, we can use that to identify which decoder
            // configuration is appropriate. If a format is not required here, the caller
            // must pass render targets to createContext so we can use those to establish
            // the surface format and bit depth.
            if (num_attribs > 0 && attrib_list[0].type == VAConfigAttribRTFormat) {
                switch(attrib_list[0].value) {
                case VA_RT_FORMAT_YUV444_12:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 12;
                    break;
                case VA_RT_FORMAT_YUV444_10:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                    break;
                case VA_RT_FORMAT_YUV444:
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 8;
                    break;
                default:
                    break;
                }
            } else {
                if (cfg->profile == VAProfileVP9Profile3) {
                    cfg->surfaceFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
                    cfg->chromaFormat = cudaVideoChromaFormat_444;
                    cfg->bitDepth = 10;
                }
            }
        default:
            break;
        }
    }

    *config_id = obj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    deleteObject(drv, config_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,		/* out */
        VAEntrypoint *entrypoint, 	/* out */
        VAConfigAttrib *attrib_list,	/* out */
        int *num_attribs		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    if (num_attribs == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (profile) {
        *profile = cfg->profile;
    }
    if (entrypoint) {
        *entrypoint = cfg->entrypoint;
    }

    if (cfg->isEncode) {
        const int availableAttribs = 2;
        if (!attrib_list) {
            *num_attribs = availableAttribs;
            return VA_STATUS_SUCCESS;
        }
        attrib_list[0].type = VAConfigAttribRTFormat;
        attrib_list[0].value = VA_RT_FORMAT_YUV420;
        attrib_list[1].type = VAConfigAttribRateControl;
        attrib_list[1].value = cfg->encodeRCMode;
        *num_attribs = availableAttribs;
        return VA_STATUS_SUCCESS;
    }

    const int availableAttribs = 1;
    if (!attrib_list) {
        *num_attribs = availableAttribs;
        return VA_STATUS_SUCCESS;
    }

    int i = 0;
    attrib_list[i].value = VA_RT_FORMAT_YUV420;
    attrib_list[i].type = VAConfigAttribRTFormat;
    switch (cfg->profile) {
    case VAProfileHEVCMain12:
    case VAProfileVP9Profile2:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain10:
    case VAProfileAV1Profile0:
        attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
        break;

    case VAProfileHEVCMain444_12:
    case VAProfileVP9Profile3:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_12 | VA_RT_FORMAT_YUV420_12;
        // Fall-through
    case VAProfileHEVCMain444_10:
    case VAProfileAV1Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV420_10;
        // Fall-through
    case VAProfileHEVCMain444:
    case VAProfileVP9Profile1:
        attrib_list[i].value |= VA_RT_FORMAT_YUV444;
        break;
    default:
        //do nothing
        break;
    }

    if (!drv->supports16BitSurface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_YUV420_12 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }
    if (!drv->supports444Surface) {
        attrib_list[i].value &= ~(VA_RT_FORMAT_YUV444 | VA_RT_FORMAT_YUV444_10 | VA_RT_FORMAT_YUV444_12);
    }

    i++;
    *num_attribs = i;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSurfaces2(
            VADriverContextP    ctx,
            unsigned int        format,
            unsigned int        width,
            unsigned int        height,
            VASurfaceID        *surfaces,
            unsigned int        num_surfaces,
            VASurfaceAttrib    *attrib_list,
            unsigned int        num_attribs
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    cudaVideoSurfaceFormat nvFormat;
    cudaVideoChromaFormat chromaFormat;
    int bitdepth;

    switch (format)
    {
    case VA_RT_FORMAT_YUV420:
        nvFormat = cudaVideoSurfaceFormat_NV12;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV420_10:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV420_12:
        nvFormat = cudaVideoSurfaceFormat_P016;
        chromaFormat = cudaVideoChromaFormat_420;
        bitdepth = 12;
        break;
    case VA_RT_FORMAT_YUV444:
        nvFormat = cudaVideoSurfaceFormat_YUV444;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 8;
        break;
    case VA_RT_FORMAT_YUV444_10:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 10;
        break;
    case VA_RT_FORMAT_YUV444_12:
        nvFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        chromaFormat = cudaVideoChromaFormat_444;
        bitdepth = 12;
        break;
    
    default:
        LOG("Unknown format: %X", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    // If there is subsampled chroma make the size a multple of it
    switch(chromaFormat) {
        case cudaVideoChromaFormat_422:
            width = ROUND_UP(width, 2);
            break;
        case cudaVideoChromaFormat_420:
            width = ROUND_UP(width, 2);
            height = ROUND_UP(height, 2);
            break;
        default:
            // no change needed
            break;
    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    LOG("Creating %u surface(s): %ux%u format=%X", num_surfaces, width, height, format);
    for (uint32_t i = 0; i < num_surfaces; i++) {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->pictureIdx = -1;
        suf->bitDepth = bitdepth;
        suf->context = NULL;
        suf->chromaFormat = chromaFormat;
        pthread_mutex_init(&suf->mutex, NULL);
        pthread_cond_init(&suf->cond, NULL);
        if (!ensureSurfaceHostBuffer(suf)) {
            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        suf->contentX = 0;
        suf->contentY = 0;
        suf->contentWidth = width;
        suf->contentHeight = height;
        suf->contentValid = 0;
        suf->status = VASurfaceReady;

    }

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces		/* out */
    )
{
    return nvCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}


static VAStatus nvDestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    LOG("Destroying %d surface(s)", num_surfaces);
    for (int i = 0; i < num_surfaces; i++) {
        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_list[i]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        drv->backend->detachBackingImageFromSurface(drv, surface);
        free(surface->hostData);
        surface->hostData = NULL;

        deleteObject(drv, surface_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config_id);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->isEncode) {
        if (!drv->nvencAvailable) {
            return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
        }
        if (picture_width <= 0 || picture_height <= 0) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
        NVContext *nvCtx = (NVContext*) contextObj->obj;
        nvCtx->drv = drv;
        nvCtx->mode = NV_CONTEXT_ENCODE;
        nvCtx->profile = cfg->profile;
        nvCtx->entrypoint = cfg->entrypoint;
        nvCtx->width = picture_width;
        nvCtx->height = picture_height;
        nvCtx->encPresetGuid = NV_ENC_PRESET_P4_GUID;
        nvCtx->encFrameNum = 0;
        nvCtx->encCurrentSurface = VA_INVALID_SURFACE;
        nvCtx->encCodedBufId = VA_INVALID_ID;
        nvCtx->encVisibleX = 0;
        nvCtx->encVisibleY = 0;
        nvCtx->encVisibleWidth = picture_width;
        nvCtx->encVisibleHeight = picture_height;
        nvCtx->encVisibleValid = 0;
        nvCtx->encLastFrameData = NULL;
        nvCtx->encLastFrameSize = 0;
        nvCtx->encLastFrameValid = 0;
        nvCtx->encStartupIdrLeft = g_encStartupIdrFrames;
        nvCtx->encNeedMoreInputStreak = 0;
        nvCtx->encOutputFrameCount = 0;
        nvCtx->encLastHealthLogFrame = 0;
        nvCtx->nvencIoBufferCount = parseEnvU32("NVD_ENC_IO_DEPTH", 1);
        if (nvCtx->nvencIoBufferCount == 0) {
            nvCtx->nvencIoBufferCount = 1;
        } else if (nvCtx->nvencIoBufferCount > NVENC_MAX_IO_BUFFERS) {
            nvCtx->nvencIoBufferCount = NVENC_MAX_IO_BUFFERS;
        }
        nvCtx->nvencIoBufferIndex = 0;

        if (!profileToNvencGuids(cfg->profile, &nvCtx->encCodecGuid, &nvCtx->encProfileGuid, &nvCtx->encIsHevc)) {
            deleteObject(drv, contextObj->id);
            return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        }

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams;
        memset(&openParams, 0, sizeof(openParams));
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        openParams.device = drv->cudaContext;
        openParams.apiVersion = NVENCAPI_VERSION;

        NVENCSTATUS nvs = drv->nvencFuncs.nvEncOpenEncodeSessionEx(&openParams, &nvCtx->nvencEncoder);
        if (nvs != NV_ENC_SUCCESS) {
            cu->cuCtxPopCurrent(NULL);
            deleteObject(drv, contextObj->id);
            LOG("nvEncOpenEncodeSessionEx failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        NV_ENC_PRESET_CONFIG presetConfig;
        memset(&presetConfig, 0, sizeof(presetConfig));
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

        nvs = drv->nvencFuncs.nvEncGetEncodePresetConfigEx(
            nvCtx->nvencEncoder,
            nvCtx->encCodecGuid,
            nvCtx->encPresetGuid,
            NV_ENC_TUNING_INFO_LOW_LATENCY,
            &presetConfig);
        if (nvs != NV_ENC_SUCCESS) {
            drv->nvencFuncs.nvEncDestroyEncoder(nvCtx->nvencEncoder);
            nvCtx->nvencEncoder = NULL;
            cu->cuCtxPopCurrent(NULL);
            deleteObject(drv, contextObj->id);
            LOG("nvEncGetEncodePresetConfigEx failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        nvCtx->encConfig = presetConfig.presetCfg;
        nvCtx->encConfig.version = NV_ENC_CONFIG_VER;
        nvCtx->encConfig.profileGUID = nvCtx->encProfileGuid;
        nvCtx->encConfig.gopLength = 120;
        nvCtx->encConfig.frameIntervalP = 1;
        nvCtx->encConfig.rcParams.version = NV_ENC_RC_PARAMS_VER;
        nvCtx->encConfig.rcParams.enableLookahead = 0;
        nvCtx->encConfig.rcParams.lookaheadDepth = 0;
        nvCtx->encConfig.rcParams.zeroReorderDelay = 1;

        if (cfg->encodeRCMode & VA_RC_CBR) {
            nvCtx->encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            nvCtx->encConfig.rcParams.averageBitRate = 5000000;
            nvCtx->encConfig.rcParams.maxBitRate = 5000000;
        } else if (cfg->encodeRCMode & VA_RC_VBR) {
            nvCtx->encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
            nvCtx->encConfig.rcParams.averageBitRate = 5000000;
            nvCtx->encConfig.rcParams.maxBitRate = 10000000;
        } else if (cfg->encodeRCMode & VA_RC_CQP) {
            nvCtx->encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            nvCtx->encConfig.rcParams.constQP.qpInterP = 28;
            nvCtx->encConfig.rcParams.constQP.qpInterB = 31;
            nvCtx->encConfig.rcParams.constQP.qpIntra = 25;
        } else {
            nvCtx->encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            nvCtx->encConfig.rcParams.averageBitRate = 5000000;
            nvCtx->encConfig.rcParams.maxBitRate = 5000000;
        }

        if (!nvCtx->encIsHevc) {
            nvCtx->encConfig.encodeCodecConfig.h264Config.idrPeriod = nvCtx->encConfig.gopLength;
            nvCtx->encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        } else {
            nvCtx->encConfig.encodeCodecConfig.hevcConfig.idrPeriod = nvCtx->encConfig.gopLength;
            nvCtx->encConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        }

        memset(&nvCtx->encInitParams, 0, sizeof(nvCtx->encInitParams));
        nvCtx->encInitParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        nvCtx->encInitParams.encodeGUID = nvCtx->encCodecGuid;
        nvCtx->encInitParams.presetGUID = nvCtx->encPresetGuid;
        nvCtx->encInitParams.encodeWidth = picture_width;
        nvCtx->encInitParams.encodeHeight = picture_height;
        nvCtx->encInitParams.darWidth = picture_width;
        nvCtx->encInitParams.darHeight = picture_height;
        nvCtx->encInitParams.frameRateNum = 30;
        nvCtx->encInitParams.frameRateDen = 1;
        nvCtx->encInitParams.enableEncodeAsync = 0;
        nvCtx->encInitParams.enablePTD = 1;
        nvCtx->encInitParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        nvCtx->encInitParams.encodeConfig = &nvCtx->encConfig;
        nvCtx->encInitParams.maxEncodeWidth = picture_width;
        nvCtx->encInitParams.maxEncodeHeight = picture_height;

        nvs = drv->nvencFuncs.nvEncInitializeEncoder(nvCtx->nvencEncoder, &nvCtx->encInitParams);
        if (nvs != NV_ENC_SUCCESS) {
            drv->nvencFuncs.nvEncDestroyEncoder(nvCtx->nvencEncoder);
            nvCtx->nvencEncoder = NULL;
            cu->cuCtxPopCurrent(NULL);
            deleteObject(drv, contextObj->id);
            LOG("nvEncInitializeEncoder failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }
        nvCtx->encLastReconfigureNs = getMonotonicNs();

        for (uint32_t i = 0; i < nvCtx->nvencIoBufferCount; i++) {
            NV_ENC_CREATE_INPUT_BUFFER createInput;
            memset(&createInput, 0, sizeof(createInput));
            createInput.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
            createInput.width = picture_width;
            createInput.height = picture_height;
            createInput.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

            nvs = drv->nvencFuncs.nvEncCreateInputBuffer(nvCtx->nvencEncoder, &createInput);
            if (nvs != NV_ENC_SUCCESS) {
                nvencDestroyIoBuffers(drv, nvCtx);
                drv->nvencFuncs.nvEncDestroyEncoder(nvCtx->nvencEncoder);
                nvCtx->nvencEncoder = NULL;
                cu->cuCtxPopCurrent(NULL);
                deleteObject(drv, contextObj->id);
                LOG("nvEncCreateInputBuffer[%u] failed: %s", i, nvencStatusStr(nvs));
                return nvencToVaStatus(nvs);
            }
            nvCtx->nvencInputBuffers[i] = createInput.inputBuffer;

            NV_ENC_CREATE_BITSTREAM_BUFFER createBS;
            memset(&createBS, 0, sizeof(createBS));
            createBS.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            nvs = drv->nvencFuncs.nvEncCreateBitstreamBuffer(nvCtx->nvencEncoder, &createBS);
            if (nvs != NV_ENC_SUCCESS) {
                nvencDestroyIoBuffers(drv, nvCtx);
                drv->nvencFuncs.nvEncDestroyEncoder(nvCtx->nvencEncoder);
                nvCtx->nvencEncoder = NULL;
                cu->cuCtxPopCurrent(NULL);
                deleteObject(drv, contextObj->id);
                LOG("nvEncCreateBitstreamBuffer[%u] failed: %s", i, nvencStatusStr(nvs));
                return nvencToVaStatus(nvs);
            }
            nvCtx->nvencOutputBuffers[i] = createBS.bitstreamBuffer;
        }

        LOG("NVENC IO ring depth: %u", nvCtx->nvencIoBufferCount);

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
        *context = contextObj->id;
        return VA_STATUS_SUCCESS;
    }

    LOG("Creating context with %d render targets, at %dx%d", num_render_targets, picture_width, picture_height);

    //find the codec they've selected
    const NVCodec *selectedCodec = NULL;
    for (const NVCodec *c = __start_nvd_codecs; c < __stop_nvd_codecs; c++) {
        for (int i = 0; i < c->supportedProfileCount; i++) {
            if (c->supportedProfiles[i] == cfg->profile) {
                selectedCodec = c;
                break;
            }
        }
    }
    if (selectedCodec == NULL) {
        LOG("Unable to find codec for profile: %d", cfg->profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (num_render_targets) {
        // Update the decoder configuration to match the passed in surfaces.
        NVSurface *surface = (NVSurface *) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_targets[0]);
        if (!surface) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        cfg->surfaceFormat = surface->format;
        cfg->chromaFormat = surface->chromaFormat;
        cfg->bitDepth = surface->bitDepth;
    }

     // Setting to maximun value if num_render_targets == 0 to prevent picture index overflow as additional surfaces can be created after calling nvCreateContext
    int surfaceCount = num_render_targets > 0 ? num_render_targets : 32;

    if (surfaceCount > 32) {
        LOG("Application requested %d surface(s), limiting to 32. This may cause issues.", surfaceCount);
        surfaceCount = 32;
    }

    int display_area_width = picture_width;
    int display_area_height = picture_height;

    // If we're increasing the surface size for the chroma subsampling,
    // increase the displayArea to match
    switch(cfg->chromaFormat) {
        case cudaVideoChromaFormat_422:
            display_area_width = ROUND_UP(display_area_width, 2);
            break;
        case cudaVideoChromaFormat_420:
            display_area_width = ROUND_UP(display_area_width, 2);
            display_area_height = ROUND_UP(display_area_height, 2);
            break;
        default:
            // no change needed
            break;
    }

    CUVIDDECODECREATEINFO vdci = {
        .ulWidth             = vdci.ulMaxWidth  = vdci.ulTargetWidth  = picture_width,
        .ulHeight            = vdci.ulMaxHeight = vdci.ulTargetHeight = picture_height,
        .CodecType           = cfg->cudaCodec,
        .ulCreationFlags     = cudaVideoCreate_PreferCUVID,
        .ulIntraDecodeOnly   = 0, //TODO (flag & VA_PROGRESSIVE) != 0
        .display_area.right  = display_area_width,
        .display_area.bottom = display_area_height,
        .ChromaFormat        = cfg->chromaFormat,
        .OutputFormat        = cfg->surfaceFormat,
        .bitDepthMinus8      = cfg->bitDepth - 8,
        .DeinterlaceMode     = cudaVideoDeinterlaceMode_Weave,

        //we only ever map one frame at a time, so we can set this to 1
        //it isn't particually efficient to do this, but it is simple
        .ulNumOutputSurfaces = 1,
        //just allocate as many surfaces as have been created since we can never have as much information as the decode to guess correctly
        .ulNumDecodeSurfaces = surfaceCount,
        //.vidLock             = drv->vidLock
    };

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    CUvideodecoder decoder;
    CHECK_CUDA_RESULT_RETURN(cv->cuvidCreateDecoder(&decoder, &vdci), VA_STATUS_ERROR_ALLOCATION_FAILED);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    LOG("Creating decoder: %p for context id: %d", decoder, contextObj->id);

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->mode = NV_CONTEXT_DECODE;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;
    nvCtx->codec = selectedCodec;
    nvCtx->surfaceCount = surfaceCount;
    nvCtx->firstKeyframeValid = false;
    
    pthread_mutexattr_t attrib;
    pthread_mutexattr_init(&attrib);
    pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&nvCtx->surfaceCreationMutex, &attrib);

    pthread_mutex_init(&nvCtx->resolveMutex, NULL);
    pthread_cond_init(&nvCtx->resolveCondition, NULL);
    int err = pthread_create(&nvCtx->resolveThread, NULL, &resolveSurfaces, nvCtx);
    if (err != 0) {
        LOG("Unable to create resolve thread: %d", err);
        deleteObject(drv, contextObj->id);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    *context = contextObj->id;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyContext(
        VADriverContextP ctx,
        VAContextID context)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Destroying context: %d", context);

    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    VAStatus ret = VA_STATUS_SUCCESS;

    if (!destroyContext(drv, nvCtx)) {
        ret = VA_STATUS_ERROR_OPERATION_FAILED;
    }

    deleteObject(drv, context);

    return ret;
}

static VAStatus nvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,		/* in */
        VABufferType type,		/* in */
        unsigned int size,		/* in */
        unsigned int num_elements,	/* in */
        void *data,			/* in */
        VABufferID *buf_id
    )
{
    //LOG("got buffer %p, type %x, size %u, elements %u", data, type, size, num_elements);
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    if (context != 0 && nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    bool isEncode = (nvCtx != NULL && nvCtx->mode == NV_CONTEXT_ENCODE);

    //HACK: This is an awful hack to support VP8 videos when running within FFMPEG.
    //VA-API doesn't pass enough information for NVDEC to work with, but the information is there
    //just before the start of the buffer that was passed to us.
    size_t offset = 0;
    if (!isEncode && nvCtx != NULL && nvCtx->profile == VAProfileVP8Version0_3 && type == VASliceDataBufferType) {
        offset = ((uintptr_t) data) & 0xf;
        data = ((char *) data) - offset;
        size += (unsigned int)offset;
    }

    //TODO should pool these as most of the time these should be the same size
    Object bufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    buf->offset = offset;
    buf->ownsPtr = true;
    buf->surfaceId = VA_INVALID_SURFACE;
    buf->commitOnUnmap = 0;
    buf->mappedSurfaceLock = 0;

    if (isEncode && type == VAEncCodedBufferType) {
        size_t codedCapacity = buf->size > NVENC_CODED_BUF_SIZE ? buf->size : NVENC_CODED_BUF_SIZE;
        buf->codedData = calloc(1, sizeof(VACodedBufferSegment) + codedCapacity);
        if (!buf->codedData) {
            deleteObject(drv, bufferObject->id);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        buf->codedCapacity = codedCapacity;
        buf->codedSeg = (VACodedBufferSegment*) buf->codedData;
        memset(buf->codedSeg, 0, sizeof(VACodedBufferSegment));
        buf->codedSeg->buf = buf->codedData + sizeof(VACodedBufferSegment);
        buf->codedSeg->size = 0;
        buf->codedSeg->status = 0;
        buf->codedSeg->next = NULL;
        return VA_STATUS_SUCCESS;
    }

    buf->ptr = memalign(16, buf->size);

    if (buf->ptr == NULL) {
        LOG("Unable to allocate buffer of %zu bytes", buf->size);
        deleteObject(drv, bufferObject->id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (data != NULL)
    {
        memcpy(buf->ptr, data, buf->size);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        unsigned int num_elements	/* in */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);
    if (!buf) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    buf->elements = num_elements;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvMapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        void **pbuf         /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buf->bufferType == VAImageBufferType && buf->surfaceId != VA_INVALID_SURFACE) {
        NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, buf->surfaceId);
        if (surf != NULL && ensureSurfaceHostBuffer(surf) && g_encStrictMode) {
            size_t need = surf->hostTotalSize;
            if (!buf->ownsPtr || buf->ptr == surf->hostData || buf->size != need || buf->ptr == NULL) {
                uint8_t *shadow = realloc((buf->ownsPtr && buf->ptr != NULL) ? buf->ptr : NULL, need);
                if (shadow == NULL) {
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                buf->ptr = shadow;
                buf->size = need;
                buf->ownsPtr = true;
            }
            pthread_mutex_lock(&surf->mutex);
            memcpy(buf->ptr, surf->hostData, need);
            pthread_mutex_unlock(&surf->mutex);
            *pbuf = buf->ptr;
            return VA_STATUS_SUCCESS;
        }
    }

    if (buf->codedSeg != NULL) {
        *pbuf = buf->codedSeg;
    } else {
        *pbuf = buf->ptr;
    }

    if (buf->bufferType == VAImageBufferType && buf->surfaceId != VA_INVALID_SURFACE && !buf->mappedSurfaceLock) {
        NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, buf->surfaceId);
        if (surf != NULL) {
            pthread_mutex_lock(&surf->mutex);
            buf->mappedSurfaceLock = 1;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvUnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id	/* in */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);
    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (buf->bufferType == VAImageBufferType && buf->surfaceId != VA_INVALID_SURFACE) {
        NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, buf->surfaceId);
        if (surf != NULL && ensureSurfaceHostBuffer(surf)) {
            int strictCommitted = 0;
            if (g_encStrictMode && buf->ptr != NULL && buf->ownsPtr) {
                size_t copy = buf->size;
                if (copy > surf->hostTotalSize) {
                    copy = surf->hostTotalSize;
                }
                pthread_mutex_lock(&surf->mutex);
                memcpy(surf->hostData, buf->ptr, copy);
                strictCommitted = 1;
            } else if (!buf->mappedSurfaceLock) {
                pthread_mutex_lock(&surf->mutex);
            }
            surf->contentX = 0;
            surf->contentY = 0;
            surf->contentWidth = surf->width;
            surf->contentHeight = surf->height;
            surf->contentValid = 1;
            surf->status = VASurfaceReady;
            if (strictCommitted) {
                pthread_mutex_unlock(&surf->mutex);
            } else if (buf->mappedSurfaceLock) {
                pthread_mutex_unlock(&surf->mutex);
                buf->mappedSurfaceLock = 0;
            } else {
                pthread_mutex_unlock(&surf->mutex);
            }
        }
    }
    if (buf->commitOnUnmap && buf->surfaceId != VA_INVALID_SURFACE && buf->ptr != NULL) {
        NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, buf->surfaceId);
        if (surf != NULL && ensureSurfaceHostBuffer(surf)) {
            size_t copy = buf->size;
            if (copy > surf->hostTotalSize) {
                copy = surf->hostTotalSize;
            }
            pthread_mutex_lock(&surf->mutex);
            memcpy(surf->hostData, buf->ptr, copy);
            surf->contentX = 0;
            surf->contentY = 0;
            surf->contentWidth = surf->width;
            surf->contentHeight = surf->height;
            surf->contentValid = 1;
            surf->status = VASurfaceReady;
            pthread_mutex_unlock(&surf->mutex);
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffer_id);

    if (buf == NULL) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    if (buf->ptr != NULL && buf->ownsPtr) {
        free(buf->ptr);
    }
    if (buf->codedData != NULL) {
        free(buf->codedData);
    }
    if (buf->mappedSurfaceLock && buf->surfaceId != VA_INVALID_SURFACE) {
        NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, buf->surfaceId);
        if (surf != NULL) {
            pthread_mutex_unlock(&surf->mutex);
        }
        buf->mappedSurfaceLock = 0;
    }

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvBeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);
    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (nvCtx->mode == NV_CONTEXT_ENCODE) {
        if (!ensureSurfaceHostBuffer(surface)) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        nvCtx->encCurrentSurface = render_target;
        nvCtx->encCodedBufId = VA_INVALID_ID;
        nvCtx->encHasCodedBuf = 0;
        nvCtx->encForceIdr = (nvCtx->encFrameNum == 0);
        surface->context = nvCtx;
        surface->contentX = 0;
        surface->contentY = 0;
        surface->contentWidth = surface->width;
        surface->contentHeight = surface->height;
        surface->contentValid = 0;
        surface->status = VASurfaceRendering;
        return VA_STATUS_SUCCESS;
    }

    if (surface->context != NULL && surface->context != nvCtx) {
        //this surface was last used on a different context, we need to free up the backing image (it might not be the correct size)
        if (surface->backingImage != NULL) {
            drv->backend->detachBackingImageFromSurface(drv, surface);
        }
        //...and reset the pictureIdx
        surface->pictureIdx = -1;
    }

    //if this surface hasn't been used before, give it a new picture index
    if (surface->pictureIdx == -1) {
        if (nvCtx->currentPictureId == nvCtx->surfaceCount) {
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        surface->pictureIdx = nvCtx->currentPictureId++;
    }

    //I don't know if we actually need to lock here, nothing should be waiting
    //until after this function returns...
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 1;
    surface->status = VASurfaceRendering;
    pthread_mutex_unlock(&surface->mutex);

    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->renderTarget = surface;
    nvCtx->renderTarget->progressiveFrame = true; //assume we're producing progressive frame unless the codec says otherwise
    nvCtx->pPicParams.CurrPicIdx = nvCtx->renderTarget->pictureIdx;

    return VA_STATUS_SUCCESS;
}

static VAStatus nvRenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (nvCtx->mode == NV_CONTEXT_ENCODE) {
        for (int i = 0; i < num_buffers; i++) {
            NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
            if (buf == NULL) {
                continue;
            }
            switch (buf->bufferType) {
                case VAEncCodedBufferType:
                    nvCtx->encCodedBufId = buffers[i];
                    nvCtx->encHasCodedBuf = 1;
                    break;
                case VAEncSequenceParameterBufferType:
                    if (!nvCtx->encIsHevc && buf->ptr != NULL && buf->size >= sizeof(VAEncSequenceParameterBufferH264)) {
                        const VAEncSequenceParameterBufferH264 *seq = (const VAEncSequenceParameterBufferH264 *) buf->ptr;
                        nvencSetBitrate(nvCtx, seq->bits_per_second);
                        nvencSetGop(nvCtx, seq->intra_period, seq->intra_idr_period);
                        nvencUpdateVisibleRectFromH264Seq(nvCtx, seq);
                        if (seq->vui_fields.bits.timing_info_present_flag && seq->time_scale != 0) {
                            uint32_t den = seq->num_units_in_tick ? (seq->num_units_in_tick * 2U) : 1U;
                            nvencSetFrameRate(nvCtx, seq->time_scale, den);
                        }
                    } else if (nvCtx->encIsHevc && buf->ptr != NULL && buf->size >= sizeof(VAEncSequenceParameterBufferHEVC)) {
                        const VAEncSequenceParameterBufferHEVC *seq = (const VAEncSequenceParameterBufferHEVC *) buf->ptr;
                        nvencSetBitrate(nvCtx, seq->bits_per_second);
                        nvencSetGop(nvCtx, seq->intra_period, seq->intra_idr_period);
                        nvencUpdateVisibleRectFromHevcSeq(nvCtx, seq);
                        if (seq->vui_fields.bits.vui_timing_info_present_flag && seq->vui_time_scale != 0) {
                            uint32_t den = seq->vui_num_units_in_tick ? seq->vui_num_units_in_tick : 1U;
                            nvencSetFrameRate(nvCtx, seq->vui_time_scale, den);
                        }
                    }
                    break;
                case VAEncPictureParameterBufferType:
                    if (!nvCtx->encIsHevc && buf->ptr != NULL && buf->size >= sizeof(VAEncPictureParameterBufferH264)) {
                        const VAEncPictureParameterBufferH264 *pic = (const VAEncPictureParameterBufferH264 *) buf->ptr;
                        if (pic->coded_buf != VA_INVALID_ID) {
                            NVBuffer *codedBuf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, pic->coded_buf);
                            if (codedBuf != NULL && codedBuf->bufferType == VAEncCodedBufferType) {
                                nvCtx->encCodedBufId = pic->coded_buf;
                                nvCtx->encHasCodedBuf = 1;
                            }
                        }
                        if (pic->pic_fields.bits.idr_pic_flag) {
                            nvCtx->encForceIdr = 1;
                        }
                    } else if (nvCtx->encIsHevc && buf->ptr != NULL && buf->size >= sizeof(VAEncPictureParameterBufferHEVC)) {
                        const VAEncPictureParameterBufferHEVC *pic = (const VAEncPictureParameterBufferHEVC *) buf->ptr;
                        if (pic->coded_buf != VA_INVALID_ID) {
                            NVBuffer *codedBuf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, pic->coded_buf);
                            if (codedBuf != NULL && codedBuf->bufferType == VAEncCodedBufferType) {
                                nvCtx->encCodedBufId = pic->coded_buf;
                                nvCtx->encHasCodedBuf = 1;
                            }
                        }
                        if (pic->pic_fields.bits.idr_pic_flag) {
                            nvCtx->encForceIdr = 1;
                        }
                    }
                    break;
                case VAEncMiscParameterBufferType:
                    if (buf->ptr != NULL && buf->size >= sizeof(VAEncMiscParameterBuffer)) {
                        const VAEncMiscParameterBuffer *misc = (const VAEncMiscParameterBuffer *) buf->ptr;
                        const void *payload = misc->data;
                        if (misc->type == VAEncMiscParameterTypeRateControl &&
                            buf->size >= sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl)) {
                            const VAEncMiscParameterRateControl *rc = (const VAEncMiscParameterRateControl *) payload;
                            if (rc->bits_per_second > 0) {
                                if (rc->target_percentage && nvCtx->encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_VBR) {
                                    uint64_t avg = ((uint64_t) rc->bits_per_second * (uint64_t) rc->target_percentage) / 100ULL;
                                    if (avg > UINT32_MAX) {
                                        avg = UINT32_MAX;
                                    }
                                    nvencSetBitrate(nvCtx, (uint32_t) avg);
                                    if (setU32IfChanged(&nvCtx->encConfig.rcParams.maxBitRate, rc->bits_per_second)) {
                                        nvCtx->encNeedsReconfigure = 1;
                                    }
                                } else {
                                    nvencSetBitrate(nvCtx, rc->bits_per_second);
                                }
                            }
                            if (rc->max_qp > 0) {
                                int changed = 0;
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.maxQP.qpInterB, rc->max_qp);
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.maxQP.qpInterP, rc->max_qp);
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.maxQP.qpIntra, rc->max_qp);
                                if (changed) {
                                    nvCtx->encNeedsReconfigure = 1;
                                }
                            }
                            if (rc->min_qp > 0) {
                                int changed = 0;
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.minQP.qpInterB, rc->min_qp);
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.minQP.qpInterP, rc->min_qp);
                                changed |= setU32IfChanged(&nvCtx->encConfig.rcParams.minQP.qpIntra, rc->min_qp);
                                if (changed) {
                                    nvCtx->encNeedsReconfigure = 1;
                                }
                            }
                        } else if (misc->type == VAEncMiscParameterTypeFrameRate &&
                                   buf->size >= sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate)) {
                            const VAEncMiscParameterFrameRate *fr = (const VAEncMiscParameterFrameRate *) payload;
                            uint32_t fpsNum = fr->framerate & 0xffff;
                            uint32_t fpsDen = (fr->framerate >> 16) & 0xffff;
                            if (fpsDen == 0) {
                                fpsDen = 1;
                            }
                            nvencSetFrameRate(nvCtx, fpsNum, fpsDen);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        return VA_STATUS_SUCCESS;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    for (int i = 0; i < num_buffers; i++) {
        NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buffers[i]);
        if (buf == NULL || buf->ptr == NULL) {
            LOG("Invalid buffer detected, skipping: %d", buffers[i]);
            continue;
        }
        HandlerFunc func = nvCtx->codec->handlers[buf->bufferType];
        if (func != NULL) {
            func(nvCtx, buf, picParams);
        } else {
            LOG("Unhandled buffer type: %d", buf->bufferType);
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObjectPtr(drv, OBJECT_TYPE_CONTEXT, context);

    if (nvCtx == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    if (nvCtx->mode == NV_CONTEXT_ENCODE) {
        if (nvCtx->encCurrentSurface == VA_INVALID_SURFACE) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (!nvCtx->encHasCodedBuf) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, nvCtx->encCurrentSurface);
        NVBuffer *codedBuf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, nvCtx->encCodedBufId);
        if (!surface || !surface->hostData) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (!codedBuf || !codedBuf->codedSeg || codedBuf->codedCapacity == 0) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        if (nvCtx->nvencIoBufferCount == 0) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uint32_t ioSlot = nvCtx->nvencIoBufferIndex % nvCtx->nvencIoBufferCount;
        NV_ENC_INPUT_PTR inputBuffer = nvCtx->nvencInputBuffers[ioSlot];
        NV_ENC_OUTPUT_PTR outputBuffer = nvCtx->nvencOutputBuffers[ioSlot];
        if (inputBuffer == NULL || outputBuffer == NULL) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        VAStatus vas = nvencReconfigureIfNeeded(drv, nvCtx);
        if (vas != VA_STATUS_SUCCESS) {
            surface->status = VASurfaceReady;
            return vas;
        }

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

        NV_ENC_LOCK_INPUT_BUFFER lockInput;
        memset(&lockInput, 0, sizeof(lockInput));
        lockInput.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockInput.inputBuffer = inputBuffer;

        NVENCSTATUS nvs = drv->nvencFuncs.nvEncLockInputBuffer(nvCtx->nvencEncoder, &lockInput);
        if (nvs != NV_ENC_SUCCESS) {
            cu->cuCtxPopCurrent(NULL);
            surface->status = VASurfaceReady;
            LOG("nvEncLockInputBuffer failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        uint8_t *dst = (uint8_t *) lockInput.bufferDataPtr;
        const uint8_t *src = surface->hostData;
        const size_t srcPitch = surface->hostPitch;
        const size_t dstPitch = lockInput.pitch;
        uint32_t encWidth = (uint32_t) nvCtx->width;
        uint32_t encHeight = (uint32_t) nvCtx->height;
        uint32_t srcX = 0;
        uint32_t srcY = 0;
        uint32_t copyWidth = surface->width < encWidth ? surface->width : encWidth;
        uint32_t copyHeight = surface->height < encHeight ? surface->height : encHeight;
        uint64_t encDbgPreFullHash = 0;
        uint64_t encDbgPreTopHash = 0;
        uint32_t encDbgFrame = 0;
        int encDbgEnabled = 0;
        int encDbgDump = 0;

        nvencDebugInitOnce();
        if (g_encDebugEnabled) {
            pthread_mutex_lock(&g_encDebugMutex);
            encDbgFrame = g_encDebugFrameIndex++;
            encDbgEnabled = 1;
            if (encDbgFrame >= g_encDebugStartFrame) {
                uint32_t rel = encDbgFrame - g_encDebugStartFrame;
                encDbgDump = (rel < g_encDebugMaxFrames);
            } else {
                encDbgDump = 0;
            }
            pthread_mutex_unlock(&g_encDebugMutex);
        }

        if (nvCtx->encVisibleValid && nvCtx->encVisibleWidth > 0 && nvCtx->encVisibleHeight > 0) {
            srcX = nvCtx->encVisibleX;
            srcY = nvCtx->encVisibleY;
            copyWidth = nvCtx->encVisibleWidth < encWidth ? nvCtx->encVisibleWidth : encWidth;
            copyHeight = nvCtx->encVisibleHeight < encHeight ? nvCtx->encVisibleHeight : encHeight;
        } else if (surface->contentValid && surface->contentWidth > 0 && surface->contentHeight > 0) {
            srcX = surface->contentX;
            srcY = surface->contentY;
            copyWidth = surface->contentWidth < encWidth ? surface->contentWidth : encWidth;
            copyHeight = surface->contentHeight < encHeight ? surface->contentHeight : encHeight;
            if (srcX >= surface->width || srcY >= surface->height) {
                srcX = 0;
                srcY = 0;
                copyWidth = surface->width < encWidth ? surface->width : encWidth;
                copyHeight = surface->height < encHeight ? surface->height : encHeight;
            } else {
                if (srcX + copyWidth > surface->width) {
                    copyWidth = surface->width - srcX;
                }
                if (srcY + copyHeight > surface->height) {
                    copyHeight = surface->height - srcY;
                }
            }
        }

        srcX &= ~1U;
        srcY &= ~1U;
        copyWidth &= ~1U;
        copyHeight &= ~1U;
        if (copyWidth == 0 || copyHeight == 0) {
            copyWidth = encWidth & ~1U;
            copyHeight = encHeight & ~1U;
            srcX = 0;
            srcY = 0;
        }

        uint8_t *dstUV = dst + ((size_t) encHeight * dstPitch);

        pthread_mutex_lock(&surface->mutex);
        const uint8_t *srcYBase = src + ((size_t) srcY * srcPitch) + (size_t) srcX;
        const uint8_t *srcUV = src + surface->hostYSize +
                               ((size_t) (srcY / 2U) * srcPitch) + (size_t) srcX;

        if (srcX == 0 && srcY == 0 &&
            copyWidth == encWidth && copyHeight == encHeight &&
            srcPitch == dstPitch) {
            /* Fast path for full-frame copies with matching pitches. */
            memcpy(dst, src, (size_t) encHeight * dstPitch);
            memcpy(dstUV, src + surface->hostYSize, ((size_t) encHeight / 2U) * dstPitch);
        } else {
            /* Copy luma/chroma payload first. */
            for (uint32_t y = 0; y < copyHeight; y++) {
                memcpy(dst + ((size_t) y * dstPitch),
                       srcYBase + ((size_t) y * srcPitch),
                       copyWidth);
            }
            for (uint32_t y = 0; y < (copyHeight / 2U); y++) {
                memcpy(dstUV + ((size_t) y * dstPitch),
                       srcUV + ((size_t) y * srcPitch),
                       copyWidth);
            }

            /* Clear only uncovered padding regions to keep output deterministic. */
            if (copyWidth < encWidth) {
                size_t pad = (size_t) (encWidth - copyWidth);
                for (uint32_t y = 0; y < copyHeight; y++) {
                    memset(dst + ((size_t) y * dstPitch) + copyWidth, 0x00, pad);
                }
                for (uint32_t y = 0; y < (copyHeight / 2U); y++) {
                    memset(dstUV + ((size_t) y * dstPitch) + copyWidth, 0x80, pad);
                }
            }
            if (copyHeight < encHeight) {
                for (uint32_t y = copyHeight; y < encHeight; y++) {
                    memset(dst + ((size_t) y * dstPitch), 0x00, encWidth);
                }
                for (uint32_t y = (copyHeight / 2U); y < (encHeight / 2U); y++) {
                    memset(dstUV + ((size_t) y * dstPitch), 0x80, encWidth);
                }
            }
        }
        pthread_mutex_unlock(&surface->mutex);

        if (encDbgEnabled) {
            uint64_t h = 1469598103934665603ULL;
            uint32_t topRows = copyHeight < 32U ? copyHeight : 32U;
            uint32_t topRowsUV = topRows / 2U;

            for (uint32_t row = 0; row < copyHeight; row++) {
                h = fnv1a64_append(h, dst + ((size_t) row * dstPitch), copyWidth);
            }
            for (uint32_t row = 0; row < (copyHeight / 2U); row++) {
                h = fnv1a64_append(h, dstUV + ((size_t) row * dstPitch), copyWidth);
            }
            encDbgPreFullHash = h;

            h = 1469598103934665603ULL;
            for (uint32_t row = 0; row < topRows; row++) {
                h = fnv1a64_append(h, dst + ((size_t) row * dstPitch), copyWidth);
            }
            for (uint32_t row = 0; row < topRowsUV; row++) {
                h = fnv1a64_append(h, dstUV + ((size_t) row * dstPitch), copyWidth);
            }
            encDbgPreTopHash = h;

            LOG("ENCDBG pre frame=%u hash_full=%016llx hash_top32=%016llx copy=%ux%u enc=%ux%u src=(%u,%u) pitch=%zu",
                encDbgFrame,
                (unsigned long long) encDbgPreFullHash,
                (unsigned long long) encDbgPreTopHash,
                copyWidth, copyHeight, encWidth, encHeight, srcX, srcY, dstPitch);

            if (encDbgDump) {
                char leaf[128];
                char path[PATH_MAX];
                snprintf(leaf, sizeof(leaf), "frame_%06u_pre_nv12_%ux%u.yuv",
                         encDbgFrame, copyWidth, copyHeight);
                if (nvencDebugBuildPath(path, sizeof(path), leaf)) {
                    nvencDebugDumpNV12(path, dst, dstUV, dstPitch, copyWidth, copyHeight);
                }
            }
        }

        nvs = drv->nvencFuncs.nvEncUnlockInputBuffer(nvCtx->nvencEncoder, inputBuffer);
        if (nvs != NV_ENC_SUCCESS) {
            cu->cuCtxPopCurrent(NULL);
            surface->status = VASurfaceReady;
            LOG("nvEncUnlockInputBuffer failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        NV_ENC_PIC_PARAMS picParams;
        memset(&picParams, 0, sizeof(picParams));
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.inputBuffer = inputBuffer;
        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        picParams.inputPitch = (uint32_t) dstPitch;
        picParams.inputWidth = copyWidth;
        picParams.inputHeight = copyHeight;
        picParams.outputBitstream = outputBuffer;
        picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picParams.inputTimeStamp = nvCtx->encFrameNum;

        if (g_encForceIdrEvery > 0 && (nvCtx->encFrameNum % g_encForceIdrEvery) == 0) {
            nvCtx->encForceIdr = 1;
        }
        if (nvCtx->encStartupIdrLeft > 0) {
            nvCtx->encForceIdr = 1;
        }
        if (nvCtx->encForceIdr || nvCtx->encFrameNum == 0) {
            picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            picParams.pictureType = NV_ENC_PIC_TYPE_IDR;
        }

        nvs = drv->nvencFuncs.nvEncEncodePicture(nvCtx->nvencEncoder, &picParams);
        if (nvs == NV_ENC_ERR_INVALID_PARAM &&
            (picParams.inputWidth != (uint32_t) nvCtx->width || picParams.inputHeight != (uint32_t) nvCtx->height)) {
            LOG("nvEncEncodePicture rejected dynamic input size %ux%u (ctx %dx%d), trying reconfigure",
                picParams.inputWidth, picParams.inputHeight, nvCtx->width, nvCtx->height);

            int reconfigured = 0;
            if (drv->nvencFuncs.nvEncReconfigureEncoder) {
                NV_ENC_RECONFIGURE_PARAMS reconfig;
                memset(&reconfig, 0, sizeof(reconfig));
                reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;
                reconfig.reInitEncodeParams = nvCtx->encInitParams;
                reconfig.reInitEncodeParams.encodeConfig = &nvCtx->encConfig;
                reconfig.reInitEncodeParams.encodeWidth = copyWidth;
                reconfig.reInitEncodeParams.encodeHeight = copyHeight;
                reconfig.reInitEncodeParams.darWidth = copyWidth;
                reconfig.reInitEncodeParams.darHeight = copyHeight;
                if (reconfig.reInitEncodeParams.maxEncodeWidth < copyWidth) {
                    reconfig.reInitEncodeParams.maxEncodeWidth = copyWidth;
                }
                if (reconfig.reInitEncodeParams.maxEncodeHeight < copyHeight) {
                    reconfig.reInitEncodeParams.maxEncodeHeight = copyHeight;
                }

                NVENCSTATUS rnvs = drv->nvencFuncs.nvEncReconfigureEncoder(nvCtx->nvencEncoder, &reconfig);
                if (rnvs == NV_ENC_SUCCESS) {
                    nvCtx->encInitParams = reconfig.reInitEncodeParams;
                    nvCtx->encInitParams.encodeConfig = &nvCtx->encConfig;
                    nvCtx->width = (int) copyWidth;
                    nvCtx->height = (int) copyHeight;
                    nvCtx->encLastReconfigureNs = getMonotonicNs();
                    nvCtx->encStartupIdrLeft = g_encStartupIdrFrames;
                    reconfigured = 1;
                    LOG("NVENC resolution reconfigured to %ux%u", copyWidth, copyHeight);
                } else {
                    LOG("nvEncReconfigureEncoder resolution change failed: %s", nvencStatusStr(rnvs));
                }
            }

            if (reconfigured) {
                nvs = drv->nvencFuncs.nvEncEncodePicture(nvCtx->nvencEncoder, &picParams);
            }
            if (nvs == NV_ENC_ERR_INVALID_PARAM) {
                picParams.inputWidth = (uint32_t) nvCtx->width;
                picParams.inputHeight = (uint32_t) nvCtx->height;
                LOG("Falling back to context encode size %ux%u", picParams.inputWidth, picParams.inputHeight);
                nvs = drv->nvencFuncs.nvEncEncodePicture(nvCtx->nvencEncoder, &picParams);
            }
        }
        if (nvs != NV_ENC_SUCCESS && nvs != NV_ENC_ERR_NEED_MORE_INPUT) {
            cu->cuCtxPopCurrent(NULL);
            surface->status = VASurfaceReady;
            LOG("nvEncEncodePicture failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        if (nvs == NV_ENC_ERR_NEED_MORE_INPUT) {
            nvCtx->encNeedMoreInputStreak++;
            if (nvCtx->encNeedMoreInputStreak == 1 || (nvCtx->encNeedMoreInputStreak % 30U) == 0U) {
                LOG("NVENC needs more input (streak=%u, frame=%u)",
                    nvCtx->encNeedMoreInputStreak, nvCtx->encFrameNum);
            }
            codedBuf->codedSeg->status = 0;
            codedBuf->codedSeg->size = 0;
            codedBuf->codedSeg->next = NULL;

            CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);
            nvCtx->encFrameNum++;
            nvCtx->encForceIdr = 0;
            nvCtx->nvencIoBufferIndex = (ioSlot + 1U) % nvCtx->nvencIoBufferCount;
            surface->status = VASurfaceReady;
            return VA_STATUS_SUCCESS;
        }

        NV_ENC_LOCK_BITSTREAM lockBS;
        memset(&lockBS, 0, sizeof(lockBS));
        lockBS.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBS.outputBitstream = outputBuffer;

        nvs = drv->nvencFuncs.nvEncLockBitstream(nvCtx->nvencEncoder, &lockBS);
        if (nvs != NV_ENC_SUCCESS) {
            cu->cuCtxPopCurrent(NULL);
            surface->status = VASurfaceReady;
            LOG("nvEncLockBitstream failed: %s", nvencStatusStr(nvs));
            return nvencToVaStatus(nvs);
        }

        size_t copySize = lockBS.bitstreamSizeInBytes;
        nvCtx->encNeedMoreInputStreak = 0;
        if (copySize > codedBuf->codedCapacity) {
            codedBuf->codedSeg->status = VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
            copySize = codedBuf->codedCapacity;
        } else {
            codedBuf->codedSeg->status = 0;
        }
        memcpy(codedBuf->codedSeg->buf, lockBS.bitstreamBufferPtr, copySize);
        codedBuf->codedSeg->size = copySize;
        codedBuf->codedSeg->next = NULL;

        if (encDbgEnabled) {
            uint64_t bitstreamHash = 1469598103934665603ULL;
            if (lockBS.bitstreamBufferPtr != NULL && lockBS.bitstreamSizeInBytes > 0) {
                bitstreamHash = fnv1a64_append(bitstreamHash,
                                               (const uint8_t *) lockBS.bitstreamBufferPtr,
                                               (size_t) lockBS.bitstreamSizeInBytes);
            }
            LOG("ENCDBG bitstream frame=%u size=%u hash=%016llx pre_full=%016llx pre_top32=%016llx flags=0x%x",
                encDbgFrame, lockBS.bitstreamSizeInBytes,
                (unsigned long long) bitstreamHash,
                (unsigned long long) encDbgPreFullHash,
                (unsigned long long) encDbgPreTopHash,
                picParams.encodePicFlags);

            if (encDbgDump) {
                char leaf[128];
                char path[PATH_MAX];
                const char *ext = nvCtx->encIsHevc ? "h265" : "h264";
                snprintf(leaf, sizeof(leaf), "frame_%06u_bitstream.%s", encDbgFrame, ext);
                if (nvencDebugBuildPath(path, sizeof(path), leaf)) {
                    nvencDebugDumpBytes(path,
                                        (const uint8_t *) lockBS.bitstreamBufferPtr,
                                        (size_t) lockBS.bitstreamSizeInBytes);
                }
            }
        }

        nvs = drv->nvencFuncs.nvEncUnlockBitstream(nvCtx->nvencEncoder, outputBuffer);
        if (nvs != NV_ENC_SUCCESS) {
            LOG("nvEncUnlockBitstream failed: %s", nvencStatusStr(nvs));
        }

        CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

        nvCtx->encOutputFrameCount++;
        if (nvCtx->encOutputFrameCount == 1) {
            nvencHealthLog("INFO",
                           "HW encode active (%s %ux%u)",
                           nvCtx->encIsHevc ? "HEVC" : "H264",
                           (unsigned int) copyWidth, (unsigned int) copyHeight);
            nvCtx->encLastHealthLogFrame = 1;
        } else if (nvCtx->encOutputFrameCount >= nvCtx->encLastHealthLogFrame + 900U) {
            nvencHealthLog("INFO",
                           "HW encode still active (%s frames=%u)",
                           nvCtx->encIsHevc ? "HEVC" : "H264",
                           (unsigned int) nvCtx->encOutputFrameCount);
            nvCtx->encLastHealthLogFrame = nvCtx->encOutputFrameCount;
        }

        nvCtx->encFrameNum++;
        nvCtx->encForceIdr = 0;
        if (nvCtx->encStartupIdrLeft > 0) {
            nvCtx->encStartupIdrLeft--;
        }
        nvCtx->nvencIoBufferIndex = (ioSlot + 1U) % nvCtx->nvencIoBufferCount;
        surface->status = VASurfaceReady;
        return VA_STATUS_SUCCESS;
    }

    if (nvCtx->decoder == NULL) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    picParams->pBitstreamData = nvCtx->bitstreamBuffer.buf;
    picParams->pSliceDataOffsets = nvCtx->sliceOffsets.buf;
    nvCtx->bitstreamBuffer.size = 0;
    nvCtx->sliceOffsets.size = 0;

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    CUresult result = cv->cuvidDecodePicture(nvCtx->decoder, picParams);
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    VAStatus status = VA_STATUS_SUCCESS;

    if (result != CUDA_SUCCESS) {
        LOG("cuvidDecodePicture failed: %d", result);
        status = VA_STATUS_ERROR_DECODING_ERROR;
    }
    //LOG("Decoded frame successfully to idx: %d (%p)", picParams->CurrPicIdx, nvCtx->renderTarget);

    NVSurface *surface = nvCtx->renderTarget;

    surface->context = nvCtx;
    surface->topFieldFirst = !picParams->bottom_field_flag;
    surface->secondField = picParams->second_field;
    surface->decodeFailed = status != VA_STATUS_SUCCESS;
    surface->status = VASurfaceRendering;

    //TODO check we're not overflowing the queue
    pthread_mutex_lock(&nvCtx->resolveMutex);
    nvCtx->surfaceQueue[nvCtx->surfaceQueueWriteIdx++] = nvCtx->renderTarget;
    if (nvCtx->surfaceQueueWriteIdx >= SURFACE_QUEUE_SIZE) {
        nvCtx->surfaceQueueWriteIdx = 0;
    }
    pthread_mutex_unlock(&nvCtx->resolveMutex);

    //Wake up the resolve thread
    pthread_cond_signal(&nvCtx->resolveCondition);

    return status;
}

static VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);

    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    //LOG("Syncing on surface: %d (%p)", surface->pictureIdx, surface);

    //wait for resolve to occur before synchronising
    pthread_mutex_lock(&surface->mutex);
    while (surface->resolving) {
        //LOG("Surface %d not resolved, waiting", surface->pictureIdx);
        pthread_cond_wait(&surface->cond, &surface->mutex);
    }
    pthread_mutex_unlock(&surface->mutex);

    //LOG("Surface %d resolved (%p)", surface->pictureIdx, surface);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surface = getObjectPtr(drv, OBJECT_TYPE_SURFACE, render_target);
    if (!surface) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (status) {
        *status = surface->status;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySurfaceError(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VAStatus error_status,
        void **error_info /*out*/
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    //LOG("In %s", __func__);

    *num_formats = 0;
    for (unsigned int i = NV_FORMAT_NONE + 1; i < ARRAY_SIZE(formatsInfo); i++) {
        if (formatsInfo[i].is16bits && !drv->supports16BitSurface) {
            continue;
        }
        if (formatsInfo[i].isYuv444 && !drv->supports444Surface) {
            continue;
        }
        format_list[(*num_formats)++] = formatsInfo[i].vaFormat;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateImage(
        VADriverContextP ctx,
        VAImageFormat *format,
        int width,
        int height,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVFormat nvFormat = nvFormatFromVaFormat(format->fourcc);
    const NVFormatInfo *fmtInfo = &formatsInfo[nvFormat];
    const NVFormatPlane *p = fmtInfo->plane;

    if (nvFormat == NV_FORMAT_NONE) {
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    image->image_id = imageObj->id;

    //LOG("created image id: %d", imageObj->id);

    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = nvFormat;

    //allocate buffer to hold image when we copy down from the GPU
    //TODO could probably put these in a pool, they appear to be allocated, used, then freed
    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->size = 0;
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        imageBuffer->size += ((width * height) >> (p[i].ss.x + p[i].ss.y)) * fmtInfo->bppc * p[i].channelCount;
    }
    imageBuffer->elements = 1;
    imageBuffer->ptr = memalign(16, imageBuffer->size);
    imageBuffer->ownsPtr = true;

    img->imageBuffer = imageBuffer;

    memcpy(&image->format, format, sizeof(VAImageFormat));
    image->buf = imageBufferObject->id;	/* image data buffer */
    /*
     * Image data will be stored in a buffer of type VAImageBufferType to facilitate
     * data store on the server side for optimal performance. The buffer will be
     * created by the CreateImage function, and proper storage allocated based on the image
     * size and format. This buffer is managed by the library implementation, and
     * accessed by the client through the buffer Map/Unmap functions.
     */
    image->width = width;
    image->height = height;
    image->data_size = imageBuffer->size;
    image->num_planes = fmtInfo->numPlanes;	/* can not be greater than 3 */
    /*
     * An array indicating the scanline pitch in bytes for each plane.
     * Each plane may have a different pitch. Maximum 3 planes for planar formats
     */
    image->pitches[0] = width * fmtInfo->bppc;
    image->pitches[1] = width * fmtInfo->bppc;
    image->pitches[2] = width * fmtInfo->bppc;
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    image->offsets[0] = 0;
    image->offsets[1] = image->offsets[0] + ((width * height) >> (p[0].ss.x + p[0].ss.y)) * fmtInfo->bppc * p[0].channelCount;
    image->offsets[2] = image->offsets[1] + ((width * height) >> (p[1].ss.x + p[1].ss.y)) * fmtInfo->bppc * p[1].channelCount;

    img->numPlanes = image->num_planes;
    memcpy(img->pitches, image->pitches, sizeof(img->pitches));
    memcpy(img->offsets, image->offsets, sizeof(img->offsets));

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!ensureSurfaceHostBuffer(surfaceObj)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->elements = 1;
    imageBuffer->size = surfaceObj->hostTotalSize;
    imageBuffer->ptr = surfaceObj->hostData;
    imageBuffer->ownsPtr = false;
    imageBuffer->surfaceId = surface;
    imageBuffer->commitOnUnmap = 0;

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    NVImage *img = (NVImage*) imageObj->obj;
    img->width = surfaceObj->width;
    img->height = surfaceObj->height;
    img->format = NV_FORMAT_NV12;
    img->imageBuffer = imageBuffer;

    memset(image, 0, sizeof(*image));
    image->image_id = imageObj->id;
    image->buf = imageBufferObject->id;
    image->format = formatsInfo[NV_FORMAT_NV12].vaFormat;
    image->width = surfaceObj->width;
    image->height = surfaceObj->height;
    image->data_size = surfaceObj->hostTotalSize;
    image->num_planes = 2;
    image->pitches[0] = surfaceObj->hostPitch;
    image->pitches[1] = surfaceObj->hostPitch;
    image->offsets[0] = 0;
    image->offsets[1] = surfaceObj->hostYSize;
    image->offsets[2] = 0;

    img->numPlanes = image->num_planes;
    memcpy(img->pitches, image->pitches, sizeof(img->pitches));
    memcpy(img->offsets, image->offsets, sizeof(img->offsets));

    return VA_STATUS_SUCCESS;
}

static VAStatus nvDestroyImage(
        VADriverContextP ctx,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVImage *img = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (img == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    Object imageBufferObj = getObjectByPtr(drv, OBJECT_TYPE_BUFFER, img->imageBuffer);

    if (imageBufferObj != NULL) {
        if (img->imageBuffer->ptr != NULL && img->imageBuffer->ownsPtr) {
            free(img->imageBuffer->ptr);
        }

        deleteObject(drv, imageBufferObj->id);
    }

    deleteObject(drv, image);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvSetImagePalette(
            VADriverContextP ctx,
            VAImageID image,
            /*
                 * pointer to an array holding the palette data.  The size of the array is
                 * num_palette_entries * entry_bytes in size.  The order of the components
                 * in the palette is described by the component_order in VAImage struct
                 */
                unsigned char *palette
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvGetImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        int x,     /* coordinates of the upper left source pixel */
        int y,
        unsigned int width, /* width and height of the region */
        unsigned int height,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);

    if (surfaceObj == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (imageObj == NULL) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    NVContext *context = (NVContext*) surfaceObj->context;
    const NVFormatInfo *fmtInfo = &formatsInfo[imageObj->format];
    uint32_t offset = 0;

    if (context == NULL || context->mode == NV_CONTEXT_ENCODE) {
        if (!surfaceObj->hostData || !imageObj->imageBuffer || !imageObj->imageBuffer->ptr) {
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        if (x < 0 || y < 0) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if ((uint32_t) x >= surfaceObj->width || (uint32_t) y >= surfaceObj->height) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        uint8_t *dst = (uint8_t *) imageObj->imageBuffer->ptr;
        uint32_t dstPitchY = imageObj->pitches[0] ? imageObj->pitches[0] : imageObj->width;
        uint32_t dstPitchUV = imageObj->pitches[1] ? imageObj->pitches[1] : dstPitchY;
        uint32_t dstOffsetY = imageObj->offsets[0];
        uint32_t dstOffsetUV = imageObj->offsets[1];

        uint32_t copyW = width;
        uint32_t copyH = height;
        if ((uint32_t) x + copyW > surfaceObj->width) {
            copyW = surfaceObj->width - (uint32_t) x;
        }
        if ((uint32_t) y + copyH > surfaceObj->height) {
            copyH = surfaceObj->height - (uint32_t) y;
        }
        if (copyW > imageObj->width) {
            copyW = imageObj->width;
        }
        if (copyH > imageObj->height) {
            copyH = imageObj->height;
        }

        pthread_mutex_lock(&surfaceObj->mutex);
        const uint8_t *srcY = surfaceObj->hostData + ((size_t) y * surfaceObj->hostPitch) + (size_t) x;
        const uint8_t *srcUV = surfaceObj->hostData + surfaceObj->hostYSize +
                               ((size_t) (y / 2) * surfaceObj->hostPitch) + (size_t) (x & ~1);

        for (uint32_t row = 0; row < copyH; row++) {
            memcpy(dst + dstOffsetY + ((size_t) row * dstPitchY),
                   srcY + ((size_t) row * surfaceObj->hostPitch),
                   copyW);
        }

        uint32_t uvWidth = copyW & ~1U;
        uint32_t uvHeight = copyH / 2U;
        for (uint32_t row = 0; row < uvHeight; row++) {
            memcpy(dst + dstOffsetUV + ((size_t) row * dstPitchUV),
                   srcUV + ((size_t) row * surfaceObj->hostPitch),
                   uvWidth);
        }
        pthread_mutex_unlock(&surfaceObj->mutex);
        return VA_STATUS_SUCCESS;
    }

    //wait for the surface to be decoded
    nvSyncSurface(ctx, surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    for (uint32_t i = 0; i < fmtInfo->numPlanes; i++) {
        const NVFormatPlane *p = &fmtInfo->plane[i];
        CUDA_MEMCPY2D memcpy2d = {
        .srcXInBytes = 0, .srcY = 0,
        .srcMemoryType = CU_MEMORYTYPE_ARRAY,
        .srcArray = surfaceObj->backingImage->arrays[i],

        .dstXInBytes = 0, .dstY = 0,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = (char *)imageObj->imageBuffer->ptr + offset,
        .dstPitch = width * fmtInfo->bppc,

        .WidthInBytes = (width >> p->ss.x) * fmtInfo->bppc * p->channelCount,
        .Height = height >> p->ss.y
        };

        CUresult result = cu->cuMemcpy2D(&memcpy2d);
        if (result != CUDA_SUCCESS) {
            LOG("cuMemcpy2D failed: %d", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        offset += ((width * height) >> (p->ss.x + p->ss.y)) * fmtInfo->bppc * p->channelCount;
    }
    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surfaceObj = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    NVImage *imageObj = (NVImage*) getObjectPtr(drv, OBJECT_TYPE_IMAGE, image);
    if (!surfaceObj) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!imageObj || !imageObj->imageBuffer || !imageObj->imageBuffer->ptr) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    if (!ensureSurfaceHostBuffer(surfaceObj)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    if (src_width == 0 || src_height == 0 || dest_width == 0 || dest_height == 0) {
        surfaceObj->status = VASurfaceReady;
        return VA_STATUS_SUCCESS;
    }
    if (src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((uint32_t) src_x >= imageObj->width || (uint32_t) src_y >= imageObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if ((uint32_t) dest_x >= surfaceObj->width || (uint32_t) dest_y >= surfaceObj->height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    uint8_t *srcBase = (uint8_t *) imageObj->imageBuffer->ptr;
    uint32_t srcPitchY = imageObj->pitches[0] ? imageObj->pitches[0] : imageObj->width;
    uint32_t srcPitchUV = imageObj->pitches[1] ? imageObj->pitches[1] : srcPitchY;
    uint32_t srcOffsetY = imageObj->offsets[0];
    uint32_t srcOffsetUV = imageObj->offsets[1];

    uint32_t copyW = src_width < dest_width ? src_width : dest_width;
    uint32_t copyH = src_height < dest_height ? src_height : dest_height;

    if ((uint32_t) src_x + copyW > imageObj->width) {
        copyW = imageObj->width - (uint32_t) src_x;
    }
    if ((uint32_t) src_y + copyH > imageObj->height) {
        copyH = imageObj->height - (uint32_t) src_y;
    }
    if ((uint32_t) dest_x + copyW > surfaceObj->width) {
        copyW = surfaceObj->width - (uint32_t) dest_x;
    }
    if ((uint32_t) dest_y + copyH > surfaceObj->height) {
        copyH = surfaceObj->height - (uint32_t) dest_y;
    }

    pthread_mutex_lock(&surfaceObj->mutex);
    const uint8_t *srcY = srcBase + srcOffsetY + ((size_t) src_y * srcPitchY) + (size_t) src_x;
    uint8_t *dstY = surfaceObj->hostData + ((size_t) dest_y * surfaceObj->hostPitch) + (size_t) dest_x;

    for (uint32_t row = 0; row < copyH; row++) {
        memcpy(dstY + ((size_t) row * surfaceObj->hostPitch),
               srcY + ((size_t) row * srcPitchY),
               copyW);
    }

    uint32_t srcXUV = (uint32_t) src_x & ~1U;
    uint32_t dstXUV = (uint32_t) dest_x & ~1U;
    uint32_t uvWidth = copyW & ~1U;
    uint32_t uvHeight = copyH / 2U;
    const uint8_t *srcUV = srcBase + srcOffsetUV +
                           ((size_t) ((uint32_t) src_y / 2U) * srcPitchUV) + (size_t) srcXUV;
    uint8_t *dstUV = surfaceObj->hostData + surfaceObj->hostYSize +
                     ((size_t) ((uint32_t) dest_y / 2U) * surfaceObj->hostPitch) + (size_t) dstXUV;

    for (uint32_t row = 0; row < uvHeight; row++) {
        memcpy(dstUV + ((size_t) row * surfaceObj->hostPitch),
               srcUV + ((size_t) row * srcPitchUV),
               uvWidth);
    }

    if (copyW > 0 && copyH > 0) {
        uint32_t rx = (uint32_t) dest_x;
        uint32_t ry = (uint32_t) dest_y;
        uint32_t rw = copyW;
        uint32_t rh = copyH;

        if (!surfaceObj->contentValid) {
            surfaceObj->contentX = rx;
            surfaceObj->contentY = ry;
            surfaceObj->contentWidth = rw;
            surfaceObj->contentHeight = rh;
            surfaceObj->contentValid = 1;
        } else {
            uint32_t x0 = surfaceObj->contentX < rx ? surfaceObj->contentX : rx;
            uint32_t y0 = surfaceObj->contentY < ry ? surfaceObj->contentY : ry;
            uint32_t x1a = surfaceObj->contentX + surfaceObj->contentWidth;
            uint32_t y1a = surfaceObj->contentY + surfaceObj->contentHeight;
            uint32_t x1b = rx + rw;
            uint32_t y1b = ry + rh;
            uint32_t x1 = x1a > x1b ? x1a : x1b;
            uint32_t y1 = y1a > y1b ? y1a : y1b;
            if (x1 > surfaceObj->width) {
                x1 = surfaceObj->width;
            }
            if (y1 > surfaceObj->height) {
                y1 = surfaceObj->height;
            }
            surfaceObj->contentX = x0;
            surfaceObj->contentY = y0;
            surfaceObj->contentWidth = x1 > x0 ? (x1 - x0) : 0;
            surfaceObj->contentHeight = y1 > y0 ? (y1 - y0) : 0;
            surfaceObj->contentValid = (surfaceObj->contentWidth > 0 && surfaceObj->contentHeight > 0) ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&surfaceObj->mutex);

    surfaceObj->status = VASurfaceReady;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    LOG("In %s", __func__);
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureChromakey(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        unsigned int chromakey_min,
        unsigned int chromakey_max,
        unsigned int chromakey_mask
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvAssociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces,
        short src_x, /* upper left offset in subpicture */
        short src_y,
        unsigned short src_width,
        unsigned short src_height,
        short dest_x, /* upper left offset in surface */
        short dest_y,
        unsigned short dest_width,
        unsigned short dest_height,
        /*
         * whether to enable chroma-keying or global-alpha
         * see VA_SUBPICTURE_XXX values
         */
        unsigned int flags
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    LOG("In %s", __func__);
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQuerySurfaceAttributes(
        VADriverContextP    ctx,
	    VAConfigID          config,
	    VASurfaceAttrib    *attrib_list,
	    unsigned int       *num_attribs
	)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObjectPtr(drv, OBJECT_TYPE_CONFIG, config);

    if (cfg == NULL) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->isEncode) {
        const unsigned int availableAttribs = 6;
        if (num_attribs == NULL) {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        unsigned int maxAttribs = *num_attribs;
        if (!attrib_list) {
            *num_attribs = availableAttribs;
            return VA_STATUS_SUCCESS;
        }

        unsigned int i = 0;
        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribPixelFormat;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_FOURCC_NV12;
            i++;
        }

        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribMemoryType;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
            i++;
        }

        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribMinWidth;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = 64;
            i++;
        }

        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribMaxWidth;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = 8192;
            i++;
        }

        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribMinHeight;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = 64;
            i++;
        }

        if (i < maxAttribs) {
            attrib_list[i].type = VASurfaceAttribMaxHeight;
            attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
            attrib_list[i].value.type = VAGenericValueTypeInteger;
            attrib_list[i].value.value.i = 8192;
            i++;
        }

        *num_attribs = i;
        return VA_STATUS_SUCCESS;
    }

    //LOG("with %d (%d) %p %d", cfg->cudaCodec, cfg->bitDepth, attrib_list, *num_attribs);

    if (cfg->chromaFormat != cudaVideoChromaFormat_420 && cfg->chromaFormat != cudaVideoChromaFormat_444) {
        //TODO not sure what pixel formats are needed for 422 formats
        LOG("Unknown chrome format: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if ((cfg->chromaFormat == cudaVideoChromaFormat_444 || cfg->surfaceFormat == cudaVideoSurfaceFormat_YUV444_16Bit) && !drv->supports444Surface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("YUV444 surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (cfg->surfaceFormat == cudaVideoSurfaceFormat_P016 && !drv->supports16BitSurface) {
        //TODO not sure what pixel formats are needed for 422 and 444 formats
        LOG("16 bits surfaces not supported: %d", cfg->chromaFormat);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    if (num_attribs == NULL) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    int availableAttribs = 4;
    if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
        availableAttribs += 1;
#if VA_CHECK_VERSION(1, 20, 0)
        availableAttribs += 1;
#endif
    } else {
        availableAttribs += 1;
        if (drv->supports16BitSurface) {
            availableAttribs += 3;
        }
    }

    unsigned int maxAttribs = *num_attribs;
    if (attrib_list == NULL) {
        *num_attribs = (unsigned int) availableAttribs;
        return VA_STATUS_SUCCESS;
    }

    CUVIDDECODECAPS videoDecodeCaps = {
        .eCodecType      = cfg->cudaCodec,
        .eChromaFormat   = cfg->chromaFormat,
        .nBitDepthMinus8 = cfg->bitDepth - 8
    };
    bool decodeCapsValid = true;
    if (checkCudaErrors(cu->cuCtxPushCurrent(drv->cudaContext), __FILE__, __func__, __LINE__)) {
        decodeCapsValid = false;
    } else {
        if (checkCudaErrors(cv->cuvidGetDecoderCaps(&videoDecodeCaps), __FILE__, __func__, __LINE__)) {
            decodeCapsValid = false;
        }
        if (checkCudaErrors(cu->cuCtxPopCurrent(NULL), __FILE__, __func__, __LINE__)) {
            decodeCapsValid = false;
        }
    }
    if (!decodeCapsValid || videoDecodeCaps.nMaxWidth == 0 || videoDecodeCaps.nMaxHeight == 0) {
        videoDecodeCaps.nMinWidth = 16;
        videoDecodeCaps.nMinHeight = 16;
        videoDecodeCaps.nMaxWidth = 8192;
        videoDecodeCaps.nMaxHeight = 8192;
    }

    unsigned int out = 0;
#define WRITE_SURF_ATTR(_type, _value)                                       \
    do {                                                                     \
        if (out < maxAttribs) {                                              \
            attrib_list[out].type = (_type);                                 \
            attrib_list[out].flags = 0;                                      \
            attrib_list[out].value.type = VAGenericValueTypeInteger;         \
            attrib_list[out].value.value.i = (_value);                       \
            out++;                                                            \
        }                                                                    \
    } while (0)

    WRITE_SURF_ATTR(VASurfaceAttribMinWidth, videoDecodeCaps.nMinWidth);
    WRITE_SURF_ATTR(VASurfaceAttribMinHeight, videoDecodeCaps.nMinHeight);
    WRITE_SURF_ATTR(VASurfaceAttribMaxWidth, videoDecodeCaps.nMaxWidth);
    WRITE_SURF_ATTR(VASurfaceAttribMaxHeight, videoDecodeCaps.nMaxHeight);

    if (cfg->chromaFormat == cudaVideoChromaFormat_444) {
        WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_444P);
#if VA_CHECK_VERSION(1, 20, 0)
        WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_Q416);
#endif
    } else {
        WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_NV12);
        if (drv->supports16BitSurface) {
            WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_P010);
            WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_P012);
            WRITE_SURF_ATTR(VASurfaceAttribPixelFormat, VA_FOURCC_P016);
        }
    }
#undef WRITE_SURF_ATTR

    *num_attribs = out;
    return VA_STATUS_SUCCESS;
}

/* used by va trace */
static VAStatus nvBufferInfo(
           VADriverContextP ctx,      /* in */
           VABufferID buf_id,         /* in */
           VABufferType *type,        /* out */
           unsigned int *size,        /* out */
           unsigned int *num_elements /* out */
)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVBuffer *buf = (NVBuffer*) getObjectPtr(drv, OBJECT_TYPE_BUFFER, buf_id);
    if (!buf) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (type) {
        *type = buf->bufferType;
    }
    if (size) {
        *size = buf->size;
    }
    if (num_elements) {
        *num_elements = buf->elements;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* lock/unlock surface for external access */
static VAStatus nvLockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* out  for follow argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name, /* if it is not NULL, assign the low lever
                                    * surface buffer name
                                    */
        void **buffer /* if it is not NULL, map the surface buffer for
                       * CPU access
                       */
)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    if (!surf) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!ensureSurfaceHostBuffer(surf)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (fourcc) *fourcc = VA_FOURCC_NV12;
    if (luma_stride) *luma_stride = (unsigned int) surf->hostPitch;
    if (chroma_u_stride) *chroma_u_stride = (unsigned int) surf->hostPitch;
    if (chroma_v_stride) *chroma_v_stride = (unsigned int) surf->hostPitch;
    if (luma_offset) *luma_offset = 0;
    if (chroma_u_offset) *chroma_u_offset = (unsigned int) surf->hostYSize;
    if (chroma_v_offset) *chroma_v_offset = (unsigned int) (surf->hostYSize + 1);
    if (buffer_name) *buffer_name = 0;
    if (buffer) *buffer = surf->hostData;
    return VA_STATUS_SUCCESS;
}

static VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVSurface *surf = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface);
    if (!surf) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
static VAStatus nvCreateBuffer2(
            VADriverContextP ctx,
            VAContextID context,                /* in */
            VABufferType type,                  /* in */
            unsigned int width,                 /* in */
            unsigned int height,                /* in */
            unsigned int *unit_size,            /* out */
            unsigned int *pitch,                /* out */
            VABufferID *buf_id                  /* out */
    )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    LOG("In %s", __func__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus nvExportSurfaceHandle(
            VADriverContextP    ctx,
            VASurfaceID         surface_id,     /* in */
            uint32_t            mem_type,       /* in */
            uint32_t            flags,          /* in */
            void               *descriptor      /* out */
)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    if ((mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) == 0) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) == 0) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    NVSurface *surface = (NVSurface*) getObjectPtr(drv, OBJECT_TYPE_SURFACE, surface_id);
    if (surface == NULL) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    //LOG("Exporting surface: %d (%p)", surface->pictureIdx, surface);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    if (!drv->backend->realiseSurface(drv, surface)) {
        LOG("Unable to export surface");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;

    drv->backend->fillExportDescriptor(drv, surface, ptr);

    // LOG("Exporting with w:%d h:%d o:%d p:%d m:%" PRIx64 " o:%d p:%d m:%" PRIx64, ptr->width, ptr->height, ptr->layers[0].offset[0],
    //                                                             ptr->layers[0].pitch[0], ptr->objects[0].drm_format_modifier,
    //                                                             ptr->layers[1].offset[0], ptr->layers[1].pitch[0],
    //                                                             ptr->objects[1].drm_format_modifier);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    return VA_STATUS_SUCCESS;
}

static VAStatus nvTerminate( VADriverContextP ctx )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("Terminating %p", ctx);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPushCurrent(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    drv->backend->destroyAllBackingImage(drv);

    deleteAllObjects(drv);

    drv->backend->releaseExporter(drv);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxPopCurrent(NULL), VA_STATUS_ERROR_OPERATION_FAILED);

    pthread_mutex_lock(&concurrency_mutex);
    instances--;
    LOG("Now have %d (%d max) instances", instances, max_instances);
    pthread_mutex_unlock(&concurrency_mutex);

    CHECK_CUDA_RESULT_RETURN(cu->cuCtxDestroy(drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);
    drv->cudaContext = NULL;
    if (drv->nvencLib) {
        dlclose(drv->nvencLib);
        drv->nvencLib = NULL;
    }

    free(drv);

    return VA_STATUS_SUCCESS;
}

extern const NVBackend DIRECT_BACKEND;
extern const NVBackend EGL_BACKEND;

#define VTABLE(func) .va ## func = &nv ## func
static const struct VADriverVTable vtable = {
    VTABLE(Terminate),
    VTABLE(QueryConfigProfiles),
    VTABLE(QueryConfigEntrypoints),
    VTABLE(QueryConfigAttributes),
    VTABLE(CreateConfig),
    VTABLE(DestroyConfig),
    VTABLE(GetConfigAttributes),
    VTABLE(CreateSurfaces),
    VTABLE(CreateSurfaces2),
    VTABLE(DestroySurfaces),
    VTABLE(CreateContext),
    VTABLE(DestroyContext),
    VTABLE(CreateBuffer),
    VTABLE(BufferSetNumElements),
    VTABLE(MapBuffer),
    VTABLE(UnmapBuffer),
    VTABLE(DestroyBuffer),
    VTABLE(BeginPicture),
    VTABLE(RenderPicture),
    VTABLE(EndPicture),
    VTABLE(SyncSurface),
    VTABLE(QuerySurfaceStatus),
    VTABLE(QuerySurfaceError),
    VTABLE(PutSurface),
    VTABLE(QueryImageFormats),
    VTABLE(CreateImage),
    VTABLE(DeriveImage),
    VTABLE(DestroyImage),
    VTABLE(SetImagePalette),
    VTABLE(GetImage),
    VTABLE(PutImage),
    VTABLE(QuerySubpictureFormats),
    VTABLE(CreateSubpicture),
    VTABLE(DestroySubpicture),
    VTABLE(SetSubpictureImage),
    VTABLE(SetSubpictureChromakey),
    VTABLE(SetSubpictureGlobalAlpha),
    VTABLE(AssociateSubpicture),
    VTABLE(DeassociateSubpicture),
    VTABLE(QueryDisplayAttributes),
    VTABLE(GetDisplayAttributes),
    VTABLE(SetDisplayAttributes),
    VTABLE(QuerySurfaceAttributes),
    VTABLE(BufferInfo),
    VTABLE(AcquireBufferHandle),
    VTABLE(ReleaseBufferHandle),
    VTABLE(LockSurface),
    VTABLE(UnlockSurface),
    VTABLE(CreateMFContext),
    VTABLE(MFAddContext),
    VTABLE(MFReleaseContext),
    VTABLE(MFSubmit),
    VTABLE(CreateBuffer2),
    VTABLE(QueryProcessingRate),
    VTABLE(ExportSurfaceHandle),
};

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);

__attribute__((visibility("default")))
VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    LOG("Initialising NVIDIA VA-API Driver");

    //drm_state can be passed in with any display type, including X11. But if it's X11, we don't
    //want to use the fd as it'll likely be an Intel GPU, as NVIDIA doesn't support DRI3 at the moment
    bool isDrm = ctx->drm_state != NULL && ((struct drm_state*) ctx->drm_state)->fd > 0;
    int drmFd = (gpu == -1 && isDrm) ? ((struct drm_state*) ctx->drm_state)->fd : -1;

    //check if the drmFd is actually an nvidia one
    LOG("Got DRM FD: %d %d", isDrm, drmFd)
    if (drmFd != -1) {
        if (!isNvidiaDrmFd(drmFd, true)) {
            LOG("Passed in DRM FD does not belong to the NVIDIA driver, ignoring");
            drmFd = -1;
        } else if (!checkModesetParameterFromFd(drmFd)) {
            //we have an NVIDIA fd but no modeset (which means no DMA-BUF support)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    pthread_mutex_lock(&concurrency_mutex);
    LOG("Now have %d (%d max) instances", instances, max_instances);
    if (max_instances > 0 && instances >= max_instances) {
        pthread_mutex_unlock(&concurrency_mutex);
        return VA_STATUS_ERROR_HW_BUSY;
    }
    instances++;
    pthread_mutex_unlock(&concurrency_mutex);

    //check to make sure we initialised the CUDA functions correctly
    if (cu == NULL || cv == NULL) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    ctx->pDriverData = drv;

    drv->cu = cu;
    drv->cv = cv;
    drv->useCorrectNV12Format = true;
    drv->cudaGpuId = gpu;
    //make sure that we want the default GPU, and that a DRM fd that we care about is passed in
    drv->drmFd = drmFd;

    if (backend == EGL) {
        LOG("Selecting EGL backend");
        drv->backend = &EGL_BACKEND;
    } else if (backend == DIRECT) {
        LOG("Selecting Direct backend");
        drv->backend = &DIRECT_BACKEND;
    }

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 3;
    ctx->max_attributes = 16;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = ARRAY_SIZE(formatsInfo) - 1;
    ctx->max_subpic_formats = 1;

    pthread_mutexattr_t attrib;
    pthread_mutexattr_init(&attrib);
    pthread_mutexattr_settype(&attrib, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&drv->objectCreationMutex, &attrib);
    pthread_mutex_init(&drv->imagesMutex, &attrib);
    pthread_mutex_init(&drv->exportMutex, NULL);

    if (!drv->backend->initExporter(drv)) {
        LOG("Exporter failed");
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (CHECK_CUDA_RESULT(cu->cuCtxCreate(&drv->cudaContext, CU_CTX_SCHED_BLOCKING_SYNC, drv->cudaGpuId))) {
        drv->backend->releaseExporter(drv);
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!loadNvencFunctions(drv)) {
        LOG("NVENC unavailable, continuing in decode-only mode");
    }

    if (backend == DIRECT) {
        ctx->str_vendor = drv->nvencAvailable ?
            "VA-API NVDEC/NVENC driver [direct backend]" :
            "VA-API NVDEC driver [direct backend]";
    } else if (backend == EGL) {
        ctx->str_vendor = drv->nvencAvailable ?
            "VA-API NVDEC/NVENC driver [egl backend]" :
            "VA-API NVDEC driver [egl backend]";
    }

    //CHECK_CUDA_RESULT_RETURN(cv->cuvidCtxLockCreate(&drv->vidLock, drv->cudaContext), VA_STATUS_ERROR_OPERATION_FAILED);

    VAStatus profileStatus = nvQueryConfigProfiles2(ctx, drv->profiles, &drv->profileCount);
    if (g_disableDecoder || profileStatus != VA_STATUS_SUCCESS) {
        drv->profileCount = 0;
    }

    if (drv->nvencAvailable) {
        appendProfileIfMissing(drv, VAProfileH264ConstrainedBaseline);
        appendProfileIfMissing(drv, VAProfileH264Main);
        appendProfileIfMissing(drv, VAProfileH264High);
        appendProfileIfMissing(drv, VAProfileHEVCMain);
        appendProfileIfMissing(drv, VAProfileHEVCMain10);
    }

    *ctx->vtable = vtable;
    return VA_STATUS_SUCCESS;
}
