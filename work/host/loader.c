/* 真机装载器 —— 把 5 个改写后的模块装载、链接、填键页、登记宿主门。
 *
 * 对应 dm/host/loader.c，但已按本游戏的实际数据定制：
 *   - 5 个模块：main / sdk / rtld（NSO，来自 ExeFS）+ NmplDLL.nro / lz4.nro（NRO，来自 RomFS）
 *   - 布局清单来自改写器产出的 <模块>.fixups.tsv（键名与 loader.c 的 read_layout 对齐）
 *   - 宿主门来自 <模块>.gates.tsv
 *   - 符号解析跨全部模块（本游戏共 29,671 个导出符号）
 *
 * 平台差异：
 *   - macOS/iOS：用 mmap + mprotect（本文件里的 dm_loader_map 就是这套）
 *   - Windows（自测）：用 VirtualAlloc 起同等语义的映射，使本文件能在 Windows 上编译并
 *     真正跑完「解析 + 重定位 + 键页填充 + 门登记」，从而验证装载逻辑
 *     （Windows 上没有 aarch64 执行环境，所以只验证「装载结果」，不执行客体代码）
 *
 * 实测数据（见 work/porting/PHASE2-LINK-REPORT.md）：
 *   main 623 条重定位、sdk 19,027、rtld 2、NmplDLL 4,963、lz4 45 —— 共 24,660 条
 *   其中 R_AARCH64_TLSDESC 62 条需要宿主支持
 */
#include "hle.h"
#include "loader.h"
#include "hle_diag.h"    /* 页保护失败时记进诊断日志 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- TLSDESC 的 resolver（第 46 轮加） ----------------
 *
 * AArch64 TLSDESC 的调用约定，从本游戏解压后的模块里反汇编确认：
 *     adrp x0, <描述符页>
 *     ldr  x1, [x0, #lo12]     ; x1 = desc[0] = 本函数
 *     add  x0, x0, #lo12       ; x0 = &desc[0]  ← **入参是描述符地址**
 *     blr  x1                  ; 出参 x0 = 要加到线程指针上的偏移
 * 所以本函数只做一件事：把 desc[1]（装载器已经填好的 TLS 偏移）返回。
 *
 * 为什么参数是 `uint64_t *`（= 描述符首地址）而不是别的：见上面的 `add x0, x0, #lo12`——
 * 传进来的是描述符本身的地址，不是 &desc[1]。按 &desc[1] 写会取到错误的字段。
 *
 * 平台注意：这是普通 C 函数，Apple 的 ABI 保留 x18，所以它不会破坏客体的线程指针；
 * 客体取 TLS 的那些序列也不会跨调用保活 x18。
 * 它会被**客体**在客体栈上调用（AAPCS64 与宿主一致），所以这里不需要任何平台差异。 */
uint64_t dm_tlsdesc_resolver(uint64_t *desc) {
    return desc ? desc[1] : 0;
}

/* ---------------- 平台抽象 ---------------- */

#if defined(_WIN32)
  #include <windows.h>
  static void *os_map(size_t n) {
      void *p = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      return p;
  }
  static void os_unmap(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
  static size_t os_page(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwPageSize; }
  static int os_protect(void *p, size_t n, int prot) {
      DWORD np = PAGE_READONLY;
      int r = prot & DM_PROT_R, w = prot & DM_PROT_W, x = prot & DM_PROT_X;
      if (x)      np = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
      else if (w) np = PAGE_READWRITE;
      else if (r) np = PAGE_READONLY;
      DWORD old;
      return VirtualProtect(p, n, np, &old) ? 0 : -1;
  }
#else
  #include <sys/mman.h>
  #include <unistd.h>
  static void *os_map(size_t n) {
      void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      return (p == MAP_FAILED) ? NULL : p;
  }
  static void os_unmap(void *p, size_t n) { munmap(p, n); }
  static size_t os_page(void) { return (size_t)getpagesize(); }
  static int os_protect(void *p, size_t n, int prot) {
      int np = 0;
      if (prot & DM_PROT_R) np |= PROT_READ;
      if (prot & DM_PROT_W) np |= PROT_WRITE;
      if (prot & DM_PROT_X) np |= PROT_EXEC;
      return mprotect(p, n, np);
  }
#endif

static dm_load_ctx g_ctx;

void dm_loader_reset(dm_load_ctx *c) {
    memset(c, 0, sizeof *c);
}

/* ---------------- 文件读取 ---------------- */

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)sz);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return p;
}

