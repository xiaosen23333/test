/* 宿主图形层 —— NVN 对象层的声明与注册。 */
#ifndef DM_HLE_NVN_OBJ_H
#define DM_HLE_NVN_OBJ_H

#include <stdint.h>

/* 设备属性（Tegra X1 实测值） */
int dm_nvn_devinfo(int item);

/* device */
int   dm_nvn_DeviceInitialize(void *dev, const void *builder);
void  dm_nvn_DeviceFinalize(void *dev);
/* nvnDeviceGetInteger(device, item, int *out) —— 通过指针写回（第 82 轮按 SDK 原型修正） */
void  dm_nvn_DeviceGetInteger(const void *dev, int item, int *out);
void *dm_nvn_DeviceGetProcAddress(const void *dev, const char *name);
void  dm_nvn_DeviceSetDebugLabel(void *dev, const char *label);

/* memory pool */
void     dm_nvn_MemoryPoolInitialize(void *mp, const void *dev, void *addr, uint64_t size, int flags);
void     dm_nvn_MemoryPoolFinalize(void *mp);
uint64_t dm_nvn_MemoryPoolGetSize(const void *mp);
void    *dm_nvn_MemoryPoolGetAddress(const void *mp);
uint64_t dm_nvn_MemoryPoolGetFlags(const void *mp);

/* buffer */
void     dm_nvn_BufferInitialize(void *buf, const void *dev, const void *pool, uint64_t off, uint64_t size);
void     dm_nvn_BufferFinalize(void *buf);
uint64_t dm_nvn_BufferGetAddress(const void *buf);
uint64_t dm_nvn_BufferGetSize(const void *buf);
void     dm_nvn_BufferFlushMappedRange(void *buf, uint64_t off, uint64_t size);

/* texture */
void dm_nvn_TextureInitialize(void *tex, const void *dev, uint64_t target, int w, int h, int d,
                              int levels, int format, int type, int flags);
void dm_nvn_TextureFinalize(void *tex);
int  dm_nvn_TextureGetWidth(const void *tex);
int  dm_nvn_TextureGetHeight(const void *tex);
int  dm_nvn_TextureGetDepth(const void *tex);
int  dm_nvn_TextureGetFormat(const void *tex);
int  dm_nvn_TextureGetLevels(const void *tex);

/* window */
void dm_nvn_WindowInitialize(void *win, const void *dev, const void *builder);
void dm_nvn_WindowFinalize(void *win);
void dm_nvn_WindowSetCrop(void *win, int x, int y, int w, int h);
int  dm_nvn_WindowGetWidth(const void *win);
int  dm_nvn_WindowGetHeight(const void *win);

/* queue */
void dm_nvn_QueueSubmitCommands(void *q, int n, const void *cmds);
void dm_nvn_QueuePresentTexture(void *q, void *win, int slot);
void dm_nvn_QueueFinish(void *q);

const void *dm_nvn_obj_stats_ptr(void);

/* 把对象层注册进 NVN 分发表 */
void dm_nvn_obj_register_all(void);

#endif /* DM_HLE_NVN_OBJ_H */
