/* 宿主门分发的自测 —— 在 Windows 上直接验证 C 侧逻辑（不需要 aarch64 环境）。
 *
 * 用真实的 gates.tsv（改写器产出）驱动：
 *   1. 装载门表，核对条数与「返回点 → 类别/参数」映射
 *   2. 对每个 svc 门：喂一组寄存器，核对分发改到了 svc 分支
 *   3. 对每个 cntpct/cntvct 门：核对**结果写进了 arg 指定的寄存器**
 *      （这是关键 —— 目标可以是任意 xN，实测有 x19..x23）
 *   4. 对未登记的返回点：核对不改任何寄存器
 *
 * 打印一律用 ASCII（Windows 控制台代码页不可控，中文会乱码）。
 */
#include "../hle.h"

#include <stdio.h>
#include <string.h>

static uint64_t g_fake_ticks = 0;
static int g_svc_seen[256];

/* 模拟的系统调用实现：只记录被调到（不写 x0 —— x0 由分发器按语义决定）。
 * 对 0x1e（GetSystemTick）这类「x0 即返回值」的调用，实现要写 x0；
 * 对结果是「结果码」的调用，分发器会在成功时把 x0 置 0。 */
static int test_svc(uint64_t *r, uint32_t n) {
    (void)r;
    if (n < 256) g_svc_seen[n]++;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "gates.tsv";
    static dm_gate_ent tab[4096];
    dm_host h;
    memset(&h, 0, sizeof h);
    h.svc = test_svc;

    if (dm_host_load_gates(&h, tab, sizeof tab / sizeof tab[0], path)) {
        printf("load %s failed\n", path);
        return 1;
    }
    printf("=== gate table ===\n");
    printf("  entries: %zu\n", h.n);

    int n_svc = 0, n_cp = 0, n_cv = 0;
    uint32_t cp_regs[32] = {0};
    for (size_t i = 0; i < h.n; i++) {
        if (h.ents[i].kind == DM_GATE_SVC) n_svc++;
        else if (h.ents[i].kind == DM_GATE_CNTPCT) { n_cp++; if (h.ents[i].arg < 32) cp_regs[h.ents[i].arg]++; }
        else if (h.ents[i].kind == DM_GATE_CNTVCT) n_cv++;
    }
    printf("  svc %d, cntpct %d, cntvct %d\n", n_svc, n_cp, n_cv);
    printf("  cntpct target regs: ");
    for (int i = 0; i < 32; i++) if (cp_regs[i]) printf("x%d x%u  ", i, cp_regs[i]);
    printf("\n\n");

    /* 判据 0：返回点唯一 */
    int dup = 0;
    for (size_t i = 1; i < h.n; i++) if (h.ents[i].ret == h.ents[i-1].ret) dup++;
    printf("  duplicate return points: %d %s\n\n", dup, dup ? "<-- PROBLEM" : "(unique)");

    /* 判据 1：svc 门分发 —— 核对「调到了」且「x0 的语义正确」
     *   若该调用的 x0 是结果码：分发器成功时应置 0
     *   若是返回值类（GetSystemTick / GetProcessId / ...）：x0 由实现决定，分发器不置 0
     * 这里只验证分发器行为：对结果码类，实现不写 x0 → 分发后应为 0。 */
    printf("=== check 1: svc gate dispatch ===\n");
    int ok1 = 0, bad1 = 0;
    for (size_t i = 0; i < h.n && ok1 + bad1 < 8; i++) {
        if (h.ents[i].kind != DM_GATE_SVC) continue;
        uint64_t r[DM_REG_COUNT];
        for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xDEAD0000 + k;
        uint32_t n = h.ents[i].arg;
        g_svc_seen[n < 256 ? n : 0] = 0;
        dm_gate_dispatch(&h, h.ents[i].ret, r);
        int dispatched = (g_svc_seen[n < 256 ? n : 0] == 1);
        if (dispatched) {
            ok1++;
            if (ok1 <= 5) printf("  ret %#llx svc#%u (%s) -> dispatched, x0=%#llx\n",
                                 (unsigned long long)h.ents[i].ret, n, "impl",
                                 (unsigned long long)r[0]);
        } else bad1++;
    }
    printf("  pass %d, mismatch %d\n\n", ok1, bad1);

    /* 判据 2：cntpct/cntvct 门 —— 结果必须写进 arg 指定的寄存器 */
    printf("=== check 2: timer gate writes the right register ===\n");
    int ok2 = 0, bad2 = 0;
    g_fake_ticks = 0x1234567890ABCDEFull;
    for (size_t i = 0; i < h.n; i++) {
        if (h.ents[i].kind != DM_GATE_CNTPCT && h.ents[i].kind != DM_GATE_CNTVCT) continue;
        uint64_t r[DM_REG_COUNT];
        for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xDEAD0000 + k;
        uint32_t tgt = h.ents[i].arg;
        dm_gate_dispatch(&h, h.ents[i].ret, r);
        if (tgt >= DM_REG_COUNT) { bad2++; continue; }
        if (r[tgt] != g_fake_ticks) { bad2++; continue; }
        int other = 0;
        for (int k = 0; k < DM_REG_COUNT; k++)
            if (k != (int)tgt && r[k] != (uint64_t)(0xDEAD0000 + k)) other++;
        if (other) { bad2++; continue; }
        ok2++;
    }
    printf("  pass %d, mismatch %d\n", ok2, bad2);
    if (ok2) printf("  (timestamp %#llx written into each gate's arg register)\n\n",
                    (unsigned long long)g_fake_ticks);

    /* 判据 3：未登记的返回点不改任何寄存器 */
    printf("=== check 3: unregistered gate ===\n");
    uint64_t r[DM_REG_COUNT];
    for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xAAAA0000 + k;
    dm_gate_dispatch(&h, 0xFFFFFFFFFFFFull, r);
    int changed = 0;
    for (int k = 0; k < DM_REG_COUNT; k++) if (r[k] != (uint64_t)(0xAAAA0000 + k)) changed++;
    printf("  registers changed: %d %s\n", changed, changed ? "<-- PROBLEM" : "(untouched, correct)");
    printf("  unknown count: %llu\n\n", (unsigned long long)h.n_unknown);

    /* 判据 4：x0 的语义 —— 用**内建实现**验证
     *   GetSystemTick(0x1e)：x0 是返回值本身，应为 tick
     *   CloseHandle(0x16)：x0 是结果码，应为 0
     *   未实现的调用：x0 应为非 0（让客体知道失败，不把垃圾当句柄） */
    printf("=== check 4: x0 semantics ===\n");
    {
        dm_host h2;
        memset(&h2, 0, sizeof h2);
        h2.ents = h.ents; h2.n = h.n; h2.svc = NULL;   /* svc=NULL -> 走内建实现 */
        g_fake_ticks = 0xABCDEF123456789ull;

        const dm_gate_ent *e_tick = NULL, *e_close = NULL, *e_unimpl = NULL;
        for (size_t i = 0; i < h.n; i++) {
            if (h.ents[i].kind != DM_GATE_SVC) continue;
            if (h.ents[i].arg == 0x1e) e_tick = &h.ents[i];
            if (h.ents[i].arg == 0x16) e_close = &h.ents[i];
            if (h.ents[i].arg == 0x53) e_unimpl = &h.ents[i];   /* ReadWriteRegister：未实现 */
        }
        int ok4 = 0, bad4 = 0;
        uint64_t r[DM_REG_COUNT];
        if (e_tick) {
            for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0;
            dm_gate_dispatch(&h2, e_tick->ret, r);
            if (r[0] == g_fake_ticks) { ok4++; printf("  GetSystemTick -> x0=%#llx (== tick) OK\n",
                                                      (unsigned long long)r[0]); }
            else { bad4++; printf("  GetSystemTick -> x0=%#llx, expected %#llx\n",
                                  (unsigned long long)r[0], (unsigned long long)g_fake_ticks); }
        }
        if (e_close) {
            for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xFFFF;
            dm_gate_dispatch(&h2, e_close->ret, r);
            if (r[0] == 0) { ok4++; printf("  CloseHandle -> x0=0 (result code) OK\n"); }
            else { bad4++; printf("  CloseHandle -> x0=%#llx, expected 0\n", (unsigned long long)r[0]); }
        }
        if (e_unimpl) {
            for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0;
            dm_gate_dispatch(&h2, e_unimpl->ret, r);
            if (r[0] != 0) { ok4++; printf("  unimplemented svc#%u -> x0=%#llx (non-zero) OK\n",
                                           e_unimpl->arg, (unsigned long long)r[0]); }
            else { bad4++; printf("  unimplemented svc -> x0=0 (should be non-zero)\n"); }
        }
        printf("  pass %d, mismatch %d\n\n", ok4, bad4);
        bad1 += bad4;   /* 计入总判定 */
    }

    printf("=== stats ===\n");
    printf("  n_svc=%llu n_cntpct=%llu n_cntvct=%llu n_unknown=%llu\n",
           (unsigned long long)h.n_svc, (unsigned long long)h.n_cntpct,
           (unsigned long long)h.n_cntvct, (unsigned long long)h.n_unknown);

    int allbad = dup + bad1 + bad2 + changed;
    printf("\n=== result: %s ===\n", allbad ? "MISMATCHES" : "ALL PASS");
    return allbad ? 1 : 0;
}
