/* nvcuda-lite: minimal nvcuda.dll for Wine/Proton, for NVIDIA NGX / DLSS-G device discovery.
 * NGX core (_nvngx.dll) and the DLSS-G 310.x runtime only ask CUDA for device enumeration,
 * the compute capability and the adapter LUID. Everything compute-related goes through
 * NVAPI's D3D12 cubin path (dxvk-nvapi -> vkd3d-proton -> VK_NVX_binary_import), so no
 * real CUDA context is ever needed here.
 *   NVCUDA_LITE_CC=8.9     compute capability to report (default: 8.6)
 *   NVCUDA_LITE_LOG=path   append a log of every call (Windows path, e.g. Z:\home\me\nvcuda.log)
 *   NVCUDA_LITE_LUID=hex   16 hex digits, override the LUID (default: queried from Vulkan)
 * License: MIT. Part of dlssg-open.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
#define CUDA_SUCCESS 0
#define CUDA_ERROR_INVALID_VALUE 1
#define CUDA_ERROR_NOT_INITIALIZED 3
#define CUDA_ERROR_INVALID_DEVICE 101
#define CUDA_ERROR_INVALID_CONTEXT 201
#define CUDA_ERROR_NOT_FOUND 500
#define CUDA_ERROR_NOT_SUPPORTED 801

static FILE *g_log;
static int g_inited;
static int g_cc_major = 8, g_cc_minor = 6;
static uint64_t g_luid;
static int g_luid_valid;
static char g_name[256] = "NVIDIA GeForce RTX 3080";
static uint8_t g_uuid[16];
static int g_uuid_valid;
static CUcontext g_ctx = (CUcontext)0x4E564355; /* fake, non-null */

#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

/* ---- Vulkan LUID / name query (winevulkan supplies LUIDs that DXGI/DXVK also use) ---- */
typedef struct { uint32_t sType; void *pNext; } VkBase;
static void query_vulkan(void)
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { LOG("vulkan-1.dll not available, LUID stays invalid"); return; }
    typedef void *(WINAPI *PFN_gipa)(void *, const char *);
    PFN_gipa gipa = (PFN_gipa)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) return;
    typedef int (WINAPI *PFN_ci)(const void *, const void *, void **);
    typedef int (WINAPI *PFN_epd)(void *, uint32_t *, void **);
    typedef void (WINAPI *PFN_gpdp2)(void *, void *);
    typedef void (WINAPI *PFN_di)(void *, const void *);
    PFN_ci vkCreateInstance = (PFN_ci)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) return;
    struct { uint32_t sType; const void *pNext; const char *app; uint32_t av; const char *eng; uint32_t ev; uint32_t api; } ai =
        { 0 /*VK_STRUCTURE_TYPE_APPLICATION_INFO*/, NULL, "nvcuda-lite", 1, "nvcuda-lite", 1, (1u << 22) | (1u << 12) };
    struct { uint32_t sType; const void *pNext; uint32_t flags; const void *ai; uint32_t lc; const char *const *ln; uint32_t ec; const char *const *en; } ci =
        { 1 /*VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO*/, NULL, 0, &ai, 0, NULL, 0, NULL };
    void *inst = NULL;
    if (vkCreateInstance(&ci, NULL, &inst) != 0 || !inst) { LOG("vkCreateInstance failed"); return; }
    PFN_epd epd = (PFN_epd)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_gpdp2 gpdp2 = (PFN_gpdp2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    PFN_di di = (PFN_di)gipa(inst, "vkDestroyInstance");
    uint32_t n = 0; void *devs[16];
    if (epd && gpdp2 && epd(inst, &n, NULL) == 0 && n) {
        if (n > 16) n = 16;
        epd(inst, &n, devs);
        for (uint32_t i = 0; i < n; i++) {
            /* VkPhysicalDeviceIDProperties */
            struct { uint32_t sType; void *pNext; uint8_t deviceUUID[16]; uint8_t driverUUID[16]; uint8_t deviceLUID[8]; uint32_t deviceNodeMask; uint32_t deviceLUIDValid; } idp;
            memset(&idp, 0, sizeof(idp)); idp.sType = 1000071004; /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES */
            /* VkPhysicalDeviceProperties2: sType, pNext, then VkPhysicalDeviceProperties (large) */
            uint8_t buf[2048]; memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = 1000059001; /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 */
            *(void **)(buf + 8) = &idp;
            gpdp2(devs[i], buf);
            /* VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType, deviceName[256] at +16+16 */
            uint32_t vendor = *(uint32_t *)(buf + 16 + 8);
            const char *name = (const char *)(buf + 16 + 20);
            LOG("vulkan device %u: vendor=%#x name='%s' luidValid=%u", i, vendor, name, idp.deviceLUIDValid);
            if (vendor == 0x10DE) {
                strncpy(g_name, name, sizeof(g_name) - 1);
                memcpy(g_uuid, idp.deviceUUID, 16); g_uuid_valid = 1;
                if (idp.deviceLUIDValid) { memcpy(&g_luid, idp.deviceLUID, 8); g_luid_valid = 1; }
                break;
            }
        }
    }
    if (di) di(inst, NULL);
}

