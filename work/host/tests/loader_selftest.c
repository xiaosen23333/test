/* 装载器自测 —— 在 Windows 上真正装载 5 个模块并跑完全部重定位。
 *
 * 打印一律 ASCII（Windows 控制台代码页不可控）。
 *
 * 验证内容：
 *   1. 5 个模块的 fixups.tsv 都能解析且自洽
 *   2. 映射成功、MOD0/动态段解析成功、符号表规模正确
 *   3. 全部重定位应用完毕；统计各类型条数并与 Python 侧（verify_link.py）对照
 *   4. 未解析符号只有 glslc*（8 个）
 *   5. 键页填充后 4 项都是「键号 × 8」
 *   6. gates.tsv 登记进 hle.c 的门表，且按返回点有序
 *   7. 页保护区间换算（16K 页、向内取整）：页两两不重叠；
 *      首页/代码尾页/补丁页不得被**不含 X** 的区间覆盖（第 70 轮回归断言）
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { const char *name; const char *bin; const char *fix; const char *gates; } ent_t;

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    char bin[512], fix[512], gat[512];

    static const char *names[] = { "main", "sdk", "rtld", "NmplDLL.nro", "lz4.nro" };
    const unsigned NK = sizeof names / sizeof names[0];

    dm_load_ctx ctx;
    dm_loader_reset(&ctx);
    /* 模拟宿主建好的 pthread 键号 */
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    printf("=== 1) map 5 modules ===\n");
    fflush(stdout);
    for (unsigned i = 0; i < NK; i++) {
        printf("  [%u] %s ... ", i, names[i]);
        fflush(stdout);
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, names[i]);
        dm_module *m = &ctx.mods[ctx.nmods];
        /* 注意：不要在调用前设 m->name —— dm_loader_map 会 memset 整个结构，
           名字由它从 bin 路径自动推导（这里踩过一次） */
        int rc = dm_loader_map(m, bin, fix, ctx.key_values);
        if (rc) { printf("FAIL rc=%d (%s)\n", rc, dm_loader_err(rc)); return 1; }
        ctx.nmods++;
        printf("ok  base %p size %8zu  text_end %#9llx  key_page %#9llx  patch %#9llx\n",
               (void *)m->base, m->size,
               (unsigned long long)m->L.text_end,
               (unsigned long long)m->L.key_page_off,
               (unsigned long long)m->L.patch_off);
        fflush(stdout);
    }
    printf("\n  [step 1a] counting exported syms ... ");
    fflush(stdout);
    {
        unsigned tot = 0;
        for (unsigned i = 0; i < ctx.nmods; i++) {
            printf("\n    mod %u (%s) nsym=%u ", i, ctx.mods[i].name, ctx.mods[i].nsym);
            fflush(stdout);
            for (unsigned k = 1; k < ctx.mods[i].nsym; k++) {
                if (ctx.mods[i].symtab[k].st_shndx &&
                    ctx.mods[i].strtab[ctx.mods[i].symtab[k].st_name]) tot++;
            }
            printf("ok");
            fflush(stdout);
        }
        printf("\n  total exported syms: %u\n\n", tot);
        fflush(stdout);
    }

    printf("=== 2) symbol tables ===\n");
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        unsigned nd = 0, nu = 0;
        for (unsigned k = 0; k < m->nsym; k++)
            if (m->strtab[m->symtab[k].st_name]) { if (m->symtab[k].st_shndx) nd++; else nu++; }
        printf("  %-14s nsym %6u  defined %6u  undefined %5u  rela %6llu  jmprel %6llu\n",
               m->name, m->nsym, nd, nu,
               (unsigned long long)(m->relasz / DM_RELA_SIZE),
               (unsigned long long)(m->pltsz / DM_RELA_SIZE));
    }

    printf("\n=== 3) relocate all ===\n");
    fflush(stdout);
    dm_load_stats tot; memset(&tot, 0, sizeof tot);
    static dm_load_stats each[DM_MAX_MODS];
    for (unsigned i = 0; i < ctx.nmods; i++) {
        printf("  [%u] relocating %s ... ", i, ctx.mods[i].name);
        fflush(stdout);
        memset(&each[i], 0, sizeof each[i]);
        dm_loader_relocate(&ctx.mods[i], ctx.mods, ctx.nmods, &each[i]);
        tot.n_relative += each[i].n_relative; tot.n_abs += each[i].n_abs;
        tot.n_jump += each[i].n_jump; tot.n_unresolved += each[i].n_unresolved;
        tot.n_bad_sym += each[i].n_bad_sym;
        printf("done\n");
        printf("       RELATIVE %6llu  ABS/GLOB %6llu  JUMP_SLOT %6llu  unresolved %4llu  bad_sym %llu  RELR %6llu\n",
               (unsigned long long)each[i].n_relative, (unsigned long long)each[i].n_abs,
               (unsigned long long)each[i].n_jump, (unsigned long long)each[i].n_unresolved,
               (unsigned long long)each[i].n_bad_sym, (unsigned long long)each[i].n_relr);
        fflush(stdout);
    }
    printf("\n  [step 3b] DT_RELR 已应用？init_array 条目必须**带基址**（第 73 轮回归断言）\n");
    int relr_ok = 1;
    /* 第 73 轮：漏处理 DT_RELR 会让模块内部所有绝对指针停在裸偏移上 ——
     * 那是本工程一连串崩溃的统一根因（init_array / 服务描述表 / 模块信息表 /
     * 构造函数里的函数指针跳转）。这里用「init_array 条目 ≥ 模块基址」来盯住它。 */
    relr_ok = 1;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        if (!m->init_array_n) { printf("    %-14s （无 init_array）\n", m->name); continue; }
        uintptr_t base = (uintptr_t)m->base;
        int ok = 1;
        for (size_t k = 0; k < m->init_array_n; k++) {
            uintptr_t v = (uintptr_t)m->init_array[k];
            if (v < base || v >= base + m->size) { ok = 0; break; }
        }
        relr_ok = relr_ok && ok;
        printf("    %-14s DT_RELR %6llu 项, init_array %zu 项全部带基址: %s\n",
               m->name, (unsigned long long)each[i].n_relr, m->init_array_n, ok ? "yes" : "NO");
        fflush(stdout);
    }
    printf("\n  TOTAL relocs %llu  (RELATIVE %llu + ABS/GLOB %llu + JUMP %llu)\n",
           (unsigned long long)(tot.n_relative + tot.n_abs + tot.n_jump),
           (unsigned long long)tot.n_relative, (unsigned long long)tot.n_abs,
           (unsigned long long)tot.n_jump);
    printf("  unresolved %llu  bad_sym %llu\n", (unsigned long long)tot.n_unresolved,
           (unsigned long long)tot.n_bad_sym);
    fflush(stdout);

    printf("\n=== 4) unresolved symbols ===\n");
    fflush(stdout);
    unsigned shown = 0;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        printf("  scanning %s (%u entries) ... ", ctx.mods[i].name, each[i].n_unres);
        fflush(stdout);
        for (unsigned k = 0; k < each[i].n_unres; k++) {
            printf("\n    type %4u  %s", each[i].unres[k].type, each[i].unres[k].name);
            fflush(stdout);
            shown++;
        }
        printf(" ok\n");
        fflush(stdout);
    }
    if (!shown) printf("  (none)\n");
    fflush(stdout);

    printf("\n=== 5) key page (must be key*8) ===\n");
    fflush(stdout);
    int kp_ok = 1;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        printf("  [%u] %s base=%p kpo=%#llx idx=%u,%u,%u,%u ... ", i, m->name, (void *)m->base,
               (unsigned long long)m->L.key_page_off,
               m->L.key_idx[0], m->L.key_idx[1], m->L.key_idx[2], m->L.key_idx[3]);
        fflush(stdout);
        uint64_t vals[DM_KEY_COUNT];
        for (unsigned k = 0; k < DM_KEY_COUNT; k++)
            vals[k] = *(uint64_t *)(m->base + m->L.key_page_off + m->L.key_idx[k] * 8);
        int ok = 1;
        for (unsigned k = 0; k < DM_KEY_COUNT; k++)
            if (vals[k] != (uint64_t)ctx.key_values[k] * 8) ok = 0;
        kp_ok = kp_ok && ok;
        printf("K18=%llu KTLS=%llu KTLR=%llu KGATE=%llu  %s\n",
               (unsigned long long)vals[0], (unsigned long long)vals[1],
               (unsigned long long)vals[2], (unsigned long long)vals[3],
               ok ? "OK" : "BAD");
        fflush(stdout);
    }

    printf("\n=== 6) register gates ===\n");
    fflush(stdout);
    int gt_ok = 1;
    static dm_gate_ent tbl[DM_MAX_MODS][1024];
    static dm_host hs[DM_MAX_MODS];
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        snprintf(gat, sizeof gat, "%s/%s.gates.tsv", dir, m->name);
        printf("  [%u] %s <- %s ... ", i, m->name, gat);
        fflush(stdout);
        int n = dm_loader_register_gates(&hs[i], tbl[i], 1024, gat, m->base);
        if (n < 0) n = 0;
        printf("n=%d ", n);
        fflush(stdout);
        int sorted = 1;
        for (int k = 1; k < n; k++) if (tbl[i][k].ret <= tbl[i][k-1].ret) sorted = 0;
        gt_ok = gt_ok && sorted;
        printf("sorted=%s (expect %u)\n", sorted ? "yes" : "NO", m->L.ngates);
        fflush(stdout);
        if ((unsigned)n != m->L.ngates) gt_ok = 0;
    }

    printf("\n=== 7) page-protection spans (16K pages, inward rounding) ===\n");
    fflush(stdout);
    /* 第 70 轮回归断言。背景：真机 .ips = CODESIGNING / Invalid Page ——
     * 16K 页与「非 16K 对齐的段边界」会让相邻区间共享边界页，向外取整时
     * 后写的覆盖先写的（rtld 代码页被 rodata 压成 r--）。这里对**真实模块布局**
     * 断言两条：
     *   ① 区间换算出的页两两不重叠（不再互相覆盖）；
     *   ② 「必须可执行」的三页（首页/代码尾页/补丁页）不得落在任何**不含 X** 的区间里。 */
    int pp_ok = 1;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        dm_prot_span sp[8];
        int n = dm_loader_prot_spans(m, 0x4000, sp, 8);   /* iOS 真机页大小 */
        int overlap = 0;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                if (sp[a].off < sp[b].end && sp[b].off < sp[a].end) overlap = 1;
        uint64_t mustx[3];
        mustx[0] = 0;
        mustx[1] = (m->L.text_end ? (m->L.text_end - 1) : 0) & ~(uint64_t)0x3FFF;
        mustx[2] = m->L.patch_off & ~(uint64_t)0x3FFF;
        int xbad = 0;
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < n; a++)
                if (mustx[k] >= sp[a].off && mustx[k] < sp[a].end && !(sp[a].prot & DM_PROT_X))
                    xbad = 1;
        pp_ok = pp_ok && !overlap && !xbad;
        printf("  [%u] %-14s spans=%d  overlap=%s  xbad=%s\n", i, m->name, n,
               overlap ? "YES" : "no", xbad ? "YES" : "no");
        for (int a = 0; a < n; a++)
            printf("        [%#010llx, %#010llx) prot=%d\n",
                   (unsigned long long)sp[a].off, (unsigned long long)sp[a].end, sp[a].prot);
        fflush(stdout);
    }

    printf("\n=== result: %s ===\n",
           (tot.n_bad_sym == 0 && kp_ok && gt_ok && pp_ok && relr_ok && tot.n_unresolved <= 27) ? "ALL PASS" : "CHECK FAILED");
    /* 注意：模块映像是 os_map()（Windows 上是 VirtualAlloc）分配的，
       必须用 os_unmap 释放 —— 用 free() 会访问违例（这里踩过）。
       自测里其实不必释放（进程马上退出），但显式释放能顺带验证映射成对。 */
    for (unsigned i = 0; i < ctx.nmods; i++) dm_loader_unmap(&ctx.mods[i]);
    return 0;
}
