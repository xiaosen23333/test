/* 宿主 HLE —— nn::fs 的目录 / 存档 / 写文件部分（19 个）。
 *
 * 与 hle_nn_fs.c 的分工：
 *   hle_nn_fs.c   基本的挂载/打开/读/关（8 个）
 *   本文件         目录、存档、创建/删除/改名、写文件、大小（19 个）
 *
 * 路径映射规则（与 hle_nn_fs.c 一致）：
 *   客体 "/data/xxx"        → <romfs_root>/data/xxx
 *   客体 存档路径（挂载后）  → <save_root>/<name>/...（可写）
 *
 * 存档的处理：
 *   Horizon 的存档是一个挂载点，客体用 MountSaveData("save", uid) 挂上，
 *   之后按路径访问。宿主把它落到 <save_root>/save/ 目录，**这样存档能持久化**
 *   —— 这是「游戏可玩」的必要条件之一。
 */
#include "hle_nn.h"
#include "hle_nn_fs2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
  #include <direct.h>
  #include <io.h>
  #define DM_MKDIR(p) _mkdir(p)
  #define DM_STAT     _stat64
  #define DM_STAT_T   struct _stat64
#else
  #include <unistd.h>
  #include <dirent.h>
  #define DM_MKDIR(p) mkdir(p, 0755)
  #define DM_STAT     stat
  #define DM_STAT_T   struct stat
#endif

/* ---------------- 根目录 ---------------- */

static char g_romfs2_root[1024] = {0};
static char g_save_root[1024]   = {0};

/* RomFS 根目录。注意：hle_nn_fs.c 有自己的 g_romfs_root —— 两个模块各存一份，
   所以对外统一用 dm_nn_fs_set_romfs_root()，它会把两边都设上（见 hle_nn_fs.c）。
   上一版只设了一边，导致 fs2 的目录/类型查询全部找不到路径。 */
void dm_nn_fs2_set_romfs_root(const char *dir) {
    if (!dir) { g_romfs2_root[0] = 0; return; }
    snprintf(g_romfs2_root, sizeof g_romfs2_root, "%s", dir);
}

void dm_nn_fs2_set_save_root(const char *dir) {
    if (!dir) { g_save_root[0] = 0; return; }
    snprintf(g_save_root, sizeof g_save_root, "%s", dir);
    DM_MKDIR(g_save_root);
    char sub[1280];
    snprintf(sub, sizeof sub, "%s/save", g_save_root);
    DM_MKDIR(sub);
}

static struct {
    uint64_t n_create_file, n_delete_file, n_rename, n_set_size, n_entry_type;
    uint64_t n_open_dir, n_read_dir, n_close_dir, n_dir_count;
    uint64_t n_mkdir, n_rmdir, n_mount_save, n_commit, n_ensure;
    uint64_t n_get_save_size, n_get_save_max, n_extend, n_commit_path;
    uint64_t n_write_file;
} g_fs2_stats;
const void *dm_nn_fs2_stats_ptr(void) { return &g_fs2_stats; }

/* 客体路径 → 宿主路径。
   规则：
     - 以 "/save" 开头（存档挂载点）→ <save_root>/...
     - 其他 → <romfs_root>/...        */
static int map_path(char *out, size_t cap, const char *guest) {
    if (!guest) return -1;
    const char *p = guest;
    while (*p == '/') p++;
    if (!strncmp(p, "rom:/", 5)) { p += 5; while (*p == '/') p++; }
    if (!strncmp(p, "save:/", 6)) { p += 6; while (*p == '/') p++; }
    /* 存档挂载点 */
    if (!strncmp(p, "save", 4) && (p[4] == 0 || p[4] == '/')) {
        if (!g_save_root[0]) return -1;
        snprintf(out, cap, "%s/save%s", g_save_root, p + 4);
        return 0;
    }
    if (!g_romfs2_root[0]) return -1;
    snprintf(out, cap, "%s/%s", g_romfs2_root, p);
    return 0;
}

/* 递归建目录（宿主侧需要 —— 客体可能只调一次 CreateDirectory 却给多级路径） */
static void mkdirs(const char *path) {
    char buf[1280];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *q = buf + 1; *q; q++) {
        if (*q == '/' || *q == '\\') {
            char save = *q; *q = 0;
            DM_MKDIR(buf);
            *q = save;
        }
    }
    DM_MKDIR(buf);
}

/* 取父目录并确保存在 */
static void ensure_parent(const char *path) {
    char buf[1280];
    snprintf(buf, sizeof buf, "%s", path);
    char *q = strrchr(buf, '/');
    if (!q) q = strrchr(buf, '\\');
    if (q) { *q = 0; mkdirs(buf); }
}

/* ---------------- 文件操作 ---------------- */