/* ---------------- fixups.tsv ---------------- */

/* 复刻 dm/host/loader.c:read_layout 的解析：逐行 `# <KEY> <值>`（tab 分隔）。
   注意 loader.c 用 sscanf("# %31s %lx")，空白分隔也接受；这里同时容忍 tab 与空格。 */
int dm_loader_read_fixups(dm_layout *L, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    memset(L, 0, sizeof *L);
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '#') continue;
        s++;
        while (*s == ' ' || *s == '\t') s++;
        /* 拆键与值：tab 或空白 */
        char key[64] = {0};
        size_t k = 0;
        while (*s && *s != '\t' && *s != ' ' && *s != '\n' && *s != '\r' && k + 1 < sizeof key)
            key[k++] = *s++;
        while (*s == ' ' || *s == '\t') s++;
        if (!key[0]) continue;
        if (!strcmp(key, "BUILD_ID")) {
            size_t i = 0;
            while (*s && *s != '\n' && *s != '\r' && i + 1 < sizeof L->build_id) L->build_id[i++] = *s++;
            continue;
        }
        long long v = strtoll(s, NULL, 0);
        if      (!strcmp(key, "PATCH_OFF"))      L->patch_off = (uint64_t)v;
        else if (!strcmp(key, "KEY_PAGE_OFF"))   L->key_page_off = (uint64_t)v;
        else if (!strcmp(key, "KEY_PAGE_SIZE"))  L->key_page_size = (uint64_t)v;
        else if (!strcmp(key, "TEXT_END"))       L->text_end = (uint64_t)v;
        else if (!strcmp(key, "RO_OFF"))         L->ro_off = (uint64_t)v;
        else if (!strcmp(key, "DATA_OFF"))       L->data_off = (uint64_t)v;
        else if (!strcmp(key, "IMAGE_SIZE"))     L->image_size = (uint64_t)v;
        else if (!strcmp(key, "PATCH_LEN"))      L->patch_len = (uint64_t)v;
        else if (!strcmp(key, "NGATES"))         L->ngates = (uint32_t)v;
        else if (!strcmp(key, "KEY_K18_IDX"))    L->key_idx[DM_KEY_K18]  = (uint32_t)v;
        else if (!strcmp(key, "KEY_KTLS_IDX"))   L->key_idx[DM_KEY_KTLS] = (uint32_t)v;
        else if (!strcmp(key, "KEY_KTLR_IDX"))   L->key_idx[DM_KEY_KTLR] = (uint32_t)v;
        else if (!strcmp(key, "KEY_KGATE_IDX"))  L->key_idx[DM_KEY_KGATE]= (uint32_t)v;
    }
    fclose(f);
    /* 自洽性检查（loader.c 也做这些断言） */
    if (!L->patch_off || !L->key_page_size) return -2;
    if (L->key_page_off % 0x4000) return -3;
    if (L->patch_off != L->key_page_off + 0x4000) return -4;   /* 补丁区须紧接键页之后 */
    return 0;
}

/* ---------------- 模块映射 ---------------- */

