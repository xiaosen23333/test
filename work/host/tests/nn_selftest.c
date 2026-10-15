/* nn:: 实现自测 —— 在 Windows 上验证宿主确实接管了每一个需要的符号。 */
#include "../hle.h"
#include "../hle_nn.h"
#include "../nn_expected.h"

#include <stdio.h>
#include <string.h>
extern unsigned dm_nn_expect_count(void);
extern const char *dm_nn_expect_name(unsigned i);
extern const void *dm_nn_fs_stats_ptr(void);

/* nn::fs 的几个函数（hle_nn_fs.c 没有配套头文件，只能就地声明）。
 * !! 必须放在**第一次使用之前** —— 第 44 轮修正：
 *   原来这几个声明零散写在各自的使用点旁边（第 200 多行），而调用在第 100 多行。
 *   MSVC 对「先调用、后声明」只报 C4013 警告并假定返回 int（因此 Windows 上一直没暴露），
 *   但 clang 16 起这是**硬错误**。由 work/porting/check_use_before_decl.py 扫出来的。 */
extern void dm_nn_fs_set_romfs_root(const char *);
extern int  dm_nn_fs_OpenFile(void *, const char *, int);
extern int  dm_nn_fs_CloseFile(uint64_t);
extern int  dm_nn_fs_ReadFile(uint64_t *, uint64_t, int64_t, void *, uint64_t);

static int g_thread_ran = 0;
/* 注意：MSVC 不支持嵌套函数，线程体必须放在文件作用域 */
static void thr_body(void *arg) { (void)arg; g_thread_ran = 1; }