/* int nn::fs::CreateFile(const char *path, int64_t size) */
int dm_nn_fs2_CreateFile(const char *path, int64_t size) {
    g_fs2_stats.n_create_file++;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return -1;
    ensure_parent(hp);
    FILE *f = fopen(hp, "wb");
    if (!f) return -2;
    if (size > 0) {
        /* 置大小：写最后一个字节 */
        if (fseek(f, (long)(size - 1), SEEK_SET) == 0) fputc(0, f);
    }
    fclose(f);
    return 0;
}

/* int nn::fs::DeleteFile(const char *path) */
int dm_nn_fs2_DeleteFile(const char *path) {
    g_fs2_stats.n_delete_file++;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return -1;
    return remove(hp) ? -2 : 0;
}

/* int nn::fs::RenameFile(const char *from, const char *to) */
int dm_nn_fs2_RenameFile(const char *from, const char *to) {
    g_fs2_stats.n_rename++;
    char a[1280], b[1280];
    if (map_path(a, sizeof a, from) || map_path(b, sizeof b, to)) return -1;
    ensure_parent(b);
    return rename(a, b) ? -2 : 0;
}

/* int nn::fs::SetFileSize(FileHandle h, int64_t size) */
int dm_nn_fs2_SetFileSize(uint64_t handle, int64_t size) {
    g_fs2_stats.n_set_size++;
    extern int dm_nn_fs_SeekRaw(uint64_t handle, int64_t off);   /* 见 hle_nn_fs.c */
    return dm_nn_fs_TruncateRaw(handle, size);
}

/* int nn::fs::GetEntryType(DirectoryEntryType *out, const char *path)
   DirectoryEntryType: 0 = Directory, 1 = File */
int dm_nn_fs2_GetEntryType(int *out, const char *path) {
    g_fs2_stats.n_entry_type++;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return -1;
    DM_STAT_T st;
    if (DM_STAT(hp, &st)) return -2;
#if defined(_WIN32)
    if (out) *out = (st.st_mode & _S_IFDIR) ? 0 : 1;
#else
    if (out) *out = S_ISDIR(st.st_mode) ? 0 : 1;
#endif
    return 0;
}

/* int nn::fs::WriteFile(uint64_t *out_written, FileHandle h, int64_t off,
                         const void *buf, uint64_t size, const WriteOption &opt)
   WriteOption 是 8 字节的 {u32 flags, u32 handle}；含 Flush 标志时立即落盘。 */
int dm_nn_fs2_WriteFile(uint64_t *out_written, uint64_t handle, int64_t off,
                        const void *buf, uint64_t size, const void *opt) {
    g_fs2_stats.n_write_file++;
    int flush = 0;
    if (opt) { uint32_t flags = *(const uint32_t *)opt; flush = (flags & 1) != 0; }
    return dm_nn_fs_WriteRaw(out_written, handle, off, buf, size, flush);
}

/* ---------------- 目录操作 ---------------- */

void dm_nn_fs2_CreateDirectory(const char *path) {
    g_fs2_stats.n_mkdir++;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return;
    mkdirs(hp);
}

void dm_nn_fs2_DeleteDirectory(const char *path) {
    g_fs2_stats.n_rmdir++;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return;
#if defined(_WIN32)
    _rmdir(hp);
#else
    rmdir(hp);
#endif
}

/* 目录句柄表 */
#define DM_MAX_DIRS 128
typedef struct {
    int used;
    char host_path[1280];
    char names[512][256];       /* 目录项名字（打开时一次性读入，简单且够用） */
    int  n;
    int  cursor;
} dm_dir_ent;
static dm_dir_ent *g_dirs[DM_MAX_DIRS];

static void scan_dir(dm_dir_ent *d) {
    d->n = 0;
#if defined(_WIN32)
    char pat[1400];
    snprintf(pat, sizeof pat, "%s/*", d->host_path);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..")) continue;
        if (d->n >= 512) break;
        snprintf(d->names[d->n], sizeof d->names[0], "%s", fd.name);
        d->n++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *dp = opendir(d->host_path);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (d->n >= 512) break;
        snprintf(d->names[d->n], sizeof d->names[0], "%s", de->d_name);
        d->n++;
    }
    closedir(dp);
#endif
}

/* int nn::fs::OpenDirectory(DirectoryHandle *out, const char *path, int mode) */
int dm_nn_fs2_OpenDirectory(uint64_t *out, const char *path, int mode) {
    (void)mode;
    g_fs2_stats.n_open_dir++;
    if (!out) return -1;
    char hp[1280];
    if (map_path(hp, sizeof hp, path)) return -2;
    for (int i = 0; i < DM_MAX_DIRS; i++) {
        if (g_dirs[i]) continue;
        dm_dir_ent *d = (dm_dir_ent *)calloc(1, sizeof *d);
        if (!d) return -3;
        d->used = 1;
        snprintf(d->host_path, sizeof d->host_path, "%s", hp);
        scan_dir(d);
        g_dirs[i] = d;
        *out = (uint64_t)(i + 1);
        return 0;
    }
    return -4;
}