int dm_loader_map(dm_module *m, const char *bin_path, const char *fixups_path,
                  unsigned key_values[DM_KEY_COUNT]) {
    memset(m, 0, sizeof *m);
    /* 模块名从 bin 路径推导（调用方不必在调用前设置 —— 本函数会 memset 整个结构，
       所以「调用前设名字」会被清掉；这一点我在自测里踩过） */
    {
        const char *b = strrchr(bin_path, '/');
#if defined(_WIN32)
        const char *b2 = strrchr(bin_path, '\\');
        if (b2 && (!b || b2 > b)) b = b2;
#endif
        b = b ? b + 1 : bin_path;
        size_t n = strlen(b);
        const char *suf = ".patched.bin";
        size_t sl = strlen(suf);
        if (n > sl && !strcmp(b + n - sl, suf)) n -= sl;
        if (n >= sizeof m->name) n = sizeof m->name - 1;
        memcpy(m->name, b, n);
        m->name[n] = 0;
    }
    if (dm_loader_read_fixups(&m->L, fixups_path)) return -1;

    size_t sz = 0;
    uint8_t *raw = slurp(bin_path, &sz);
    if (!raw) return -2;
    if (sz != m->L.image_size) { free(raw); return -3; }

    /* 16 KB 页对齐（Apple 的页大小就是 16 KB，与 Horizon 一致） */
    m->size = (sz + 0x3FFF) & ~(size_t)0x3FFF;
    m->base = (uint8_t *)os_map(m->size);
    if (!m->base) { free(raw); return -4; }
    memcpy(m->base, raw, sz);
    free(raw);

    /* 键页：写「pthread 键号 × 8」，并把它设为只读（真机侧；Windows 侧跳过） */
    if (m->L.key_page_off + m->L.key_page_size > m->size) return -5;
    uint64_t *page = (uint64_t *)(m->base + m->L.key_page_off);
    memset(page, 0, (size_t)m->L.key_page_size);
    for (unsigned i = 0; i < DM_KEY_COUNT; i++) {
        if (m->L.key_idx[i] >= m->L.key_page_size / 8) return -6;
        page[m->L.key_idx[i]] = (uint64_t)key_values[i] * 8;
    }

    /* 解析 MOD0 与动态段 */
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) return -7;
    m->dyn = (const dm_dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const dm_dyn *d = m->dyn; d->d_tag; d++)
        switch (d->d_tag) {
        case DM_DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DM_DT_SYMTAB: m->symtab = (const dm_sym *)(m->base + d->d_un.d_ptr); break;
        case DM_DT_RELA:   m->rela   = (const dm_rela *)(m->base + d->d_un.d_ptr); break;
        case DM_DT_RELASZ: m->relasz = d->d_un.d_val; break;
        case DM_DT_JMPREL: m->jmprel = (const dm_rela *)(m->base + d->d_un.d_ptr); break;
        case DM_DT_PLTRELSZ: m->pltsz = d->d_un.d_val; break;
        }
    if (!m->strtab || !m->symtab) return -8;
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / DM_SYM_SIZE);
    return 0;
}

/* ---------------- 符号解析 ---------------- */

/* 兜底解析器（可注册）。
 *
 * 用途：客体引用但**不在任何模块里**的符号 —— 本游戏的唯一一类是 `glslc*`
 * （NVIDIA 的离线 GLSL 着色器编译器，真机上由开发包/驱动在运行期提供）。
 * 宿主自己实现它们（见 hle_nvn_glslc.c），通过这里挂进来。
 *
 * 为什么用「可注册」而不是直接调 dm_glslc_lookup：
 *   loader 是底层模块，不该依赖图形层。注册式让依赖方向保持单向。 */
static void *(*g_fallback)(const char *name) = NULL;
void dm_loader_set_fallback(void *(*fn)(const char *name)) { g_fallback = fn; }

/* ---------------- 页保护（第 48 轮加 —— 真机「第一条客体指令就没了」的真因） --------
 *
 * dm_loader_map 给的是 **RW 匿名映射**（要能在上面写映像与重定位），
 * 但 **内核不会执行不可执行的页**。所以映像写完、重定位做完之后，
 * 必须把「代码」那几段改成 R+X，否则客体第一条指令就炸。
 *
 * 症状为什么难查（记下来别再踩）：
 *   · 宿主侧的诊断标记全部正常（"调用 _init_libc1…" 都打出来了 —— 因为那是**宿主代码**在执行）
 *   · 之后**永远没有下文**：既没有崩溃行，也没有看门狗行
 *   · 把第一个调用从 _init 换成 _init_libc1，症状**一模一样**
 *   ⇒ 因为出事的是**第一条客体指令**本身，跟调哪个函数无关。
 */
