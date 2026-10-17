/* 宿主图形层 —— NVN 对象层（device / memory pool / buffer / texture / window）。
 *
 * 这些是「有语义、对象是真的」的部分（D2R 的三类分法里的第一类）：
 *   - device：设备句柄 + **设备属性**（游戏会查这些来决定资源规模）
 *   - memory pool：游戏给真内存，宿主记录基址与大小；GPU 地址 = CPU 地址
 *   - buffer：内存池里的一个区间，nvnBufferGetAddress 返回它的地址
 *   - texture：builder 配置 + initialize，宿主记录格式/尺寸
 *   - window：与显示层挂钩
 *
 * 设备属性的数值来自 Tegra X1（与 D2R 同一来源：nnheaders nvn_DeviceConstantsNX.h）。
 * **这些值不能乱填** —— 游戏用它们算纹理/缓冲的尺寸与对齐，填错会越界或分配失败。
 *
 * 对象布局：宿主在网络（device/memory pool/…）对象开头放自己的宿主结构指针。
 * 客体结构的大小由 NVN 头文件固定，宿主不改变它。
 */
#include "hle_nvn.h"
#include "hle_nvn_obj.h"
#include "hle_diag.h"   /* 第 82 轮：设备项查询写进设备日志（真机看得到） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 设备属性（Tegra X1 实测值） ----------------
 * 来源与 D2R 完全一致（dm/gfx/nvn/hle_nvn.c 的 g_devinfo，nnheaders
 * nvn_DeviceConstantsNX.h，在 SMO 上逐项调 nvnDeviceGetInteger 得到）。
 *
 * !! 第 82 轮：这张表原来只填到 0x1F，而且 0x1B..0x1F 的值与 D2R 不一致。
 * 引擎的 CGraTexMgr 构造会查 0x0F / 0x10 / 0x11 / 0x12 / 0x25，其中 **0x25 旧表没有 → 返回 0**，
 * 再乘出一大片 0，最后算出 ~2GB 的纹理池请求 —— 直接导致 CLinkBlockHeap::alloc 下溢崩溃。
 * 现在补齐到 0x5F（与 D2R 逐项相同）。 */
static const int g_devinfo[0x60] = {
    [0x00] = 53, [0x01] = 311,   /* API 版本 */
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};

int dm_nvn_devinfo(int item) {
    if (item < 0 || item >= (int)(sizeof g_devinfo / sizeof g_devinfo[0])) return 0;
    return g_devinfo[item];
}

/* ---------------- 对象 ---------------- */

typedef struct {
    uint32_t magic;
    /* device */
    int      inited;
    uint64_t api_major, api_minor;
    /* memory pool */
    void    *pool_addr;
    uint64_t pool_size;
    uint64_t pool_flags;
    int      pool_inited;
    /* buffer */
    uint64_t buf_addr;      /* GPU 地址 = CPU 地址（内存池里偏移后的绝对地址） */
    uint64_t buf_size;
    int      buf_inited;
    /* texture */
    uint64_t tex_target, tex_width, tex_height, tex_depth;
    uint32_t tex_format, tex_type, tex_levels;
    int      tex_inited;
    /* window */
    int      win_inited;
    uint64_t win_width, win_height;
} nvn_obj_t;

#define NVN_MAGIC_DEV  0x4E564431u    /* "NVD1" */
#define NVN_MAGIC_POOL 0x4E565031u    /* "NVP1" */
#define NVN_MAGIC_BUF  0x4E564231u    /* "NVB1" */
#define NVN_MAGIC_TEX  0x4E565431u    /* "NVT1" */
#define NVN_MAGIC_WIN  0x4E565731u    /* "NVW1" */

static struct {
    uint64_t n_dev_init, n_dev_query, n_pool_init, n_buf_init, n_buf_addr;
    uint64_t n_tex_init, n_win_init, n_queue_submit, n_queue_present;
} g_obj_stats;
const void *dm_nvn_obj_stats_ptr(void) { return &g_obj_stats; }

