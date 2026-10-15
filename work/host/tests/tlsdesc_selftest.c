/* 检查 NmplDLL 的 19 个匿名 TLSDESC 重定位。
 *
 * TLSDESC（type 1031）是 AArch64 的线程局部存储描述符重定位：
 * 链接器为一个 TLS 变量生成一对（resolver 函数, 变量偏移），
 * 首次访问时调 resolver 拿到偏移，之后直接用。
 *
 * 本自测要回答：
 *   1. 这些重定位在哪个段（.data.rel.ro？）
 *   2. 它们的符号下标是多少、名字为什么空
 *   3. 描述符里的初始内容是什么（是否已经是「(0, 偏移)」的形式）
 *   4. 宿主需要给它们什么才正确（偏移 + 宿主侧 TLS 基址）
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    char bin[512], fix[512];
    snprintf(bin, sizeof bin, "%s/NmplDLL.nro.patched.bin", dir);
    snprintf(fix, sizeof fix, "%s/NmplDLL.nro.fixups.tsv", dir);

    dm_load_ctx ctx;
    dm_loader_reset(&ctx);
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    dm_module *m = &ctx.mods[ctx.nmods];
    int rc = dm_loader_map(m, bin, fix, ctx.key_values);
    if (rc) { printf("map FAIL rc=%d (%s)\n", rc, dm_loader_err(rc)); return 1; }
    ctx.nmods++;

    printf("=== %s ===\n", m->name);
    printf("  base=%p size=%zu nsym=%u\n", (void *)m->base, m->size, m->nsym);
    printf("  rela=%llu jmprel=%llu\n",
           (unsigned long long)(m->relasz / DM_RELA_SIZE),
           (unsigned long long)(m->pltsz / DM_RELA_SIZE));

    printf("\n=== 扫描全部重定位里的 TLSDESC（type 1031）===\n");
    struct { unsigned si; uint64_t off; int64_t add; } hits[64];
    unsigned nh = 0;
    const dm_rela *tabs[2] = { m->rela, m->jmprel };
    uint64_t sizes[2] = { m->relasz, m->pltsz };
    for (int t = 0; t < 2; t++) {
        if (!tabs[t]) continue;
        uint64_t n = sizes[t] / DM_RELA_SIZE;
        for (uint64_t i = 0; i < n; i++) {
            const dm_rela *r = &tabs[t][i];
            unsigned type = (unsigned)(r->r_info & 0xFFFFFFFFu);
            unsigned si   = (unsigned)(r->r_info >> 32);
            if (type != 1031) continue;
            if (nh < 64) {
                hits[nh].si = si; hits[nh].off = r->r_offset; hits[nh].add = r->r_addend;
                nh++;
            }
        }
    }
    printf("  找到 %u 个 TLSDESC\n", nh);
    if (!nh) { printf("  （没有——那说明我看到的 1031 在别处）\n"); return 0; }

    printf("\n  前 20 个:\n");
    printf("    %-4s %-10s %-12s %-10s %s\n", "#", "sym_idx", "r_offset", "addend", "sym_name");
    for (unsigned i = 0; i < nh && i < 20; i++) {
        const char *nm = (hits[i].si < m->nsym) ? (m->strtab + m->symtab[hits[i].si].st_name) : "<越界>";
        unsigned shndx = (hits[i].si < m->nsym) ? m->symtab[hits[i].si].st_shndx : 0;
        uint64_t val   = (hits[i].si < m->nsym) ? m->symtab[hits[i].si].st_value : 0;
        printf("    %-4u %-10u %#-10llx %-12lld \"%s\" (shndx=%u value=%#llx)\n",
               i, hits[i].si, (unsigned long long)hits[i].off, (long long)hits[i].add,
               nm, shndx, (unsigned long long)val);
    }

    printf("\n=== 描述符处的初始内容（16 字节：resolver + 偏移）===\n");
    for (unsigned i = 0; i < nh && i < 6; i++) {
        uint64_t off = hits[i].off;
        if (off + 16 > m->size) { printf("    #%u 越界\n", i); continue; }
        uint64_t a = *(const uint64_t *)(m->base + off);
        uint64_t b = *(const uint64_t *)(m->base + off + 8);
        printf("    #%-2u @%#-9llx  [0]=%#-18llx  [1]=%#llx\n",
               i, (unsigned long long)off,
               (unsigned long long)a, (unsigned long long)b);
    }

    printf("\n=== 判定 ===\n");
    printf("  r_offset 落在哪个段（%s 的段边界见 fixups）:\n", m->name);
    printf("    text_end=%#llx  ro_off=%#llx  data_off=%#llx  image_size=%#llx\n",
           (unsigned long long)m->L.text_end, (unsigned long long)m->L.ro_off,
           (unsigned long long)m->L.data_off, (unsigned long long)m->L.image_size);
    unsigned in_ro = 0, in_data = 0, in_other = 0;
    for (unsigned i = 0; i < nh; i++) {
        if (hits[i].off >= m->L.ro_off && hits[i].off < m->L.data_off) in_ro++;
        else if (hits[i].off >= m->L.data_off && hits[i].off < m->L.image_size) in_data++;
        else in_other++;
    }
    printf("    在 ro    : %u\n    在 data  : %u\n    其他     : %u\n", in_ro, in_data, in_other);

    /* ------------------------------------------------------------------ */
    /* !! 第 46 轮新增：真的跑一遍重定位，核对描述符被填对了 !!
     *
     * 为什么必须核对：上一版把 desc[0] 填成 0，注释还写「resolver 已不需要」。
     * 而从解压模块反汇编可以看到客体**真的会 blr desc[0]**：
     *     adrp x0, <描述符页> ; ldr x1,[x0,#lo12] ; add x0,x0,#lo12 ; blr x1
     * 所以 desc[0]=0 就是 `blr 0` —— 一按就崩。这个自测如果只「数了 62 条」是发现不了的。 */
    dm_load_stats st;
    memset(&st, 0, sizeof st);
    int rrc = dm_loader_relocate(m, ctx.mods, ctx.nmods, &st);
    printf("\n=== 应用重定位后再看描述符（第 46 轮新增的核对）===\n");
    printf("  relocate rc=%d，TLSDESC %llu 条，relative %llu，未解析 %llu\n",
           rrc, (unsigned long long)st.n_tlsdesc,
           (unsigned long long)st.n_relative, (unsigned long long)st.n_unresolved);

    unsigned bad_resolver = 0, bad_offset = 0, checked = 0;
    for (unsigned i = 0; i < nh && i < 64; i++) {
        if (hits[i].off + 16 > m->size) continue;
        uint64_t a = *(const uint64_t *)(m->base + hits[i].off);
        uint64_t b = *(const uint64_t *)(m->base + hits[i].off + 8);
        checked++;
        if (a != (uint64_t)(uintptr_t)dm_tlsdesc_resolver) bad_resolver++;
        /* 偏移：具名符号用 st_value，匿名用 addend —— 与 loader.c 的规则一致 */
        uint64_t want = m->symtab[hits[i].si].st_value
                        ? m->symtab[hits[i].si].st_value : (uint64_t)hits[i].add;
        if (b != want) bad_offset++;
    }
    printf("  核对 %u 条：resolver 不对 %u 条、偏移不对 %u 条\n",
           checked, bad_resolver, bad_offset);
    printf("  resolver 地址 = %p（dm_tlsdesc_resolver）\n", (void *)(uintptr_t)dm_tlsdesc_resolver);
    printf("  说明：resolver 不能是 0 —— 客体取 TLS 变量时会 blr 它（blr 0 = 立刻崩）。\n");
    printf("        入参是**描述符地址**（不是 &desc[1]），所以实现是 `return desc[1]`。\n");

    int ok = (nh > 0) && (bad_resolver == 0) && (bad_offset == 0) && (checked > 0);
    printf("\n=== result: %s ===\n", ok ? "ALL PASS" : "FAIL");

    return ok ? 0 : 1;
}
