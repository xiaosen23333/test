/* 宿主门分发的自测 —— 在 Windows 上直接验证 C 侧逻辑（不需要 aarch64 环境）。
 *
 * 用真实的 gates.tsv（改写器产出）驱动；**命令行上给的每个门表文件都独立跑一遍**。
 * （第 59 轮修：之前只传 main.gates.tsv，而它只有注释、0 条门 —— 整项测试在空转，
 *   是一盏假绿灯。现在由 build_all.ps1 传 sdk/rtld/main 三个，sdk 有 101 条门。）
 *
 * 判据：
 *   0. 门表装载：返回点唯一
 *   1. svc 门分发：喂一组寄存器，核对确实分发到了 svc 分支
 *   2. cntpct/cntvct 门：**结果写进 arg 指定的寄存器**（目标可以是任意 xN）
 *   3. 未登记的返回点：不改任何寄存器
 *   4. x0 的语义：GetSystemTick(0x1e) 是「返回值」、CloseHandle(0x16) 是「结果码」
 *   5. 未实现的调用 → 假成功（x0=0、其它寄存器原样；第 59 轮策略，防 SDK 错误路径）
 *   6. **句柄不是指针**：svc 0x21/0x22 收到会话句柄值（0xC0000002）时绝不许 deref
 *      —— deref 的话本测试进程当场崩（正是第 59 轮真机 .ips 的复现）
 *   7. TLR 应答头改写（第 60 轮）：svc 0x21 看到 TLR 头是 Horizon IPC 请求形态时，
 *      必须把它改成应答形态并清探针，否则 SDK 会 Abort。Windows 自测通过
 *      dm_test_tlr_override 指向本地缓冲区来验证这条路径。
 *
 * 打印一律用 ASCII（Windows 控制台代码页不可控，中文会乱码）。
 */
#include "../hle.h"
#include "../hle_plat.h"   /* dm_test_tlr_override：覆盖 TLR 地址做回归测试 */

#include <stdio.h>
#include <string.h>

static int g_svc_seen[256];

/* 模拟的系统调用实现：只记录被调到（不写 x0 —— x0 由分发器按语义决定）。 */
static int test_svc(uint64_t *r, uint32_t n) {
    (void)r;
    if (n < 256) g_svc_seen[n]++;
    return 0;
}