static dm_dir_ent *dir_of(uint64_t h) {
    if (h < 1 || h > DM_MAX_DIRS) return NULL;
    return g_dirs[h - 1];
}

/* int nn::fs::ReadDirectory(int64_t *out_count, DirectoryEntry *out,
                             DirectoryHandle h, int64_t max) */
int dm_nn_fs2_ReadDirectory(int64_t *out_count, void *out, uint64_t handle, int64_t max) {
    g_fs2_stats.n_read_dir++;
    dm_dir_ent *d = dir_of(handle);
    if (!d || !out) return -1;
    int64_t got = 0;
    /* DirectoryEntry 布局（Horizon）：{char name[0x300]; u8 pad; u8 type; ...}
       名字在最前面，类型在 0x300 附近。宿主按这个布局填。 */
    uint8_t *dst = (uint8_t *)out;
    const size_t ENTRY_SIZE = 0x310;
    while (got < max && d->cursor < d->n) {
        uint8_t *e = dst + (size_t)got * ENTRY_SIZE;
        memset(e, 0, ENTRY_SIZE);
        snprintf((char *)e, 0x300, "%s", d->names[d->cursor]);
        /* 判断类型 */
        char full[1600];
        snprintf(full, sizeof full, "%s/%s", d->host_path, d->names[d->cursor]);
        DM_STAT_T st;
        uint8_t type = 1;   /* 默认文件 */
        if (!DM_STAT(full, &st)) {
#if defined(_WIN32)
            type = (st.st_mode & _S_IFDIR) ? 0 : 1;
#else
            type = S_ISDIR(st.st_mode) ? 0 : 1;
#endif
        }
        e[0x300] = type;
        d->cursor++;
        got++;
    }
    if (out_count) *out_count = got;
    return 0;
}

/* int nn::fs::GetDirectoryEntryCount(int64_t *out, DirectoryHandle h) */
int dm_nn_fs2_GetDirectoryEntryCount(int64_t *out, uint64_t handle) {
    g_fs2_stats.n_dir_count++;
    dm_dir_ent *d = dir_of(handle);
    if (!d || !out) return -1;
    *out = d->n;
    return 0;
}

/* int nn::fs::CloseDirectory(DirectoryHandle h) */
int dm_nn_fs2_CloseDirectory(uint64_t handle) {
    g_fs2_stats.n_close_dir++;
    if (handle < 1 || handle > DM_MAX_DIRS) return -1;
    free(g_dirs[handle - 1]);
    g_dirs[handle - 1] = NULL;
    return 0;
}

/* ---------------- 存档 ----------------
 *
 * Horizon 的存档 API 都带 account::Uid（0x10 字节的用户 ID）。
 * 宿主不区分用户，落到同一个 <save_root>/save/ 下。
 * 这样**存档能持久化**，是「可玩」的前提。
 */

int dm_nn_fs2_MountSaveData(const char *name, const void *uid) {
    (void)name; (void)uid;
    g_fs2_stats.n_mount_save++;
    if (!g_save_root[0]) return -1;
    char sub[1280];
    snprintf(sub, sizeof sub, "%s/save", g_save_root);
    mkdirs(sub);
    return 0;
}

int dm_nn_fs2_CommitSaveData(const char *name) {
    (void)name;
    g_fs2_stats.n_commit++;
    return 0;      /* 宿主直接写文件，没有「提交」这一步 */
}

int dm_nn_fs2_EnsureSaveData(const void *uid) {
    (void)uid;
    g_fs2_stats.n_ensure++;
    if (!g_save_root[0]) return -1;
    char sub[1280];
    snprintf(sub, sizeof sub, "%s/save", g_save_root);
    mkdirs(sub);
    return 0;
}

int dm_nn_fs2_ExtendSaveData(const void *uid, int64_t a, int64_t b) {
    (void)uid; (void)a; (void)b;
    g_fs2_stats.n_extend++;
    return 0;
}

int dm_nn_fs2_GetSaveDataSize(int64_t *out_journal, int64_t *out_normal, const void *uid) {
    (void)uid;
    g_fs2_stats.n_get_save_size++;
    if (out_journal) *out_journal = 0x1000;
    if (out_normal)  *out_normal  = 0x100000;
    return 0;
}

int dm_nn_fs2_GetSaveDataSizeMax(int64_t *out_journal, int64_t *out_normal) {
    g_fs2_stats.n_get_save_max++;
    if (out_journal) *out_journal = 0x1000;
    if (out_normal)  *out_normal  = 0x4000000;
    return 0;
}

int dm_nn_fs2_Commit(const char *path) {
    (void)path;
    g_fs2_stats.n_commit_path++;
    return 0;
}