int dm_loader_finalize(dm_module *m) {
    size_t ps = os_page();
    uintptr_t base = (uintptr_t)m->base;
    int fails = 0;

#define DM_PROT_RANGE(off, end, prot)                                            \
    do {                                                                         \
        uintptr_t a = (base + (uintptr_t)(off)) & ~(uintptr_t)(ps - 1);           \
        uintptr_t e = (base + (uintptr_t)(end) + ps - 1) & ~(uintptr_t)(ps - 1);  \
        if (e > a && os_protect((void *)a, e - a, (prot)) != 0) {                 \
            fails++;                                                             \
            dm_diag_mark("页保护失败：%s [%#llx,%#llx) prot=%d", m->name,          \
                         (unsigned long long)(off), (unsigned long long)(end),   \
                         (prot));                                                \
        }                                                                        \
    } while (0)

    DM_PROT_RANGE(0, m->L.ro_off, DM_PROT_R | DM_PROT_X);            /* 代码段 */
    if (m->L.data_off > m->L.ro_off)
        DM_PROT_RANGE(m->L.ro_off, m->L.data_off, DM_PROT_R);        /* rodata */
    if (m->L.key_page_off > m->L.data_off)
        DM_PROT_RANGE(m->L.data_off, m->L.key_page_off, DM_PROT_R | DM_PROT_W);
    if (m->L.key_page_size)
        DM_PROT_RANGE(m->L.key_page_off, m->L.key_page_off + m->L.key_page_size,
                      DM_PROT_R | DM_PROT_W);                        /* 键页：读写 */
    if (m->L.patch_len)
        DM_PROT_RANGE(m->L.patch_off, m->L.patch_off + m->L.patch_len,
                      DM_PROT_R | DM_PROT_X);                        /* 补丁区：可执行 */
    if (m->size > m->L.patch_off + m->L.patch_len)
        DM_PROT_RANGE(m->L.patch_off + m->L.patch_len, m->size, DM_PROT_R | DM_PROT_W);

#undef DM_PROT_RANGE
    return fails;
}

const void *dm_loader_sym(const dm_module *mods, unsigned nmods, const char *name) {
    /* 与 loader.c 的 module_sym_linear 语义一致：首个「已定义且有名字」的同名符号 */
    for (unsigned i = 0; i < nmods; i++) {
        const dm_module *m = &mods[i];
        for (unsigned k = 1; k < m->nsym; k++) {
            const dm_sym *s = &m->symtab[k];
            if (s->st_shndx && !strcmp(m->strtab + s->st_name, name))
                return m->base + s->st_value;
        }
    }
    /* 模块里没有 → 问兜底解析器（宿主自实现的平台符号） */
    if (g_fallback) return g_fallback(name);
    return NULL;
}

/* ---------------- 重定位 ---------------- */

int dm_loader_relocate(dm_module *m, const dm_module *mods, unsigned nmods,
                       dm_load_stats *st) {
    const dm_rela *tabs[2] = { m->rela, m->jmprel };
    uint64_t sizes[2] = { m->relasz, m->pltsz };
    for (int t = 0; t < 2; t++) {
        if (!tabs[t]) continue;
        for (uint64_t i = 0; i < sizes[t] / DM_RELA_SIZE; i++) {
            const dm_rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = (unsigned)(r->r_info & 0xFFFFFFFFu);
            unsigned si   = (unsigned)(r->r_info >> 32);
            if (type == DM_R_AARCH64_RELATIVE) {
                *where = (uint64_t)m->base + r->r_addend;
                st->n_relative++;
                continue;
            }
            if (si >= m->nsym) { st->n_bad_sym++; continue; }
            const char *nm = m->strtab + m->symtab[si].st_name;

            /* ---- TLSDESC（type 1031）：线程局部存储描述符 ----
             *
             * AArch64 的 TLSDESC 重定位要给出一对 16 字节：
             *     [0] = resolver 函数地址
             *     [1] = 变量在 TLS 块内的偏移
             * 客体取变量时的实际指令序列（第 46 轮从**解压后的模块**里反汇编确认，
             * 见 work/porting/ 的扫描脚本）：
             *     adrp x0, <描述符页>
             *     ldr  x1, [x0, #lo12]     ; x1 = desc[0] = resolver
             *     add  x0, x0, #lo12       ; x0 = &desc[0]  ← 入参是描述符地址
             *     blr  x1                  ; 回来时 x0 = TLS 偏移
             *     mrs  x8, tpidr_el0       ; ← 已被改写器换成读宿主线程槽
             *     add/str … [x8, x0]       ; 用「线程指针 + 偏移」访问变量
             *
             * !! 第 46 轮修的一个致命错 !!
             *   上一版把 [0] 填成 **0**，注释还写「resolver 已不需要」——
             *   那是错的：客体**真的会 blr 它**，于是 `blr 0` → 跳到地址 0 → 立刻崩。
             *   现在填真正的 resolver（dm_tlsdesc_resolver，本文件下面实现）。
             *
             * 偏移从哪来：
             *   · 符号是 TLS 变量（st_value != 0）→ 用 st_value
             *   · 符号是匿名的（st_name 为空，如局部 TLS）→ 用 addend
             * 实测（tlsdesc_selftest）：NmplDLL 的 62 个 TLSDESC 全是这种形状，
             * 匿名那批 addend 都是小整数（8/16/192/2800/5552…）正是 TLS 偏移；
             * 有名字那批是 `...tlsManage_E` 之类，st_value 非 0（0x1640）。 */
            if (type == 1031) {
                uint64_t tls_off;
                if (m->symtab[si].st_value) tls_off = m->symtab[si].st_value;
                else                         tls_off = (uint64_t)r->r_addend;
                if (where + 16 <= m->base + m->size) {
                    ((uint64_t *)where)[0] = (uint64_t)(uintptr_t)dm_tlsdesc_resolver;
                    ((uint64_t *)where)[1] = tls_off;
                }
                st->n_tlsdesc++;
                continue;
            }

            const uint8_t *target = (const uint8_t *)dm_loader_sym(mods, nmods, nm);
            if (!target) {
                /* 未解析：登记并保持原值（诊断用；真机上这里必须补上宿主实现） */
                if (st->n_unres < DM_MAX_UNRES) {
                    st->unres[st->n_unres].name = nm;
                    st->unres[st->n_unres].type = type;
                    st->n_unres++;
                }
                st->n_unresolved++;
                continue;
            }
            uint64_t v = (uint64_t)target;
            if (type == DM_R_AARCH64_JUMP_SLOT) { *where = v;            st->n_jump++; }
            else                                { *where = v + r->r_addend; st->n_abs++; }
        }
    }
    return 0;
}