/* Round 97: the caller address of a host hook (MSVC has no __builtin_return_address) */
#if defined(_MSC_VER)
#include <intrin.h>
#define NVN_RET_ADDR() ((void *)_ReturnAddress())
#else
#define NVN_RET_ADDR() (__builtin_return_address(0))
#endif

/* Round 96: never dereference a guest pointer we have not already proven readable.
 *
 * Evidence (r95 crash, host+0x12578 = dm_nvn_BufferInitialize+0x58):
 *     ldr x9, [x21]        x21 = 0x5be7f80   <- the engine passed a bogus pool pointer
 * The host trusted it and died. A bogus pointer from the guest must degrade to a
 * logged miss, not a SIGSEGV: this is also how we learn which call site produces it.
 *
 * dm_diag_addr_left() answers "how many bytes from this address are inside a
 * registered readable region" (guest modules, and the application pool registered
 * by hle_plat_aarch64.c). 0 = not safe to touch. */
static int g_ptr_guard = 1;   /* the Windows selftest uses stack objects, so it turns this off */
void dm_nvn_SetPtrGuard(int on) { g_ptr_guard = on; }

static int ptr_ok(const void *p) {
    extern unsigned long long dm_diag_addr_left(unsigned long long);
    if (!g_ptr_guard) return p != NULL;
    return p && dm_diag_addr_left((unsigned long long)(uintptr_t)p) >= 8;
}

static unsigned g_bad_ptr_logs;
static void log_bad_ptr(const char *what, const void *p) {
    if (g_bad_ptr_logs++ < 12)
        dm_diag_mark("[nvn] %s: 客体指针 %p 不在可读区（本轮已拒绝 %u 次）",
                     what, p, g_bad_ptr_logs);
}

