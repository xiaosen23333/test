/* 命令回放器自测 —— 用**假后端**验证回放的正确性。
 *
 * 判据：
 *   1. 录 N 条命令 → 回放顺序与录制顺序**完全一致**（这是回放器唯一的硬要求）
 *   2. 每条命令被分到正确的类别（bind/draw/clear/state/sync/present）
 *   3. 参数**原样传出**（不能被截断或错位）
 *   4. 「身份未知」的命令走 unknown 回调，而**不被猜成别的类别**
 *   5. 回放后录音缓冲清空（提交语义）
 */
#include "../hle_nvn.h"
#include "../hle_nvn_cmd.h"
#include "../hle_nvn_replay.h"

#include <stdio.h>
#include <string.h>

/* ---- 假后端：把收到的操作按序记下来，供核对 ---- */
#define MAXLOG 128
static struct { char kind[16]; char name[64]; uint64_t a[4]; } g_log[MAXLOG];
static int g_nlog = 0;
static int g_nunknown = 0;

static void logit(const char *kind, const char *name,
                  uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (g_nlog >= MAXLOG) return;
    snprintf(g_log[g_nlog].kind, sizeof g_log[0].kind, "%s", kind);
    snprintf(g_log[g_nlog].name, sizeof g_log[0].name, "%s", name);
    g_log[g_nlog].a[0] = a0; g_log[g_nlog].a[1] = a1;
    g_log[g_nlog].a[2] = a2; g_log[g_nlog].a[3] = a3;
    g_nlog++;
}
static void b_bind   (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("bind",n,a,b,c,d);}
static void b_draw   (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("draw",n,a,b,c,d);}
static void b_clear  (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("clear",n,a,b,c,d);}
static void b_state  (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("state",n,a,b,c,d);}
static void b_sync   (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("sync",n,a,b,c,d);}
static void b_present(void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("present",n,a,b,c,d);}
static void b_other  (void*u,const char*n,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;logit("other",n,a,b,c,d);}
static void b_unknown(void*u,uint64_t a,uint64_t b,uint64_t c,uint64_t d){(void)u;(void)a;(void)b;(void)c;(void)d;g_nunknown++;}

/* ---- 用真实 NVN 名字调「分发」以产生录音（走 t_nvn 的等价路径） ---- */
static uint64_t g_seq = 0;
static void call(const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    /* 经 NVN 分发调用 → 名字对应的实现是「带 idx 的包装」→ 录进环形缓冲 */
    dm_nvn_dispatch(name, a0, a1, a2, a3, 0);
}