/* ---------------- 门登记 ---------------- */

/* 把 gates.tsv 读进 hle.c 的门表。
   注意：本函数用**文件形式**登记；真机上还会把返回点加上模块基址。 */
int dm_loader_register_gates(dm_host *h, dm_gate_ent *tbl, size_t cap,
                             const char *gates_path, uint8_t *base) {
    FILE *f = fopen(gates_path, "r");
    if (!f) return -1;
    char line[512];
    size_t n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned long long ret = 0, orig = 0;
        char kind[16] = {0};
        long arg = 0;
        if (sscanf(line, "%llu\t%15[^\t]\t%ld\t%llu", &ret, kind, &arg, &orig) < 4) continue;
        if (n >= cap) break;
        dm_gate_ent *e = &tbl[n++];
        e->ret  = ret + (uint64_t)base;
        e->orig = orig;
        e->arg  = (uint32_t)arg;
        e->name = NULL;
        if      (!strcmp(kind, "svc"))    e->kind = DM_GATE_SVC;
        else if (!strcmp(kind, "cntpct")) e->kind = DM_GATE_CNTPCT;
        else if (!strcmp(kind, "cntvct")) e->kind = DM_GATE_CNTVCT;
        else { n--; continue; }
    }
    fclose(f);
    /* 按返回点排序（dm_host_find 是二分查找，必须有序） */
    for (size_t i = 1; i < n; i++) {
        dm_gate_ent k = tbl[i];
        size_t j = i;
        while (j > 0 && tbl[j-1].ret > k.ret) { tbl[j] = tbl[j-1]; j--; }
        tbl[j] = k;
    }
    h->ents = tbl;
    h->n = n;
    h->svc = NULL;
    return (int)n;
}

/* ---------------- 总入口 ---------------- */

void dm_loader_unmap(dm_module *m) {
    if (m && m->base) { os_unmap(m->base, m->size); m->base = NULL; m->size = 0; }
}

const char *dm_loader_err(int rc) {
    switch (rc) {
    case -1: return "fixups.tsv 读取失败";
    case -2: return "映像读取失败";
    case -3: return "映像大小与 fixups 的 IMAGE_SIZE 不符";
    case -4: return "内存映射失败";
    case -5: return "键页越界";
    case -6: return "键索引越界";
    case -7: return "没有 MOD0";
    case -8: return "动态段缺 STRTAB/SYMTAB";
    default: return "未知";
    }
}
