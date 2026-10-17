/* 宿主 HLE —— nn:: 系列函数的声明。
 *
 * 这 38 个函数就是 main 的全部平台面（main 不做任何 svc）。
 * 命名规则：dm_nn_<名字空间>_<函数名>，参数按 AArch64 调用约定原样对应。
 */
#ifndef DM_HLE_NN_H
#define DM_HLE_NN_H

#include <stdint.h>

/* ---------- nn::os ---------- */
void  *dm_nn_AllocateMemoryBlock(uint64_t *out_size, uint64_t size);
/* 第 82 轮：真机 ABI —— 地址写回 *out_addr，返回 Result（0 成功 / 0x1003 失败）。
   loader.c 对 _ZN2nn2os19AllocateMemoryBlockEPmm 强制接管到这个实现。 */
uint64_t dm_nn_AllocateMemoryBlock_out(uint64_t *out_addr, uint64_t size);
void   dm_nn_FreeMemoryBlock(void *p, uint64_t size);
/* 第 85/86 轮：崩溃块里的「内存账本」（池/客体堆/已划走/块数/最近失败/最近大块） */
void   dm_mem_stats(uint64_t *cap, uint64_t *cur, uint64_t *carved, int *blocks,
                    uint64_t *last_fail, int *last_err,
                    uint64_t *last_big, uint64_t *last_big_ptr, int *last_big_ok, int *fail_n);
/* 第 89 轮：>=64MB 分配的历史（老→新），用于判断哪个池没拿到内存 */
void   dm_big_hist(uint32_t *mb, int *ok, int *n);
void   dm_nn_SetMemoryHeapSize(uint64_t size);
void   dm_nn_QueryMemoryInfo(uint64_t *out);
void   dm_nn_SetMemoryAllocatorForThreadLocal(void *alloc, void *freefn);
int    dm_nn_GetHostArgc(void);
char **dm_nn_GetHostArgv(void);

void   dm_nn_InitializeMutex(void *m, int recursive, int level);
void   dm_nn_FinalizeMutex(void *m);
void   dm_nn_LockMutex(void *m);
void   dm_nn_UnlockMutex(void *m);
void   dm_nn_InitializeBusyMutex(void *m);
void   dm_nn_FinalizeBusyMutex(void *m);
void   dm_nn_LockBusyMutex(void *m);
int    dm_nn_TryLockBusyMutex(void *m);
void   dm_nn_UnlockBusyMutex(void *m);

void   dm_nn_InitializeLightEvent(void *e, int signaled, int mode);
void   dm_nn_FinalizeLightEvent(void *e);
void   dm_nn_SignalLightEvent(void *e);
void   dm_nn_ClearLightEvent(void *e);
void   dm_nn_WaitLightEvent(void *e);

void   dm_nn_SleepThread(int64_t ns);
void   dm_nn_YieldThread(void);

/* ---------- nn::oe ---------- */
/* Horizon 的 LanguageCode：0=AmericanEnglish … 简体中文是 0x0B（与 D2R 一致） */
uint64_t dm_nn_GetDesiredLanguage(void);
void     dm_nn_SetFocusHandlingMode(int mode);

/* ---------- nn::fs ---------- */
void dm_nn_fs_SetAllocator(void *alloc, void *freefn);
void dm_nn_fs_QueryMountRomCacheSize(uint64_t *out);
int  dm_nn_fs_MountRom(const char *name, void *cache, uint64_t cache_size);
int  dm_nn_fs_Unmount(const char *name);
/* FileHandle 在 Horizon 里是 8 字节的 {u8 raw[8]}；这里当作 uint64 传 */
int  dm_nn_fs_OpenFile(void *out_handle, const char *path, int mode);
int  dm_nn_fs_GetFileSize(int64_t *out_size, uint64_t handle);
int  dm_nn_fs_ReadFile(uint64_t *out_read, uint64_t handle, int64_t offset, void *buf, uint64_t size);
int  dm_nn_fs_CloseFile(uint64_t handle);

/* ---------- nn::ro ---------- */
void dm_nn_ro_Initialize(void);
int  dm_nn_ro_LoadModule(void *out_mod, const void *path, void *buf, uint64_t size, int flags);
int  dm_nn_ro_RegisterModuleInfo(void *info, const void *mod);
int  dm_nn_ro_ProtectRelro(const void *a, const void *b, const void *c, const void *d, const void *e);
int  dm_nn_ro_GetBufferSize(uint64_t *out, const void *mod);

/* ---------- 宿主强制接管（按符号名） ---------- */
/* 装载器在解析重定位时先问它；返回非空就用宿主的实现，而不是客体里的。 */
void *dm_nn_force_lookup(const char *name);
unsigned dm_nn_symbol_count(void);

/* RomFS 根目录（宿主启动时设置；nn::fs 的路径在这里解析） */
void dm_nn_fs_set_romfs_root(const char *dir);
/* 只为自测：把上面的 38 个符号全部「预解析」一遍，返回找到的数量 */
unsigned dm_nn_selftest_symbols(void);

#endif /* DM_HLE_NN_H */