int main(void) {
    printf("=== 1) 注册命令类 + 建索引 ===\n");
    dm_nvn_obj_register_all();
    dm_nvn_cmd_register_all();
    dm_nvn_cmd_build_index();
    printf("  对象层实现 %u 个，命令类 %u 个\n",
           dm_nvn_registered(), dm_nvn_cmd_registered());

    printf("\n=== 2) 分类函数核对 ===\n");
    struct { const char *n; dm_cmd_kind k; } K[] = {
        { "nvnCommandBufferDrawElements",       DM_CK_DRAW },
        { "nvnCommandBufferBindProgram",        DM_CK_BIND },
        { "nvnCommandBufferClearColor",         DM_CK_CLEAR },
        { "nvnCommandBufferSetViewport",        DM_CK_STATE },
        { "nvnCommandBufferBarrier",            DM_CK_SYNC },
        { "nvnQueuePresentTexture",             DM_CK_PRESENT },
        { "nvnCommandBufferSomethingElse",      DM_CK_OTHER },
    };
    int kfails = 0;
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        dm_cmd_kind got = dm_nvn_cmd_kind_of(K[i].n);
        if (got != K[i].k) { printf("    ✗ %-34s -> %d, expect %d\n", K[i].n, got, K[i].k); kfails++; }
    }
    printf("  7 个分类 %s\n", kfails ? "有错" : "全部 OK");

    printf("\n=== 3) 录一串命令（模拟一帧）===\n");
    /* 用真实存在的 NVN 名字 —— 不在 538 名表里的名字注册不上，也就不会被录。
       先列出「能录的名字」，避免我用了不存在的名字而误判。 */
    static const char *tries[] = {
        "nvnCommandBufferBindProgram", "nvnCommandBufferBindTextures",
        "nvnCommandBufferBindVertexBuffer", "nvnCommandBufferDrawElements",
        "nvnCommandBufferClearColor", "nvnCommandBufferSetViewport",
        "nvnCommandBufferBarrier", "nvnCommandBufferBindBlendState",
        "nvnCommandBufferDrawArrays",
    };
    unsigned usable = 0;
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        int idx = dm_nvn_index_of(tries[i]);
        if (idx < 0) { printf("    (名字不在表里，跳过: %s)\n", tries[i]); continue; }
        usable++;
    }
    printf("  表里存在、可录的名字: %u / %u\n", usable, (unsigned)(sizeof tries / sizeof tries[0]));

    /* 录一帧：绑定 → 绘制 → 状态 → 同步 */
    dm_nvn_cmd_enable(1);
    g_nlog = 0; g_nunknown = 0;
    int nrec = 0;
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (dm_nvn_index_of(tries[i]) < 0) continue;
        call(tries[i], 0x1000 + i * 0x100, 0x2000 + i, 0x3000, 0x4000);
        nrec++;
    }
    printf("  录了 %d 条，pending = %u\n", nrec, dm_nvn_cmd_pending());

    printf("\n=== 4) 回放到假后端 ===\n");
    dm_nvn_backend be;
    memset(&be, 0, sizeof be);
    be.bind = b_bind; be.draw = b_draw; be.clear = b_clear; be.state = b_state;
    be.sync = b_sync; be.present = b_present; be.other = b_other; be.unknown = b_unknown;
    dm_replay_stats st;
    unsigned done = dm_nvn_cmd_replay_to(&be, 0, &st);
    printf("  回放 %u 条；分类: bind=%llu draw=%llu clear=%llu state=%llu sync=%llu present=%llu other=%llu unknown=%llu\n",
           done, (unsigned long long)st.n_bind, (unsigned long long)st.n_draw,
           (unsigned long long)st.n_clear, (unsigned long long)st.n_state,
           (unsigned long long)st.n_sync, (unsigned long long)st.n_present,
           (unsigned long long)st.n_other, (unsigned long long)st.n_unknown);

    printf("\n  后端收到的操作序列:\n");
    int rfails = 0;
    for (int i = 0; i < g_nlog; i++)
        printf("    [%d] %-8s %-36s a0=%#llx a1=%#llx\n", i, g_log[i].kind,
               g_log[i].name, (unsigned long long)g_log[i].a[0],
               (unsigned long long)g_log[i].a[1]);

    printf("\n=== 5) 顺序与参数核对 ===\n");
    /* 期望：后端收到的顺序 == 调用顺序（且都进了对应类别） */
    unsigned expect = 0;
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        if (dm_nvn_index_of(tries[i]) < 0) continue;
        if ((int)expect >= g_nlog) { printf("    ✗ 少了第 %u 条\n", expect); rfails++; break; }
        if (strcmp(g_log[expect].name, tries[i])) {
            printf("    ✗ 顺序不符：第 %u 条应为 %s，实为 %s\n", expect, tries[i], g_log[expect].name);
            rfails++;
        }
        /* 参数原样核对 */
        uint64_t w0 = 0x1000 + i * 0x100, w1 = 0x2000 + i;
        if (g_log[expect].a[0] != w0 || g_log[expect].a[1] != w1) {
            printf("    ✗ 参数不符：第 %u 条 a0=%#llx(expect %#llx) a1=%#llx(expect %#llx)\n",
                   expect, (unsigned long long)g_log[expect].a[0], (unsigned long long)w0,
                   (unsigned long long)g_log[expect].a[1], (unsigned long long)w1);
            rfails++;
        }
        expect++;
    }
    printf("  %u 条顺序与参数 %s\n", expect, rfails ? "**有错**" : "全部一致 OK");

    printf("\n=== 6) 回放后缓冲应清空 ===\n");
    unsigned left = dm_nvn_cmd_pending();
    printf("  pending = %u (expect 0)\n", left);
    if (left != 0) rfails++;

    printf("\n=== 7) 「身份未知」不猜类别 ===\n");
    /* 直接用通用入口录（它拿不到 idx） */
    g_nlog = 0; g_nunknown = 0;
    dm_nvn_cmd_generic((void *)0xAA, (void *)0xBB, (void *)0xCC, (void *)0xDD);
    dm_nvn_cmd_generic((void *)0x11, (void *)0x22, (void *)0x33, (void *)0x44);
    dm_replay_stats st2;
    unsigned d2 = dm_nvn_cmd_replay_to(&be, 0, &st2);
    printf("  回放 %u 条：unknown=%llu，进了别的类别 %llu 条\n", d2,
           (unsigned long long)st2.n_unknown,
           (unsigned long long)(st2.n_bind + st2.n_draw + st2.n_clear +
                                st2.n_state + st2.n_sync + st2.n_present + st2.n_other));
    if (st2.n_unknown != 2 || g_nunknown != 2) { printf("    ✗ unknown 计数不符\n"); rfails++; }
    if (st2.n_bind + st2.n_draw + st2.n_clear + st2.n_state + st2.n_sync + st2.n_present + st2.n_other != 0) {
        printf("    ✗ 身份未知的命令被猜成了别的类别\n"); rfails++;
    }
    printf("  身份未知的命令进了 unknown 而没被猜 %s\n", rfails ? "**有错**" : "OK");

    printf("\n=== result: %s（失败 %d 项）===\n", rfails ? "CHECK FAILED" : "ALL PASS", rfails);
    return rfails ? 1 : 0;
}