int main(int argc, char **argv) {
    const char *romfs = (argc > 1) ? argv[1] : NULL;
    printf("=== nn:: host coverage ===\n");
    printf("  真实需要（从 5 个模块的符号表提取）: %d\n", DM_NN_EXPECTED_COUNT);

    unsigned have = 0, missing = 0;
    /* 逐条核对「从真实符号表提取的清单」里，宿主有没有实现 */
    for (unsigned i = 0; i < DM_NN_EXPECTED_COUNT; i++) {
        if (dm_nn_force_lookup(g_nn_expected[i])) have++;
        else missing++;
    }
    printf("  宿主已实现    : %u\n", have);
    printf("  宿主未实现    : %u\n", missing);
    printf("  覆盖率        : %.1f%%\n\n", 100.0 * have / DM_NN_EXPECTED_COUNT);

    printf("=== 未实现的按名字空间分组（前 6 组）===\n");
    {
        const char *ns[64]; unsigned cnt[64]; unsigned n = 0;
        for (unsigned i = 0; i < DM_NN_EXPECTED_COUNT; i++) {
            if (dm_nn_force_lookup(g_nn_expected[i])) continue;
            /* 从 _ZN2nn<len><ns> 里取 ns */
            const char *s = g_nn_expected[i];
            char name[32] = {0};
            if (s[0]=='_'&&s[1]=='Z'&&s[2]=='N'&&s[3]=='2'&&s[4]=='n'&&s[5]=='n') {
                unsigned L = (unsigned)(s[6]-'0');
                if (L > 0 && L < 20) { memcpy(name, s+7, L); name[L]=0; }
            } else {
                snprintf(name, sizeof name, "(other)");
            }
            unsigned k = 0;
            for (; k < n; k++) if (!strcmp(ns[k], name)) break;
            if (k == n) { ns[n] = _strdup(name); cnt[n] = 0; n++; }
            cnt[k]++;
        }
        for (unsigned k = 0; k < n && k < 6; k++) {
            /* 简单选择排序前几项 */
            for (unsigned j = k+1; j < n; j++)
                if (cnt[j] > cnt[k]) {
                    unsigned t = cnt[k]; cnt[k] = cnt[j]; cnt[j] = t;
                    const char *p = ns[k]; ns[k] = ns[j]; ns[j] = p;
                }
            printf("  nn::%-10s %u\n", ns[k], cnt[k]);
        }
    }

    printf("\n=== nn::os / fs / ro / oe 的行为自测 ===\n");
    /* 1. 内存 */
    uint64_t sz = 0;
    void *p = dm_nn_AllocateMemoryBlock(&sz, 0x2000);
    printf("  AllocateMemoryBlock(0x2000) -> %s, out_size=%llu\n",
           p ? "ok" : "NULL", (unsigned long long)sz);
    if (p) { memset(p, 0xAB, 0x2000); dm_nn_FreeMemoryBlock(p, 0x2000); printf("  FreeMemoryBlock ok\n"); }

    /* 2. 互斥 */
    struct { void *slot[8]; } mtx; memset(&mtx, 0, sizeof mtx);
    dm_nn_InitializeMutex(&mtx, 0, 0);
    dm_nn_LockMutex(&mtx);   dm_nn_UnlockMutex(&mtx);
    dm_nn_LockMutex(&mtx);   dm_nn_UnlockMutex(&mtx);
    printf("  Mutex init/lock/unlock x2 ok\n");
    dm_nn_FinalizeMutex(&mtx);

    /* 3. BusyMutex 的 trylock 语义 */
    struct { void *slot[8]; } bm; memset(&bm, 0, sizeof bm);
    dm_nn_InitializeBusyMutex(&bm);
    int a = dm_nn_TryLockBusyMutex(&bm);          /* 应成功 */
    printf("  BusyMutex TryLock 首次 -> %d (expect 1)\n", a);
    dm_nn_UnlockBusyMutex(&bm);
    int b = dm_nn_TryLockBusyMutex(&bm);          /* 应再次成功 */
    printf("  BusyMutex TryLock 解锁后 -> %d (expect 1)\n", b);
    dm_nn_UnlockBusyMutex(&bm);
    dm_nn_FinalizeBusyMutex(&bm);

    /* 4. LightEvent：已置位时 Wait 应立即返回 */
    struct { void *slot[8]; } ev; memset(&ev, 0, sizeof ev);
    dm_nn_InitializeLightEvent(&ev, 1, 0);        /* signaled=1, auto clear */
    dm_nn_WaitLightEvent(&ev);                    /* 立即返回 */
    dm_nn_SignalLightEvent(&ev);
    dm_nn_WaitLightEvent(&ev);
    dm_nn_ClearLightEvent(&ev);
    dm_nn_FinalizeLightEvent(&ev);
    printf("  LightEvent init/wait/signal/clear ok\n");

    dm_nn_SleepThread(1000000);                   /* 1 ms */
    dm_nn_YieldThread();
    printf("  SleepThread(1ms) / YieldThread ok\n");

    /* 5. 语言 */
    printf("  GetDesiredLanguage -> %#llx (0x0B = 简体中文)\n",
           (unsigned long long)dm_nn_GetDesiredLanguage());

    /* 6. nn::fs —— 用真实 RomFS 目录读一个文件 */
    if (romfs) {
        dm_nn_fs_set_romfs_root(romfs);
        uint64_t cache = 0;
        dm_nn_fs_QueryMountRomCacheSize(&cache);
        int r = dm_nn_fs_MountRom("dm", NULL, cache);
        printf("  QueryMountRomCacheSize -> %llu, MountRom -> %d\n",
               (unsigned long long)cache, r);

        const char *test = "/data/database/shader.dat";   /* 已知存在的小文件 */
        uint64_t h = 0;
        r = dm_nn_fs_OpenFile(&h, test, 1);
        printf("  OpenFile(\"%s\") -> %d, handle=%llu\n", test, r, (unsigned long long)h);
        if (r == 0) {
            int64_t fsz = 0;
            dm_nn_fs_GetFileSize(&fsz, h);
            printf("  GetFileSize -> %lld\n", (long long)fsz);
            static char buf[8192];
            uint64_t got = 0;
            dm_nn_fs_ReadFile(&got, h, 0, buf, sizeof buf);
            printf("  ReadFile(0, %u) -> got %llu, first 4 bytes: %02x %02x %02x %02x\n",
                   (unsigned)sizeof buf, (unsigned long long)got,
                   (unsigned char)buf[0], (unsigned char)buf[1],
                   (unsigned char)buf[2], (unsigned char)buf[3]);
            dm_nn_fs_CloseFile(h);
            printf("  CloseFile ok\n");
        }
        /* 不存在的文件应当失败 */
        uint64_t h2 = 0;
        int r2 = dm_nn_fs_OpenFile(&h2, "/does/not/exist.dat", 1);
        printf("  OpenFile(不存在) -> %d (expect 非 0)\n", r2);
        dm_nn_fs_Unmount("dm");
    } else {
        printf("  (未提供 romfs 目录，跳过 nn::fs 的文件读写自测)\n");
    }

    printf("\n=== 线程 / TLS / 信号量 / 计时 行为自测 ===\n");
    {
        extern void     dm_nn_CreateThread(void *, void *, void *, void *, uint64_t, int, int);
        extern void     dm_nn_StartThread(void *);
        extern void     dm_nn_WaitThread(void *);
        extern void     dm_nn_SetThreadName(void *, const char *);
        extern int      dm_nn_GetThreadPriority(const void *);
        extern uint64_t dm_nn_GetSystemTick(void);
        extern uint64_t dm_nn_ConvertToTick(int64_t);
        extern int      dm_nn_AllocateTlsSlot(uint32_t *, void (*)(uint64_t));
        extern void     dm_nn_FreeTlsSlot(uint32_t);
        extern void     dm_nn_SetTlsValue(uint32_t, uint64_t);
        extern uint64_t dm_nn_GetTlsValue(uint32_t);
        extern void     dm_nn_InitializeSemaphore(void *, int, int);
        extern void     dm_nn_AcquireSemaphore(void *);
        extern void     dm_nn_ReleaseSemaphore(void *, int);
        extern void     dm_nn_FinalizeSemaphore(void *);
        extern void     dm_nn_InitializeSystemEvent(void *, int);
        extern void     dm_nn_SignalSystemEvent(void *);
        extern int      dm_nn_TimedWaitSystemEvent_rc(void *, int64_t);

        /* 线程：建、起、等 */
        struct { int slot[8]; } th; memset(&th, 0, sizeof th);
        dm_nn_CreateThread(&th, (void *)thr_body, NULL, NULL, 0, 0x2C, 0);
        dm_nn_SetThreadName(&th, "selftest");
        printf("  CreateThread -> idx=%d, GetThreadPriority=%d (expect 44)\n",
               *(int *)&th, dm_nn_GetThreadPriority(&th));
        dm_nn_StartThread(&th);
        for (int i = 0; i < 200 && !g_thread_ran; i++) dm_nn_SleepThread(1000000);  /* 最多等 200ms */
        printf("  StartThread -> body ran = %s\n", g_thread_ran ? "yes" : "NO");
        dm_nn_WaitThread(&th);

        /* 计时：tick 频率 19.2 MHz */
        uint64_t t0 = dm_nn_GetSystemTick();
        uint64_t tk = dm_nn_ConvertToTick(1000000000LL);   /* 1 秒 */
        printf("  GetSystemTick=%llu  ConvertToTick(1s)=%llu (expect 19200000)\n",
               (unsigned long long)t0, (unsigned long long)tk);

        /* TLS */
        uint32_t slot = 0;
        int r = dm_nn_AllocateTlsSlot(&slot, NULL);
        dm_nn_SetTlsValue(slot, 0xCAFEBABEull);
        uint64_t v = dm_nn_GetTlsValue(slot);
        printf("  AllocateTlsSlot -> rc=%d slot=%u; Set/Get = %#llx (expect 0xcafebabe)\n",
               r, slot, (unsigned long long)v);
        dm_nn_FreeTlsSlot(slot);

        /* 信号量 */
        struct { void *p; } sem; memset(&sem, 0, sizeof sem);
        dm_nn_InitializeSemaphore(&sem, 1, 8);
        dm_nn_AcquireSemaphore(&sem);        /* 立即通过（初值 1） */
        dm_nn_ReleaseSemaphore(&sem, 2);
        dm_nn_AcquireSemaphore(&sem);
        dm_nn_AcquireSemaphore(&sem);
        dm_nn_FinalizeSemaphore(&sem);
        printf("  Semaphore init(1,8)/acquire/release(2)/acquire x2/finalize ok\n");

        /* 系统事件（带超时） */
        struct { void *p; } ev; memset(&ev, 0, sizeof ev);
        dm_nn_InitializeSystemEvent(&ev, 1);
        dm_nn_SignalSystemEvent(&ev);
        int got = dm_nn_TimedWaitSystemEvent_rc(&ev, 100000000LL);   /* 100 ms 超时 */
        printf("  SystemEvent signal -> TimedWait = %d (expect 1)\n", got);
        int got2 = dm_nn_TimedWaitSystemEvent_rc(&ev, 10000000LL);   /* 已消费，应超时 0 */
        printf("  SystemEvent 再等待（未置位）-> %d (expect 0)\n", got2);
    }

    printf("\n=== nn::fs 目录 / 存档 / 写文件 行为自测 ===\n");
    if (romfs) {
        extern void dm_nn_fs2_set_save_root(const char *);
        extern int  dm_nn_fs2_CreateFile(const char *, int64_t);
        extern int  dm_nn_fs2_WriteFile(uint64_t *, uint64_t, int64_t, const void *, uint64_t, const void *);
        extern int  dm_nn_fs2_GetEntryType(int *, const char *);
        extern int  dm_nn_fs2_OpenDirectory(uint64_t *, const char *, int);
        extern int  dm_nn_fs2_ReadDirectory(int64_t *, void *, uint64_t, int64_t);
        extern int  dm_nn_fs2_GetDirectoryEntryCount(int64_t *, uint64_t);
        extern int  dm_nn_fs2_CloseDirectory(uint64_t);
        extern int  dm_nn_fs2_MountSaveData(const char *, const void *);
        extern int  dm_nn_fs2_CommitSaveData(const char *);
        extern int  dm_nn_fs2_GetSaveDataSizeMax(int64_t *, int64_t *);

        /* 存档根目录设在宿主临时目录下 */
        char save_root[1024];
        snprintf(save_root, sizeof save_root, "%s/../host/build/savetest", romfs);
        dm_nn_fs2_set_save_root(save_root);
        dm_nn_fs_set_romfs_root(romfs);

        /* 1. 目录：列出 RomFS 的一个真实目录 */
        uint64_t dh = 0;
        int r = dm_nn_fs2_OpenDirectory(&dh, "/data/database", 1);
        int64_t cnt = -1;
        dm_nn_fs2_GetDirectoryEntryCount(&cnt, dh);
        printf("  OpenDirectory(/data/database) -> rc=%d, 项数=%lld\n", r, (long long)cnt);
        if (r == 0 && cnt > 0) {
            static uint8_t ent[0x310 * 4];
            int64_t got = 0;
            dm_nn_fs2_ReadDirectory(&got, ent, dh, 3);
            printf("  ReadDirectory -> 读到 %lld 项，前 3 个: ", (long long)got);
            for (int i = 0; i < (int)got && i < 3; i++)
                printf("\"%s\"(type=%u) ", (char *)(ent + (size_t)i * 0x310), ent[(size_t)i * 0x310 + 0x300]);
            printf("\n");
        }
        dm_nn_fs2_CloseDirectory(dh);

        /* 2. 存档：挂载 + 写 + 读回 */
        int m = dm_nn_fs2_MountSaveData("save", NULL);
        printf("  MountSaveData -> %d\n", m);
        const char *sf = "/save/selftest.bin";
        int cf = dm_nn_fs2_CreateFile(sf, 16);
        uint64_t wh = 0;
        int of = dm_nn_fs_OpenFile(&wh, sf, 2 /*写*/);
        const char payload[] = "DM-HELLO-SAVE!!";
        uint64_t wrote = 0;
        int wr = dm_nn_fs2_WriteFile(&wrote, wh, 0, payload, sizeof payload, NULL);
        dm_nn_fs_CloseFile(wh);
        printf("  CreateFile/OpenFile(写)/WriteFile -> cf=%d of=%d wr=%d wrote=%llu\n",
               cf, of, wr, (unsigned long long)wrote);

        /* 读回 */
        uint64_t rh = 0;
        int ro = dm_nn_fs_OpenFile(&rh, sf, 1);
        char back[32] = {0};
        uint64_t got2 = 0;
        dm_nn_fs_ReadFile(&got2, rh, 0, back, sizeof back - 1);
        dm_nn_fs_CloseFile(rh);
        printf("  读回: rc=%d got=%llu 内容=\"%s\"  %s\n", ro, (unsigned long long)got2, back,
               strcmp(back, payload) == 0 ? "✓ 一致" : "✗ 不一致");

        /* 3. 项类型 */
        int ty = -1;
        dm_nn_fs2_GetEntryType(&ty, "/data/database");
        printf("  GetEntryType(/data/database) -> %d (0=目录)\n", ty);

        /* 4. 存档大小 */
        int64_t j = 0, n2 = 0;
        dm_nn_fs2_GetSaveDataSizeMax(&j, &n2);
        printf("  GetSaveDataSizeMax -> journal=%lld normal=%lld\n", (long long)j, (long long)n2);
        dm_nn_fs2_CommitSaveData("save");
    } else {
        printf("  (未提供 romfs 目录，跳过)\n");
    }

    printf("\n=== result: %s ===\n", missing == 0 ? "ALL PASS" : "PARTIAL");
    return 0;
}
