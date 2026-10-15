/* 诊断工具：问「sdk 里的某个地址到底是什么」。
 *
 * 起因（第 47 轮）：真机日志卡在
 *      [客体] 第 1 步：调用 _init_libc1（libc 锁初始化…）
 * 反汇编发现 sdk+0x56c210 只是个小函数，尾跳到 sdk+0x56c240，那里做的是：
 *      adrp x19, #0xb6a000
 *      ldr  x19, [x19, #0x488]     ← 从 GOT 槽取一个**指针**
 *      cbz  x0, ...
 *      str  x8, [x19]              ← 往它指向的地方写
 * 若这个 GOT 槽被重定位成「宿主函数的地址」（而不是「客体里某个全局的地址」），
 * 上面那句 `str` 就是往**宿主的代码页**里写 → 立刻出错。
 *
 * 本工具回答三件事：
 *   ① 这个偏移上有哪条重定位、它指向哪个符号
 *   ② 重定位前后那个槽里的值
 *   ③ 附近（0x56c000..0x56c400）还有哪些符号名 —— 用来确认 0x56c210 这个
 *      `_init_libc1` 到底是什么（这个名字是从 D2R 那边借来的叫法，本游戏的
 *       同名符号未必是「libc 锁初始化」）
 *
 * 用法：diag_sdk_addr.exe <patcher out 目录> [模块名=sdk] [偏移=0xb6a488]
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *dir   = (argc > 1) ? argv[1] : ".";
    const char *modn  = (argc > 2) ? argv[2] : "sdk";
    unsigned long long probe = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0xb6a488ULL;

    char bin[512], fix[512];
    snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, modn);
    snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, modn);

    dm_load_ctx ctx; dm_loader_reset(&ctx);
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    dm_module *m = &ctx.mods[ctx.nmods];
    int rc = dm_loader_map(m, bin, fix, ctx.key_values);
    if (rc) { printf("map FAIL rc=%d (%s)\n", rc, dm_loader_err(rc)); return 1; }
    ctx.nmods++;

    printf("=== %s  base=%p image=%#llx ===\n", m->name, (void *)m->base,
           (unsigned long long)m->size);

    /* ① 落在这个偏移上的重定位 */
    printf("\n=== 偏移 %#llx 上的重定位 ===\n", probe);
    int found = 0;
    const dm_rela *tabs[2]   = { m->rela, m->jmprel };
    const char    *tn[2]     = { "rela", "jmprel" };
    uint64_t       sizes[2]  = { m->relasz, m->pltsz };
    for (int t = 0; t < 2; t++) {
        if (!tabs[t]) continue;
        for (uint64_t i = 0; i < sizes[t] / DM_RELA_SIZE; i++) {
            const dm_rela *r = &tabs[t][i];
            if (r->r_offset != probe) continue;
            unsigned type = (unsigned)(r->r_info & 0xFFFFFFFFu);
            unsigned si   = (unsigned)(r->r_info >> 32);
            const char *nm = (si < m->nsym) ? m->strtab + m->symtab[si].st_name : "?";
            printf("  [%s] type=%u sym=%u name=\"%s\" addend=%lld\n",
                   tn[t], type, si, nm ? nm : "", (long long)r->r_addend);
            found++;
        }
    }
    if (!found) printf("  （没有重定位落在这里）\n");

    /* ② 值：重定位前 / 后 */
    printf("\n=== 槽里的值 ===\n");
    printf("  重定位前: %#llx\n", *(const unsigned long long *)(m->base + probe));
    dm_load_stats st; memset(&st, 0, sizeof st);
    int rrc = dm_loader_relocate(m, ctx.mods, ctx.nmods, &st);
    printf("  relocate rc=%d 未解析=%llu\n", rrc, (unsigned long long)st.n_unresolved);
    printf("  重定位后: %#llx\n", *(const unsigned long long *)(m->base + probe));

    /* ③ 附近的符号名 */
    if (argc > 4) {
        unsigned long long lo = strtoull(argv[4], NULL, 0);
        unsigned long long hi = (argc > 5) ? strtoull(argv[5], NULL, 0) : lo + 0x400;
        printf("\n=== 值在 [%#llx, %#llx) 的符号（最多 40 个）===\n", lo, hi);
        int n = 0;
        for (unsigned i = 0; i < m->nsym && n < 40; i++) {
            unsigned long long v = m->symtab[i].st_value;
            const char *nm = m->strtab + m->symtab[i].st_name;
            if (v >= lo && v < hi && nm && nm[0]) {
                printf("   %#-10llx %s\n", v, nm);
                n++;
            }
        }
        printf("  （共列出 %d 个）\n", n);
    }
    return 0;
}