static void init_once(void)
{
    static int done; if (done) return; done = 1;
    const char *lp = getenv("NVCUDA_LITE_LOG");
    if (lp && *lp) g_log = fopen(lp, "a");
    LOG("---- nvcuda-lite loaded (pid %lu) ----", GetCurrentProcessId());
    const char *cc = getenv("NVCUDA_LITE_CC");
    if (cc && sscanf(cc, "%d.%d", &g_cc_major, &g_cc_minor) != 2) { g_cc_major = 8; g_cc_minor = 6; }
    query_vulkan();
    const char *lu = getenv("NVCUDA_LITE_LUID");
    if (lu && strlen(lu) == 16) { g_luid = strtoull(lu, NULL, 16); g_luid_valid = 1; }
    LOG("reporting: name='%s' cc=%d.%d luid=%016llx (valid=%d)", g_name, g_cc_major, g_cc_minor, (unsigned long long)g_luid, g_luid_valid);
}

#define API __declspec(dllexport) CUresult WINAPI

API cuInit(unsigned flags) { init_once(); g_inited = 1; LOG("cuInit(%u)", flags); return CUDA_SUCCESS; }
API cuDriverGetVersion(int *v) { init_once(); if (v) *v = 13000; LOG("cuDriverGetVersion -> 13000"); return CUDA_SUCCESS; }
API cuDeviceGetCount(int *c) { init_once(); if (c) *c = 1; LOG("cuDeviceGetCount -> 1"); return CUDA_SUCCESS; }
API cuDeviceGet(CUdevice *d, int ordinal) { init_once(); LOG("cuDeviceGet(%d)", ordinal); if (ordinal != 0) return CUDA_ERROR_INVALID_DEVICE; if (d) *d = 0; return CUDA_SUCCESS; }
API cuDeviceGetName(char *name, int len, CUdevice dev) { init_once(); LOG("cuDeviceGetName(dev %d)", dev); if (!name || len <= 0) return CUDA_ERROR_INVALID_VALUE; strncpy(name, g_name, len - 1); name[len - 1] = 0; return CUDA_SUCCESS; }
API cuDeviceComputeCapability(int *major, int *minor, CUdevice dev) { init_once(); if (major) *major = g_cc_major; if (minor) *minor = g_cc_minor; LOG("cuDeviceComputeCapability(dev %d) -> %d.%d", dev, g_cc_major, g_cc_minor); return CUDA_SUCCESS; }
API cuDeviceGetLuid(char *luid, unsigned *nodeMask, CUdevice dev)
{
    init_once(); LOG("cuDeviceGetLuid(dev %d) -> %016llx valid=%d", dev, (unsigned long long)g_luid, g_luid_valid);
    if (!g_luid_valid) return CUDA_ERROR_NOT_SUPPORTED;
    if (luid) memcpy(luid, &g_luid, 8); if (nodeMask) *nodeMask = 1; return CUDA_SUCCESS;
}
API cuDeviceGetUuid(void *uuid, CUdevice dev) { init_once(); LOG("cuDeviceGetUuid(dev %d)", dev); if (!g_uuid_valid) return CUDA_ERROR_NOT_SUPPORTED; memcpy(uuid, g_uuid, 16); return CUDA_SUCCESS; }
API cuDeviceGetUuid_v2(void *uuid, CUdevice dev) { return cuDeviceGetUuid(uuid, dev); }
API cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev) { init_once(); if (bytes) *bytes = (size_t)10 << 30; LOG("cuDeviceTotalMem_v2(dev %d)", dev); return CUDA_SUCCESS; }
API cuDeviceGetAttribute(int *pi, int attrib, CUdevice dev)
{
    init_once();
    int v = 0;
    switch (attrib) {
    case 75: v = g_cc_major; break;   /* CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR */
    case 76: v = g_cc_minor; break;   /* CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR */
    case 16: v = 68; break;           /* MULTIPROCESSOR_COUNT (GA102 3080) */
    case 1:  v = 1024; break;         /* MAX_THREADS_PER_BLOCK */
    case 8:  v = 49152; break;        /* MAX_SHARED_MEMORY_PER_BLOCK */
    case 10: v = 32; break;           /* WARP_SIZE */
    case 13: v = 1710000; break;      /* CLOCK_RATE kHz */
    case 33: v = 0x0A; break;         /* PCI_BUS_ID */
    case 34: v = 0; break;            /* PCI_DEVICE_ID */
    case 50: v = 0; break;            /* PCI_DOMAIN_ID */
    case 83: v = 1; break;            /* MANAGED_MEMORY */
    default: v = 0; break;
    }
    if (pi) *pi = v;
    LOG("cuDeviceGetAttribute(attr %d, dev %d) -> %d", attrib, dev, v);
    return CUDA_SUCCESS;
}
API cuGetErrorString(CUresult e, const char **s) { init_once(); static char b[64]; snprintf(b, sizeof(b), "nvcuda-lite error %d", e); if (s) *s = b; return CUDA_SUCCESS; }
API cuGetErrorName(CUresult e, const char **s) { return cuGetErrorString(e, s); }
API cuCtxGetDevice(CUdevice *d) { init_once(); LOG("cuCtxGetDevice"); if (d) *d = 0; return CUDA_SUCCESS; }
API cuCtxGetCurrent(CUcontext *c) { init_once(); LOG("cuCtxGetCurrent"); if (c) *c = g_ctx; return CUDA_SUCCESS; }
API cuCtxSetCurrent(CUcontext c) { init_once(); LOG("cuCtxSetCurrent(%p)", c); return CUDA_SUCCESS; }
API cuCtxPushCurrent_v2(CUcontext c) { init_once(); LOG("cuCtxPushCurrent_v2(%p)", c); return CUDA_SUCCESS; }
API cuCtxPopCurrent_v2(CUcontext *c) { init_once(); LOG("cuCtxPopCurrent_v2"); if (c) *c = g_ctx; return CUDA_SUCCESS; }
API cuCtxPushCurrent(CUcontext c) { return cuCtxPushCurrent_v2(c); }
API cuCtxPopCurrent(CUcontext *c) { return cuCtxPopCurrent_v2(c); }
API cuDevicePrimaryCtxRetain(CUcontext *c, CUdevice dev) { init_once(); LOG("cuDevicePrimaryCtxRetain(dev %d)", dev); if (c) *c = g_ctx; return CUDA_SUCCESS; }
API cuDevicePrimaryCtxRelease_v2(CUdevice dev) { init_once(); LOG("cuDevicePrimaryCtxRelease_v2(dev %d)", dev); return CUDA_SUCCESS; }
API cuDevicePrimaryCtxRelease(CUdevice dev) { return cuDevicePrimaryCtxRelease_v2(dev); }
API cuCtxCreate_v2(CUcontext *c, unsigned flags, CUdevice dev) { init_once(); LOG("cuCtxCreate_v2(flags %u, dev %d)", flags, dev); if (c) *c = g_ctx; return CUDA_SUCCESS; }
API cuCtxDestroy_v2(CUcontext c) { init_once(); LOG("cuCtxDestroy_v2(%p)", c); return CUDA_SUCCESS; }
API cuCtxSynchronize(void) { init_once(); LOG("cuCtxSynchronize"); return CUDA_SUCCESS; }
API cuGetExportTable(const void **t, const void *id) { init_once(); LOG("cuGetExportTable -> not supported"); return CUDA_ERROR_NOT_SUPPORTED; }
API cuGetProcAddress(const char *sym, void **pfn, int ver, uint64_t flags)
{
    init_once();
    HMODULE self = GetModuleHandleA("nvcuda.dll");
    void *p = self ? (void *)GetProcAddress(self, sym) : NULL;
    if (!p && self) { char v2[128]; snprintf(v2, sizeof(v2), "%s_v2", sym); p = (void *)GetProcAddress(self, v2); }
    LOG("cuGetProcAddress('%s', ver %d, flags %llu) -> %s", sym, ver, (unsigned long long)flags, p ? "ok" : "NOT FOUND");
    if (pfn) *pfn = p;
    return p ? CUDA_SUCCESS : CUDA_ERROR_NOT_FOUND;
}
API cuGetProcAddress_v2(const char *sym, void **pfn, int ver, uint64_t flags, int *status)
{
    CUresult r = cuGetProcAddress(sym, pfn, ver, flags);
    if (status) *status = (r == CUDA_SUCCESS) ? 0 : 1;
    return r;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init_once(); }
    return TRUE;
}