/* ---- 单个门表文件：跑全部判据，返回 mismatch 总数 ---- */
static int run_file(const char *path) {
    static dm_gate_ent tab[4096];
    dm_host h;
    memset(&h, 0, sizeof h);
    h.svc = test_svc;

    printf("---- gates: %s ----\n", path);
    if (dm_host_load_gates(&h, tab, sizeof tab / sizeof tab[0], path)) {
        printf("  load failed\n\n");
        return 1;
    }
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
    printf("\n");

    int bad_all = 0;

    /* 判据 0：返回点唯一 */
    int dup = 0;
    for (size_t i = 1; i < h.n; i++) if (h.ents[i].ret == h.ents[i-1].ret) dup++;
    printf("  [0] duplicate return points: %d %s\n", dup, dup ? "<-- PROBLEM" : "(unique)");
    bad_all += dup;

    /* 判据 1：svc 门分发（最多看前 8 条） */
    int ok1 = 0, bad1 = 0;
    for (size_t i = 0; i < h.n && ok1 + bad1 < 8; i++) {
        if (h.ents[i].kind != DM_GATE_SVC) continue;
        uint64_t r[DM_REG_COUNT];
        for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xDEAD0000 + k;
        uint32_t n = h.ents[i].arg;
        g_svc_seen[n < 256 ? n : 0] = 0;
        dm_gate_dispatch(&h, h.ents[i].ret, r);
        if (g_svc_seen[n < 256 ? n : 0] == 1) ok1++;
        else bad1++;
    }
    printf("  [1] svc dispatch: pass %d, mismatch %d\n", ok1, bad1);
    bad_all += bad1;

    /* 判据 2：cntpct/cntvct 门 —— 结果必须写进 arg 指定的寄存器，别的不许动 */
    int ok2 = 0, bad2 = 0;
    for (size_t i = 0; i < h.n; i++) {
        if (h.ents[i].kind != DM_GATE_CNTPCT && h.ents[i].kind != DM_GATE_CNTVCT) continue;
        uint64_t r[DM_REG_COUNT];
        for (int k = 0; k < DM_REG_COUNT; k++) r[k] = 0xDEAD0000 + k;
        uint32_t tgt = h.ents[i].arg;
        dm_gate_dispatch(&h, h.ents[i].ret, r);
        if (tgt >= DM_REG_COUNT) { bad2++; continue; }
        if (r[tgt] == (uint64_t)(0xDEAD0000 + tgt) || r[tgt] == 0) { bad2++; continue; }
        int other = 0;
        for (int k = 0; k < DM_REG_COUNT; k++)
            if (k != (int)tgt && r[k] != (uint64_t)(0xDEAD0000 + k)) other++;
        if (other) { bad2++; continue; }
        ok2++;
    }
    printf("  [2] timer gate writes arg register: pass %d, mismatch %d\n", ok2, bad2);
    bad_all += bad2;

    /* 判据 3：未登记的返回点不改任何寄存器 */
    uint64_t r3[DM_REG_COUNT];
    for (int k = 0; k < DM_REG_COUNT; k++) r3[k] = 0xAAAA0000 + k;
    dm_gate_dispatch(&h, 0xFFFFFFFFFFFFull, r3);
    int changed = 0;
    for (int k = 0; k < DM_REG_COUNT; k++) if (r3[k] != (uint64_t)(0xAAAA0000 + k)) changed++;
    printf("  [3] unregistered gate: registers changed %d %s\n", changed,
           changed ? "<-- PROBLEM" : "(untouched, correct)");
    bad_all += changed;

    /* 判据 4~6 用内建实现（svc=NULL）—— 并顺便找好各自要用的门 */
    dm_host h2;
    memset(&h2, 0, sizeof h2);
    h2.ents = h.ents; h2.n = h.n;

    const dm_gate_ent *e_tick = NULL, *e_close = NULL, *e_un = NULL, *e21 = NULL, *e22 = NULL;
    for (size_t i = 0; i < h.n; i++) {
        if (h.ents[i].kind != DM_GATE_SVC) continue;
        switch (h.ents[i].arg) {
        case 0x1e: e_tick  = &h.ents[i]; break;
        case 0x16: e_close = &h.ents[i]; break;
        case 0x21: e21     = &h.ents[i]; break;
        case 0x22: e22     = &h.ents[i]; break;
        case 0x02: case 0x07: e_un = &h.ents[i]; break;   /* 已知未实现：SetMemoryPermission / ExitProcess */
        default: break;
        }
    }
    uint64_t r4[DM_REG_COUNT];

    /* 判据 4：x0 的语义 */
    int ok4 = 0, bad4 = 0;
    if (!e_tick) printf("  [4] (no svc 0x1e in table; skipped)\n");
    else {
        uint64_t a = 0, b = 0;
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0;
        dm_gate_dispatch(&h2, e_tick->ret, r4); a = r4[0];
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0;
        dm_gate_dispatch(&h2, e_tick->ret, r4); b = r4[0];
        if (a && b > a) { ok4++; printf("  [4] GetSystemTick -> x0 %llu then %llu (value, not result code) OK\n",
                                        (unsigned long long)a, (unsigned long long)b); }
        else { bad4++; printf("  [4] GetSystemTick -> x0 %llu then %llu (expected increasing non-zero)\n",
                              (unsigned long long)a, (unsigned long long)b); }
    }
    if (e_close) {
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0xFFFF;
        dm_gate_dispatch(&h2, e_close->ret, r4);
        if (r4[0] == 0) { ok4++; printf("  [4] CloseHandle -> x0=0 (result code) OK\n"); }
        else { bad4++; printf("  [4] CloseHandle -> x0=%#llx, expected 0\n", (unsigned long long)r4[0]); }
    }
    bad_all += bad4;

    /* 判据 5：未实现的调用 -> 假成功（x0=0、其它寄存器原样） */
    int bad5 = 0;
    if (!e_un) printf("  [5] (no known-unimplemented svc in table; skipped)\n");
    else {
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0xDEAD0000 + k;
        dm_gate_dispatch(&h2, e_un->ret, r4);
        int chg = 0;
        for (int k = 0; k < DM_REG_COUNT; k++)
            if (k != DM_REG_X0 && r4[k] != (uint64_t)(0xDEAD0000 + k)) chg++;
        if (r4[DM_REG_X0] == 0 && chg == 0) {
            printf("  [5] svc#%u (unimplemented) -> x0=0, other regs untouched OK\n", e_un->arg);
        } else {
            bad5++;
            printf("  [5] svc#%u -> x0=%#llx, other-changed=%d (expected x0=0 and others untouched)\n",
                   e_un->arg, (unsigned long long)r4[0], chg);
        }
    }
    bad_all += bad5;

    /* 判据 6：句柄不是指针（第 59 轮 .ips 复现）—— deref 会直接崩掉本测试进程 */
    int bad6 = 0;
    if (!e21 && !e22) printf("  [6] (no svc 0x21/0x22 in table; skipped)\n");
    if (e21) {
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0x11110000 + k;
        r4[DM_REG_X0] = 0xC0000002ull;      /* 会话句柄值 */
        dm_gate_dispatch(&h2, e21->ret, r4);   /* 若把它当缓冲读，这里就崩了 */
        if (r4[DM_REG_X0] == 0) { printf("  [6] svc21 handle-as-x0: no deref, x0=0 OK\n"); }
        else { bad6++; printf("  [6] svc21 -> x0=%#llx (expected 0)\n", (unsigned long long)r4[0]); }
    }
    if (e22) {
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0x22220000 + k;
        r4[DM_REG_X0] = 0xC0000002ull;      /* 再喂一次「句柄当缓冲」的形态 */
        r4[DM_REG_X2] = 0xC0000001ull;
        dm_gate_dispatch(&h2, e22->ret, r4);
        if (r4[DM_REG_X0] == 0) { printf("  [6] svc22 bogus buf: no deref, x0=0 OK\n"); }
        else { bad6++; printf("  [6] svc22 -> x0=%#llx (expected 0)\n", (unsigned long long)r4[0]); }
    }
    bad_all += bad6;

    /* 判据 7：TLR 应答头改写（r60）。Windows 通过覆盖 dm_test_tlr_override
     * 指向本地缓冲区，验证内建实现确实把请求头 0x8000000000000010 + 探针 1
     * 改成应答头 0x0000000100000010 + 探针 0。 */
    int bad7 = 0;
    if (e21) {
        uint64_t tlr_buf[4] = { 0 };
        volatile uint32_t *wb = (volatile uint32_t *)tlr_buf;
        volatile uint64_t *hb = (volatile uint64_t *)tlr_buf;
        /* 请求形态：h[0]=0x8000000000000010, w[2]=1 */
        hb[0] = 0x8000000000000010ULL;
        wb[2] = 1;
        dm_test_tlr_override = (uint64_t)tlr_buf;
        for (int k = 0; k < DM_REG_COUNT; k++) r4[k] = 0;
        r4[DM_REG_X0] = 0xC0000002ull;
        dm_gate_dispatch(&h2, e21->ret, r4);
        dm_test_tlr_override = 0;   /* 恢复 */
        if (r4[DM_REG_X0] == 0 && hb[0] == 0x0000000100000010ULL && wb[2] == 0)
            printf("  [7] svc21 TLR response header: ok (probe 1->0) OK\n");
        else {
            bad7++;
            printf("  [7] svc21 TLR response header: x0=%#llx hdr=%#llx probe=%u (expected x0=0 hdr=0x100000010 probe=0)\n",
                   (unsigned long long)r4[0], (unsigned long long)hb[0], wb[2]);
        }
    }
    bad_all += bad7;

    printf("  file total mismatches: %d\n\n", bad_all);
    return bad_all;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: host_selftest <gates.tsv> [more gates.tsv ...]\n");
        return 2;
    }
    int total = 0, nok = 0, nbad = 0;
    for (int i = 1; i < argc; i++) {
        int bad = run_file(argv[i]);
        if (bad) { nbad++; total += bad; }
        else nok++;
    }
    printf("=== files: %d ok, %d with mismatches ===\n", nok, nbad);
    printf("=== result: %s ===\n", total ? "MISMATCHES" : "ALL PASS");
    return total ? 1 : 0;
}