static nvn_obj_t *obj_of(void *p, uint32_t magic) {
    if (!ptr_ok(p)) return NULL;
    nvn_obj_t *o = *(nvn_obj_t **)p;
    if (!o) return NULL;
    /* the host object itself lives in our heap, so a plain read is fine once we know
     * the guest slot was readable; but the stored value may still be garbage */
    if (o->magic != magic) return NULL;
    return o;
}
static nvn_obj_t *obj_new(void *p, uint32_t magic) {
    if (!ptr_ok(p)) { log_bad_ptr("obj_new", p); return NULL; }
    nvn_obj_t *o = (nvn_obj_t *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->magic = magic;
    *(nvn_obj_t **)p = o;
    return o;
}
static void obj_del(void *p, uint32_t magic) {
    nvn_obj_t *o = obj_of(p, magic);
    if (!o) return;
    free(o);
    *(nvn_obj_t **)p = NULL;
}

/* ---------------- device ---------------- */

int dm_nvn_DeviceInitialize(void *dev, const void *builder) {
    (void)builder;
    g_obj_stats.n_dev_init++;
    nvn_obj_t *o = obj_new(dev, NVN_MAGIC_DEV);
    if (!o) return 0;
    o->inited = 1;
    o->api_major = (uint64_t)g_devinfo[0x00];
    o->api_minor = (uint64_t)g_devinfo[0x01];
    return 1;                    /* NVNboolean = true */
}
void dm_nvn_DeviceFinalize(void *dev) {
    obj_del(dev, NVN_MAGIC_DEV);
}
/* nvnDeviceGetInteger(const NVNdevice*, NVNdeviceInfo what, int *out)
 *
 * !! 第 82 轮：签名必须是 (device, item, out) —— **通过指针写回**，不是返回值 !!
 * 依据：dm/gfx/nvn/nvn_protos.h 的 `typedef void (*NVNT_nvnDeviceGetInteger)(const NVNdevice*, NVNdeviceInfo, int*)`，
 * 以及真机实证（DmGame-2026-10-17-171543.ips）：Nmp1DLL 的 CGraTexMgr 构造函数就是
 *     x0 = device; w1 = item; x2 = &out; blr nvnDeviceGetInteger
 * 旧实现返回 int 且不写 *out ⇒ 引擎读到的是栈上的垃圾，算出 ~2GB 的纹理池请求 ⇒
 * CLinkBlockHeap::alloc 里 (block_end - size) 下溢，写 0xffffffffb3c6e118 崩。
 * 修好后同一处查询 0x0F/0x10/0x11/0x12/0x25 得到 32/32/256/256/4096，纹理池请求回到几十 KB。 */
void dm_nvn_DeviceGetInteger(const void *dev, int item, int *out) {
    (void)dev;
    g_obj_stats.n_dev_query++;
    int v = dm_nvn_devinfo(item);
    if (out) *out = v;
    /* 头几次查询写进日志：下一轮真机日志能直接看到引擎要哪些设备项、我们给了什么 */
    static unsigned n_log = 0;
    if (n_log < 12) {
        n_log++;
        dm_diag_mark("[nvn] DeviceGetInteger(%#x) -> %d", item, v);
    }
}
/* nvnDeviceGetProcAddress(dev, name) —— 按名返回实现（与 D2R 同语义） */
void *dm_nvn_DeviceGetProcAddress(const void *dev, const char *name) {
    (void)dev;
    int idx = dm_nvn_index_of(name);
    if (idx >= 0 && nvn_slot[idx].fn) return nvn_slot[idx].fn;
    return (void *)dm_nvn_unimpl;
}
void dm_nvn_DeviceSetDebugLabel(void *dev, const char *label) { (void)dev; (void)label; }

/* ---------------- memory pool ---------------- */

/* !! 第 91 轮：MemoryPool 的**真机 ABI 是 builder 式** !!
 *
 *   NVNmemoryPoolBuilder = { dev, mem, size, flags }   （4 × 8 字节）
 *   nvnMemoryPoolBuilderSetDefaults(b)
 *   nvnMemoryPoolBuilderSetDevice(b, dev)
 *   nvnMemoryPoolBuilderSetStorage(b, mem, size)
 *   nvnMemoryPoolBuilderSetFlags(b, flags)
 *   nvnMemoryPoolInitialize(pool, b)      ← **只收两个参数**
 *
 * 以前我们按 (pool, dev, addr, size, flags) 解析 Initialize —— 引擎传进来的 x1 其实是
 * builder 指针，于是 pool_addr/pool_size 拿到的是寄存器里的垃圾。
 * 实证（2026-10-17 的 .ips）：块头 `基址=0x30000000`，正好是一个残留寄存器值；
 * 正确值应该是 AllocateMemoryBlock 给的那块内存。 */
#define NVN_POOL_BUILDER_SIZE 0x20

void dm_nvn_MemoryPoolBuilderSetDefaults(void *b) {
    if (b) memset(b, 0, NVN_POOL_BUILDER_SIZE);
}
void dm_nvn_MemoryPoolBuilderSetDevice(void *b, const void *dev) {
    if (b) *(uint64_t *)((char *)b + 0x00) = (uint64_t)(uintptr_t)dev;
}
void dm_nvn_MemoryPoolBuilderSetStorage(void *b, void *mem, uint64_t size) {
    if (!b) return;
    *(uint64_t *)((char *)b + 0x08) = (uint64_t)(uintptr_t)mem;
    *(uint64_t *)((char *)b + 0x10) = size;
}
void dm_nvn_MemoryPoolBuilderSetFlags(void *b, int flags) {
    if (b) *(uint64_t *)((char *)b + 0x18) = (uint64_t)(unsigned)flags;
}

void dm_nvn_MemoryPoolInitialize(void *mp, const void *builder) {
    g_obj_stats.n_pool_init++;
    nvn_obj_t *o = obj_new(mp, NVN_MAGIC_POOL);
    if (!o) return;
    const char *b = (const char *)builder;
    o->pool_addr  = b ? *(void **)(uintptr_t)(b + 0x08) : NULL;
    o->pool_size  = b ? *(uint64_t *)(uintptr_t)(b + 0x10) : 0;
    o->pool_flags = b ? *(uint64_t *)(uintptr_t)(b + 0x18) : 0;
    o->pool_inited = 1;
}
void dm_nvn_MemoryPoolFinalize(void *mp) { obj_del(mp, NVN_MAGIC_POOL); }
uint64_t dm_nvn_MemoryPoolGetSize(const void *mp) {
    nvn_obj_t *o = obj_of((void *)mp, NVN_MAGIC_POOL);
    return o ? o->pool_size : 0;
}
void *dm_nvn_MemoryPoolGetAddress(const void *mp) {
    nvn_obj_t *o = obj_of((void *)mp, NVN_MAGIC_POOL);
    return o ? o->pool_addr : NULL;
}
uint64_t dm_nvn_MemoryPoolGetFlags(const void *mp) {
    nvn_obj_t *o = obj_of((void *)mp, NVN_MAGIC_POOL);
    return o ? o->pool_flags : 0;
}
/* nvnMemoryPoolMap(pool) -> void*
 *
 * !! 第 90 轮：这两个函数以前**没实现**（落到 dm_nvn_unimpl 返回 0）。
 * 引擎建「纹理池」时走的是：
 *     nvnMemoryPoolMap(pool)            → 拿映射后的 CPU 地址
 *     nvnMemoryPoolGetBufferAddress(pool) → 拿 GPU 地址
 * 然后把这个地址当作 CLinkBlockHeap::init 的**堆基址**。
 * 返回 0 的后果（DmGame-2026-10-17-184731.ips）：
 *     块头: 基址=0x0 剩余=0x30000000 尾=0x30000000
 *     → 第一次 alloc 算出 0x2feddfb8（未映射）→ SIGSEGV。
 * 我们的 NVN 是「零拷贝」：池本来就是宿主内存，映射 = 原样返回池地址。 */
void *dm_nvn_MemoryPoolMap(const void *mp) {
    nvn_obj_t *o = obj_of((void *)mp, NVN_MAGIC_POOL);
    return o ? o->pool_addr : NULL;
}
/* nvnMemoryPoolGetBufferAddress(pool) -> NVNbufferAddress（这里 CPU/GPU 地址同值） */
uint64_t dm_nvn_MemoryPoolGetBufferAddress(const void *mp) {
    nvn_obj_t *o = obj_of((void *)mp, NVN_MAGIC_POOL);
    return o ? (uint64_t)(uintptr_t)o->pool_addr : 0;
}

/* ---------------- buffer ---------------- */
/* nvnBufferInitialize(buf, dev, pool, offset, size)
   GPU 地址 = CPU 地址 = 池基址 + 偏移（零拷贝：宿主直接把游戏给的内存当 GPU 内存）。 */
void dm_nvn_BufferInitialize(void *buf, const void *dev, const void *pool,
                            uint64_t offset, uint64_t size) {
    (void)dev;
    g_obj_stats.n_buf_init++;
    /* Round 96: report the arguments the first few times so a bogus pool pointer can be
     * traced to its call site by size/offset instead of by another crash. */
    static unsigned n_log;
    if (n_log++ < 8)
        dm_diag_mark("[nvn] BufferInitialize(buf=%p pool=%p off=%llu size=%llu) caller=%p",
                     buf, pool, (unsigned long long)offset, (unsigned long long)size,
                     NVN_RET_ADDR());
    nvn_obj_t *o = obj_new(buf, NVN_MAGIC_BUF);
    if (!o) { log_bad_ptr("BufferInitialize(buf)", buf); return; }
    nvn_obj_t *p = obj_of((void *)pool, NVN_MAGIC_POOL);
    o->buf_addr = (p && p->pool_addr ? (uint64_t)(uintptr_t)p->pool_addr : 0) + offset;
    o->buf_size = size;
    o->buf_inited = 1;
}
void dm_nvn_BufferFinalize(void *buf) { obj_del(buf, NVN_MAGIC_BUF); }
uint64_t dm_nvn_BufferGetAddress(const void *buf) {
    g_obj_stats.n_buf_addr++;
    nvn_obj_t *o = obj_of((void *)buf, NVN_MAGIC_BUF);
    return o ? o->buf_addr : 0;
}
uint64_t dm_nvn_BufferGetSize(const void *buf) {
    nvn_obj_t *o = obj_of((void *)buf, NVN_MAGIC_BUF);
    return o ? o->buf_size : 0;
}
void dm_nvn_BufferFlushMappedRange(void *buf, uint64_t off, uint64_t size) { (void)buf; (void)off; (void)size; }

/* ---------------- texture ---------------- */

void dm_nvn_TextureInitialize(void *tex, const void *dev, uint64_t target,
                             int width, int height, int depth, int levels,
                             int format, int type, int flags) {
    (void)dev; (void)flags;
    g_obj_stats.n_tex_init++;
    nvn_obj_t *o = obj_new(tex, NVN_MAGIC_TEX);
    if (!o) return;
    o->tex_target = target;
    o->tex_width = (uint64_t)width;
    o->tex_height = (uint64_t)height;
    o->tex_depth = (uint64_t)depth;
    o->tex_levels = (uint32_t)levels;
    o->tex_format = (uint32_t)format;
    o->tex_type = (uint32_t)type;
    o->tex_inited = 1;
}
void dm_nvn_TextureFinalize(void *tex) { obj_del(tex, NVN_MAGIC_TEX); }
int dm_nvn_TextureGetWidth(const void *tex) {
    nvn_obj_t *o = obj_of((void *)tex, NVN_MAGIC_TEX);
    return o ? (int)o->tex_width : 0;
}
int dm_nvn_TextureGetHeight(const void *tex) {
    nvn_obj_t *o = obj_of((void *)tex, NVN_MAGIC_TEX);
    return o ? (int)o->tex_height : 0;
}
int dm_nvn_TextureGetDepth(const void *tex) {
    nvn_obj_t *o = obj_of((void *)tex, NVN_MAGIC_TEX);
    return o ? (int)o->tex_depth : 0;
}
int dm_nvn_TextureGetFormat(const void *tex) {
    nvn_obj_t *o = obj_of((void *)tex, NVN_MAGIC_TEX);
    return o ? (int)o->tex_format : 0;
}
int dm_nvn_TextureGetLevels(const void *tex) {
    nvn_obj_t *o = obj_of((void *)tex, NVN_MAGIC_TEX);
    return o ? (int)o->tex_levels : 0;
}

/* ---------------- window ---------------- */

void dm_nvn_WindowInitialize(void *win, const void *dev, const void *builder) {
    (void)dev; (void)builder;
    g_obj_stats.n_win_init++;
    nvn_obj_t *o = obj_new(win, NVN_MAGIC_WIN);
    if (!o) return;
    o->win_inited = 1;
}
void dm_nvn_WindowFinalize(void *win) { obj_del(win, NVN_MAGIC_WIN); }
void dm_nvn_WindowSetCrop(void *win, int x, int y, int w, int h) {
    (void)win; (void)x; (void)y;
    nvn_obj_t *o = obj_of(win, NVN_MAGIC_WIN);
    if (o) { o->win_width = (uint64_t)w; o->win_height = (uint64_t)h; }
}
int dm_nvn_WindowGetWidth(const void *win) {
    nvn_obj_t *o = obj_of((void *)win, NVN_MAGIC_WIN);
    return o ? (int)o->win_width : 0;
}
int dm_nvn_WindowGetHeight(const void *win) {
    nvn_obj_t *o = obj_of((void *)win, NVN_MAGIC_WIN);
    return o ? (int)o->win_height : 0;
}

/* ---------------- queue ----------------
 * 真机侧这里要把录下的命令回放成 Metal 命令缓冲。
 * TODO（图形输出）：宿主目前只记账，不产生画面。 */
void dm_nvn_QueueSubmitCommands(void *q, int n, const void *cmds) {
    (void)q; (void)n; (void)cmds;
    g_obj_stats.n_queue_submit++;
}
void dm_nvn_QueuePresentTexture(void *q, void *win, int slot) {
    (void)q; (void)win; (void)slot;
    g_obj_stats.n_queue_present++;
}
void dm_nvn_QueueFinish(void *q) { (void)q; }

/* ---------------- 注册进 NVN 分发表 ----------------
 * 只注册**本游戏真的会用到**的名字；`nvn_names.h` 里有 538 个，
 * 逐个 `dm_nvn_register` 即可。这里先注册对象层的核心集合。 */
void dm_nvn_obj_register_all(void) {
    struct { const char *name; void *fn; } tab[] = {
        { "nvnDeviceInitialize",        (void *)dm_nvn_DeviceInitialize },
        { "nvnDeviceFinalize",          (void *)dm_nvn_DeviceFinalize },
        { "nvnDeviceGetInteger",        (void *)dm_nvn_DeviceGetInteger },
        { "nvnDeviceGetProcAddress",    (void *)dm_nvn_DeviceGetProcAddress },
        { "nvnDeviceSetDebugLabel",     (void *)dm_nvn_DeviceSetDebugLabel },

        { "nvnMemoryPoolInitialize",    (void *)dm_nvn_MemoryPoolInitialize },
        { "nvnMemoryPoolFinalize",      (void *)dm_nvn_MemoryPoolFinalize },
        { "nvnMemoryPoolGetSize",       (void *)dm_nvn_MemoryPoolGetSize },
        { "nvnMemoryPoolGetAddress",    (void *)dm_nvn_MemoryPoolGetAddress },
        { "nvnMemoryPoolGetFlags",      (void *)dm_nvn_MemoryPoolGetFlags },
        /* 第 90 轮：这两个漏了会导致引擎的纹理池「基址=0」（见函数注释） */
        { "nvnMemoryPoolMap",               (void *)dm_nvn_MemoryPoolMap },
        { "nvnMemoryPoolGetBufferAddress",  (void *)dm_nvn_MemoryPoolGetBufferAddress },
        /* 第 91 轮：builder 式创建内存池（少了这四个，builder 全是 0，池地址也是 0） */
        { "nvnMemoryPoolBuilderSetDefaults", (void *)dm_nvn_MemoryPoolBuilderSetDefaults },
        { "nvnMemoryPoolBuilderSetDevice",   (void *)dm_nvn_MemoryPoolBuilderSetDevice },
        { "nvnMemoryPoolBuilderSetStorage",  (void *)dm_nvn_MemoryPoolBuilderSetStorage },
        { "nvnMemoryPoolBuilderSetFlags",    (void *)dm_nvn_MemoryPoolBuilderSetFlags },

        { "nvnBufferInitialize",        (void *)dm_nvn_BufferInitialize },
        { "nvnBufferFinalize",          (void *)dm_nvn_BufferFinalize },
        { "nvnBufferGetAddress",        (void *)dm_nvn_BufferGetAddress },
        { "nvnBufferGetSize",           (void *)dm_nvn_BufferGetSize },
        { "nvnBufferFlushMappedRange",  (void *)dm_nvn_BufferFlushMappedRange },

        { "nvnTextureInitialize",       (void *)dm_nvn_TextureInitialize },
        { "nvnTextureFinalize",         (void *)dm_nvn_TextureFinalize },
        { "nvnTextureGetWidth",         (void *)dm_nvn_TextureGetWidth },
        { "nvnTextureGetHeight",        (void *)dm_nvn_TextureGetHeight },
        { "nvnTextureGetDepth",         (void *)dm_nvn_TextureGetDepth },
        { "nvnTextureGetFormat",        (void *)dm_nvn_TextureGetFormat },
        { "nvnTextureGetLevels",        (void *)dm_nvn_TextureGetLevels },

        { "nvnWindowInitialize",        (void *)dm_nvn_WindowInitialize },
        { "nvnWindowFinalize",          (void *)dm_nvn_WindowFinalize },
        { "nvnWindowSetCrop",           (void *)dm_nvn_WindowSetCrop },
        { "nvnWindowGetWidth",          (void *)dm_nvn_WindowGetWidth },
        { "nvnWindowGetHeight",         (void *)dm_nvn_WindowGetHeight },

        { "nvnQueueSubmitCommands",     (void *)dm_nvn_QueueSubmitCommands },
        { "nvnQueuePresentTexture",     (void *)dm_nvn_QueuePresentTexture },
        { "nvnQueueFinish",             (void *)dm_nvn_QueueFinish },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        dm_nvn_register(tab[i].name, tab[i].fn);
}

