/* 宿主 HLE —— nn::fs 的目录 / 存档 / 写文件部分（19 个）。 */
#ifndef DM_HLE_NN_FS2_H
#define DM_HLE_NN_FS2_H

#include <stdint.h>

/* 根目录设置 */
void dm_nn_fs2_set_save_root(const char *dir);

/* 文件 */
int  dm_nn_fs2_CreateFile(const char *path, int64_t size);
int  dm_nn_fs2_DeleteFile(const char *path);
int  dm_nn_fs2_RenameFile(const char *from, const char *to);
int  dm_nn_fs2_SetFileSize(uint64_t handle, int64_t size);
int  dm_nn_fs2_GetEntryType(int *out_type, const char *path);
int  dm_nn_fs2_WriteFile(uint64_t *out_written, uint64_t handle, int64_t off,
                         const void *buf, uint64_t size, const void *opt);

/* 目录 */
void dm_nn_fs2_CreateDirectory(const char *path);
void dm_nn_fs2_DeleteDirectory(const char *path);
int  dm_nn_fs2_OpenDirectory(uint64_t *out, const char *path, int mode);
int  dm_nn_fs2_ReadDirectory(int64_t *out_count, void *out, uint64_t handle, int64_t max);
int  dm_nn_fs2_GetDirectoryEntryCount(int64_t *out, uint64_t handle);
int  dm_nn_fs2_CloseDirectory(uint64_t handle);

/* 存档 */
int  dm_nn_fs2_MountSaveData(const char *name, const void *uid);
int  dm_nn_fs2_CommitSaveData(const char *name);
int  dm_nn_fs2_EnsureSaveData(const void *uid);
int  dm_nn_fs2_ExtendSaveData(const void *uid, int64_t a, int64_t b);
int  dm_nn_fs2_GetSaveDataSize(int64_t *out_j, int64_t *out_n, const void *uid);
int  dm_nn_fs2_GetSaveDataSizeMax(int64_t *out_j, int64_t *out_n);
int  dm_nn_fs2_Commit(const char *path);

const void *dm_nn_fs2_stats_ptr(void);

/* 由 hle_nn_fs.c 提供的底层原语（句柄表在那边） */
int  dm_nn_fs_WriteRaw(uint64_t *out_written, uint64_t handle, int64_t off,
                       const void *buf, uint64_t size, int flush);
int  dm_nn_fs_TruncateRaw(uint64_t handle, int64_t size);

#endif /* DM_HLE_NN_FS2_H */
