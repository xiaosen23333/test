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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 设备属性（Tegra X1 实测值） ---------------- */

static const int g_devinfo[0x100] = {
    [0x00] = 53, [0x01] = 311,          /* API 版本 */
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4,
    [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4,
    [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4,
    [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096,
    [0x1A] = 131072, [0x1B] = 4, [0x1C] = 4, [0x1D] = 4, [0x1E] = 4, [0x1F] = 1024,
};

int dm_nvn_devinfo(int item) {
    if (item < 0 || item >= 0x100) return 0;
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

static nvn_obj_t *obj_of(void *p, uint32_t magic) {
    if (!p) return NULL;
    nvn_obj_t *o = *(nvn_obj_t **)p;
    if (!o || o->magic != magic) return NULL;
    return o;
}
static nvn_obj_t *obj_new(void *p, uint32_t magic) {
    if (!p) return NULL;
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
/* nvnDeviceGetInteger(const NVNdevice*, NVNdeviceInfo item) */
int dm_nvn_DeviceGetInteger(const void *dev, int item) {
    (void)dev;
    g_obj_stats.n_dev_query++;
    return dm_nvn_devinfo(item);
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

void dm_nvn_MemoryPoolInitialize(void *mp, const void *dev, void *addr, uint64_t size, int flags) {
    (void)dev;
    g_obj_stats.n_pool_init++;
    nvn_obj_t *o = obj_new(mp, NVN_MAGIC_POOL);
    if (!o) return;
    o->pool_addr = addr;
    o->pool_size = size;
    o->pool_flags = (uint64_t)flags;
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

/* ---------------- buffer ---------------- */
/* nvnBufferInitialize(buf, dev, pool, offset, size)
   GPU 地址 = CPU 地址 = 池基址 + 偏移（零拷贝：宿主直接把游戏给的内存当 GPU 内存）。 */
void dm_nvn_BufferInitialize(void *buf, const void *dev, const void *pool,
                            uint64_t offset, uint64_t size) {
    (void)dev;
    g_obj_stats.n_buf_init++;
    nvn_obj_t *o = obj_new(buf, NVN_MAGIC_BUF);
    if (!o) return;
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

