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
#include "hle_nn.h"      /* dm_nn_force_lookup：nn::ro 走宿主实现（见 dm_loader_sym） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>       /* 执行自检要报 errno */
#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    #include <mach-o/dyld.h>
    #include <mach-o/getsect.h>
    #include <dlfcn.h>
    /* !! iOS 上 mach 头文件的分工（被 CI 教训过三次，记在这里）!!
     *   <mach/mach.h>       mach_task_self、kern_return_t、vm_address_t
     *   <mach/vm_map.h>     vm_allocate / vm_remap / vm_deallocate、VM_FLAGS_*、VM_INHERIT_*
     *   <mach/vm_region.h>  vm_region_basic_info_data_64_t、VM_REGION_BASIC_INFO_64
     *   **千万不要**包含 <mach/mach_vm.h> —— iOS SDK 里那个文件第一行就是
     *   `#error mach_vm.h unsupported.`（macOS 才有 mach_vm_region）。 */
    #include <mach/mach.h>
    #include <mach/vm_map.h>
    #include <mach/vm_region.h>
  #endif
#endif

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

/* loader_finish 定义在下面，但 dm_loader_map 先用到它 —— 变量/函数都没有
   "后补声明"这回事，必须先声明后用（本项目的 check_use_before_decl.py 会查）。 */
static int loader_finish(dm_module *m, unsigned key_values[DM_KEY_COUNT]);

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
    return loader_finish(m, key_values);
}

/* ---------------- 装载收尾（两种映射路径共用） ----------------
 * 键页 / MOD0 / 动态段解析 —— 这一段与「映像从哪来」无关：
 *   · 文件路径（Windows 自测、macOS 开发）：mmap 一块 RW 再 memcpy 进去
 *   · iOS 真机：从已签名的 Guest.framework 里 vm_remap 过来（见 dm_loader_map_embedded）
 * 抽出来是为了让两条路径**跑的是同一段代码**，避免又出现"两边行为不一致"。 */
static int loader_finish(dm_module *m, unsigned key_values[DM_KEY_COUNT]) {
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
        /* 第 61 轮：模块的 init 函数与 C++ 全局构造数组。
         * 真机上 rtld 在装载时就跑它们（早于一切 SDK 初始化）—— 本游戏的
         * nn::sf::hipc 服务描述表就是靠构造函数填的名字指针。 */
        case DM_DT_INIT:          m->dt_init = (void *)(m->base + d->d_un.d_ptr); break;
        case DM_DT_INIT_ARRAY:    m->init_array = (void **)(m->base + d->d_un.d_ptr); break;
        case DM_DT_INIT_ARRAYSZ:  m->init_array_n = d->d_un.d_val / sizeof(void *); break;
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


#if defined(__APPLE__) && TARGET_OS_IPHONE
/* ==================================================================
 * iOS：从**会被代码签名的** Guest.framework/Guest 里取段，vm_remap 回原布局
 * ==================================================================
 *
 * 为什么必须这样（第 50 轮真机实测）：
 *   iOS **不允许**普通 App 执行「运行时写进内存」的代码。实测：匿名页 mmap(RW)
 *   → mprotect(R+X) **返回成功**，但一执行进程就被内核干掉（无信号、无看门狗，
 *   日志停在那一行）。这正是「客体第一条指令就没了」的真因。
 * 正确做法（照 D2R 的做法，见本仓库 dm/host/loader.c 的注释）：
 *   把映像放进一个**会被签名**的 Mach-O（用户侧自签工具给整包签名），运行时只做
 *   vm_remap：代码页 copy=FALSE（与 dylib 的 __TEXT 共享同一批**已签名页**，本来就 r-x），
 *   可写部分 copy=TRUE（写时复制）。代码页从没被写过、本来就是签名的可执行页 ⇒ 可执行。
 *
 * 节布局由 work/tools/guest_dylib.py 写出（两边约定必须一致）：
 *   __TEXT,__dm<i>x0    [0, r16(TEXT_END))
 *   __DM<i>W,__img      [r16(TEXT_END), PATCH_OFF)
 *   __TEXT,__dm<i>x1    [PATCH_OFF, r16(映像尾))
 */
static void dm_remap_at(const char *name, vm_address_t to, const void *from,
                        unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0,
                                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                mach_task_self(), (vm_address_t)from, copy,
                                &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) {
        dm_diag_mark("**vm_remap 失败** %s from=%p to=%p size=%#lx kr=%d",
                     name, from, (void *)to, size, (int)kr);
        return;
    }
    if ((cur & need) != need)
        dm_diag_mark("vm_remap 后权限不足 %s: 实际 %#x 需要 %#x", name, cur, need);
}

int dm_loader_map_embedded(dm_module *m, unsigned idx, const char *name,
                           unsigned key_values[DM_KEY_COUNT]) {
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);

    /* fixups 与其它元数据在包内 dm/ 下（与文件路径同一套） */
    extern const char *dm_host_res_dir(void);
    char fix[1400];
    snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dm_host_res_dir(), name);
    if (dm_loader_read_fixups(&m->L, fix)) return -1;

    /* 打开那个只装段的 dylib（iOS 外壳给出绝对路径） */
    extern const char *dm_ios_guest_dylib_path(void);
    const char *path = dm_ios_guest_dylib_path();
    void *h = path ? dlopen(path, RTLD_NOW) : NULL;
    if (!h) {
        dm_diag_mark("**dlopen 失败** path=%s err=%s",
                     path ? path : "(null)", dlerror() ? dlerror() : "?");
        return -2;
    }

    /* !! dlopen 只用来「确保它被加载」，**绝不能拿它的返回值当 mach_header** !!
     * 第 51 轮真机崩溃报告（DmGame-2026-10-15-173419.ips）：
     *     #0 libmacho.dylib  getsectiondata
     *     #1 DmGame          dm_loader_map_embedded
     *     EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x679d1080
     * 原因就是我直接把 dlopen 的句柄转成 mach_header 传给了 getsectiondata。
     * 正确做法（照 D2R 的 dm/host/loader.c：它也是 dlopen 之后再去 dyld 的映像表里找）：
     * 用 _dyld_get_image_header 按映像名匹配，拿到真正的 mach_header。 */
    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "/Guest.framework/Guest"))
            mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    }
    if (!mh) {
        dm_diag_mark("**找不到 Guest 映像头**：dlopen 成功，但 dyld 映像表里没有 "
                     "*/Guest.framework/Guest（共 %u 个映像）", (unsigned)_dyld_image_count());
        return -3;
    }

    char sn0[20], sn1[20], wn[20];
    snprintf(sn0, sizeof sn0, "__dm%ux0", idx);
    snprintf(sn1, sizeof sn1, "__dm%ux1", idx);
    snprintf(wn,  sizeof wn,  "__DM%uW",  idx);
    dm_diag_mark("Guest 映像头 = %p（按映像名找到）", (const void *)mh);
    unsigned long s0 = 0, sw = 0, s1 = 0;
    const uint8_t *p0 = getsectiondata(mh, "__TEXT", sn0, &s0);
    const uint8_t *pw = getsectiondata(mh, wn,     "__img", &sw);
    const uint8_t *p1 = getsectiondata(mh, "__TEXT", sn1, &s1);
    if (!p0 || !pw || !p1) {
        dm_diag_mark("**找不到节** %s（%s / %s.__img / %s）", name, sn0, wn, sn1);
        return -3;
    }

    unsigned long a   = (unsigned long)((m->L.text_end + 0x3FFF) & ~(uint64_t)0x3FFF);
    unsigned long po  = (unsigned long)m->L.patch_off;
    m->size           = (size_t)((m->L.image_size + 0x3FFF) & ~(uint64_t)0x3FFF);
    unsigned long end = (unsigned long)m->size;

    /* 节长度必须与 fixups 推出的布局吻合 —— 不吻合说明「生成工具」与「装载器」
       的约定被改歪了（例如模块顺序不同），这时必须报错，不能凑合跑 */
    int bad = 0;
    if (s0 != a)        { dm_diag_mark("节长不符 %s: %s=%lu 期望 %lu", name, sn0, s0, a);          bad = 1; }
    if (sw != po - a)   { dm_diag_mark("节长不符 %s: %s=%lu 期望 %lu", name, wn, sw, po - a);        bad = 1; }
    if (s1 != end - po) { dm_diag_mark("节长不符 %s: %s=%lu 期望 %lu", name, sn1, s1, end - po);     bad = 1; }
    if (bad) return -4;

    /* 占一块连续 VA，再把三段 remap 回原始相对位置 */
    vm_address_t base = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &base, end, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) { dm_diag_mark("vm_allocate 失败 kr=%d", (int)kr); return -5; }
    m->base = (uint8_t *)base;
    m->embedded = 1;

    dm_remap_at(name, base,      p0, s0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE); /* 代码：共享已签名页 */
    dm_remap_at(name, base + a,  pw, sw, TRUE,  VM_PROT_READ | VM_PROT_WRITE);  /* 可写：写时复制 */
    dm_remap_at(name, base + po, p1, s1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE); /* 补丁区：共享已签名页 */

    int rc = loader_finish(m, key_values);
    dm_diag_mark("embedded 映射 %s：base=%p 代码=%p 可写=%p 补丁=%p rc=%d",
                 name, (void *)base, (void *)p0, (void *)pw, (void *)p1, rc);
    return rc;
}
#endif /* __APPLE__ && TARGET_OS_IPHONE */

/* ---------------- 执行自检 + 保护位查询（第 49 轮加） ----------------
 *
 * 为什么「亲自执行一条指令」比看 mprotect 返回值可靠：
 *   真机上出现过「宿主标记全部正常、之后进程直接没了」的现象，而 mprotect 看不出问题
 *   （它可能返回成功但页实际仍不可执行，或者 iOS 对可执行匿名页另有要求）。
 *   **唯一可靠的判据是真的跳进去执行一条指令。**
 * 用 `ret` 是最小的可执行体：写 4 字节 → 调它 → 应当立刻回到这里。 */
int dm_loader_exec_selftest(void) {
    size_t ps = os_page();
    unsigned char *p = (unsigned char *)os_map(ps);
    if (!p) { dm_diag_mark("执行自检：os_map 失败"); return -1; }

#if defined(__aarch64__) || defined(_M_ARM64)
    const uint32_t RET = 0xD65F03C0u;      /* ret */
#else
    const uint32_t RET = 0xC3C3C3C3u;      /* 占位；非 aarch64 上不会真的调用 */
#endif
    memcpy(p, &RET, 4);

    int rc = os_protect(p, ps, DM_PROT_R | DM_PROT_X);
    if (rc != 0) {
        dm_diag_mark("执行自检：mprotect(R+X) 失败 rc=%d errno=%d", rc, errno);
        os_unmap(p, ps);
        return -2;
    }

    dm_diag_mark("执行自检：即将调用 RW→RX 匿名页里的 ret —— "
                 "**如果这就是日志最后一行**，说明这条路在真机上不可行");
    typedef void (*fn0)(void);
#if defined(__aarch64__) || defined(_M_ARM64)
    ((fn0)(void *)p)();
#endif
    dm_diag_mark("执行自检：**成功**（匿名页 RW→RX 之后可以执行）");
    /* 故意不 unmap：留着当"证据"，也免得 unmap 触发别的检查 */
    (void)ps;
    return 0;
}

#if defined(__APPLE__)
  /* mach 头文件已在文件开头统一包含（见那里的说明） */
  /* 把 VM_PROT_* 折叠成 1=R / 2=W / 4=X */
  static int darwin_vm_prot(uintptr_t addr, unsigned *prot_out) {
      vm_address_t a = (vm_address_t)addr;
      vm_size_t sz = 0;
      vm_region_basic_info_data_64_t info;
      mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t obj = MACH_PORT_NULL;
      kern_return_t kr = vm_region_64(mach_task_self(), &a, &sz,
                                      VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &obj);
      if (kr != KERN_SUCCESS) return -1;
      unsigned q = 0;
      if (info.protection & VM_PROT_READ)    q |= 1;
      if (info.protection & VM_PROT_WRITE)   q |= 2;
      if (info.protection & VM_PROT_EXECUTE) q |= 4;
      *prot_out = q;
      return 0;
  }
#endif

int dm_loader_query_prot(const dm_module *m, char *out, int n) {
    if (!out || n < 5) return -1;
    out[0] = '?'; out[1] = 0;
#if defined(__APPLE__)
    unsigned prot = 0;
    if (darwin_vm_prot((uintptr_t)m->base, &prot) == 0) {
        int k = 0;
        if (prot & 1) out[k++] = 'R';
        if (prot & 2) out[k++] = 'W';
        if (prot & 4) out[k++] = 'X';
        if (k == 0) out[k++] = '-';
        out[k] = 0;
        return 0;
    }
    return -2;
#else
    (void)m;
    return -1;      /* 非 Apple：不支持（Windows 自测里不查） */
#endif
}


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

/* 第 61 轮：把 .init_array 条目加上模块基址。
 *
 * 为什么必须有它（.ips 实证）：本游戏的 SDK 模块里，.init_array 的 83 个条目、
 * 以及 nn::sf::hipc 的服务描述表（"lm" 等名字指针）在 .rela.dyn 里**都没有**
 * R_AARCH64_RELATIVE 条目 —— 真机上由 rtld 按「init/fini 数组与部分表 = 基址相对」
 * 的约定处理（也由构造函数填表）。我们既不加基址、又不跑构造，SDK 第 4 步
 * 一用就是裸偏移：strlen(0x8ffad7) → SIGSEGV。
 * 必须在页保护（dm_loader_finalize）**之前**调 —— 数组在数据段，要可写。 */
void dm_loader_fix_init_arrays(dm_module *m) {
    if (!m->init_array || !m->init_array_n) return;
    for (size_t i = 0; i < m->init_array_n; i++) {
        uint64_t v = (uint64_t)m->init_array[i];
        /* 只加没有基址的：条目是裸偏移（< 模块大小）。幂等：已加过的不再加。 */
        if (v && v < m->size) m->init_array[i] = (void *)(v + (uint64_t)m->base);
    }
}

const void *dm_loader_sym(const dm_module *mods, unsigned nmods, const char *name) {
    /* !! nn::ro::* 一律走宿主实现，**不要**用客体里那份 !!
     * 为什么（第 53 轮真机证据）：sdk 自己的 `_init`（DT_INIT）会调 `nn::ro::ProtectRelro`，
     * 参数来自一张**没有重定位**的表（值是模块内偏移）——那张表是 **rtld 在装载时**填的
     * （`nn::ro::LoadModule` 的职责）。本工程的宿主不跑 rtld、而是用 HLE 顶掉 nn::ro，
     * 于是那些字段一直是偏移量 → 客体那份 ProtectRelro 拿偏移当指针 → 0xb85028 段错误。
     * 宿主这份 `dm_nn_ro_ProtectRelro` 是空实现（RELRO 在我们这里不需要保护），正好合适。
     * 目前只对 `_ZN2nn2ro` 前缀这么做：范围明确，不动 os/fs 等（那些宿主实现是另一套考虑）。 */
    /* !! 服务层初始化的强制接管（第 62 轮加）!!
     * 依据 D2R Mobile 真机源码（dm/host/hle.h 的 HLE_FORCE / dm/host/hle_sdkinit.c，MIT）：
     * 「即使 SDK 模块里有定义，装载时所有对该符号的引用一律解析到宿主实现。」
     * nninitInitializeSdkModule 会调下面这批「连 Horizon 系统服务（IPC）」的初始化，
     * iOS 上没有那些服务进程；SDK 里那份会拿 nn::sf::hipc 服务描述表去
     * ConnectToHipcService，表没填就是裸偏移 → SIGSEGV（第 61 轮 .ips 实证）。
     * 所以不建会话、直接返回；后续各服务接口在用到时再逐个接管。 */
    if (name) {
        /* !! nndiagStartup 必须**最先**判，且不受前缀表约束 !!
         * 为什么（第 69 轮 .ips 实证）：它不是 _ZN2nn 修饰名，前缀表覆盖不到它。
         * 第 68 轮把 57 条名字清单换成 20 个前缀时把它漏了 —— 于是本分支成了死代码、
         * SDK 自己那份 nndiagStartup 被执行：nninitInitializeSdkModule
         *   → nndiagStartup → nn::lm::CreateLogServiceByHipc
         *   → ConnectToHipcService(&h, "lm")（名字来自服务描述表 0xb7ddd0，
         *     该表在 .rela.dyn 里没有 RELATIVE 条目，值是裸偏移 0x8ffad7）
         *   → GetPortProcessor("lm") 查不到 → strlen(0x8ffad7) → SIGSEGV。
         * D2R 的 HLE_FORCE 清单第 178 行也有 nndiagStartup —— 它就是无条件接管的。 */
        if (!strcmp(name, "nndiagStartup")) {
            extern void dm_nn_nndiagStartup(void);
            return (void *)dm_nn_nndiagStartup;
        }
        static const char *force_prefixes[] = {   /* 20 个服务层 nn:: 命名空间 */
            "_ZN2nn7account",
            "_ZN2nn7friends",
            "_ZN2nn6socket",
            "_ZN2nn5audio",
            "_ZN2nn5codec",
            "_ZN2nn5swkbd",
            "_ZN2nn4diag",
            "_ZN2nn4nifm",
            "_ZN2nn4pctl",
            "_ZN2nn4time",
            "_ZN2nn3aoc",
            "_ZN2nn3err",
            "_ZN2nn3hid",
            "_ZN2nn3mem",
            "_ZN2nn3ngc",
            "_ZN2nn3nsd",
            "_ZN2nn3ssl",
            "_ZN2nn2ec",
            "_ZN2nn2fs",
            "_ZN2nn2oe",
        };
        int force = !strncmp(name, "_ZN2nn2ro", 9);
        if (!force) {
            for (size_t k = 0; k < sizeof force_prefixes / sizeof force_prefixes[0]; k++)
                if (!strncmp(name, force_prefixes[k], strlen(force_prefixes[k]))) { force = 1; break; }
        }
        if (force) {
            void *h = dm_nn_force_lookup(name);
            if (h) return h;
        }
    }
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
#if defined(__APPLE__) && TARGET_OS_IPHONE
    if (m->embedded) {   /* vm_remap 来的：用 vm_deallocate 还回去 */
        if (m->base) vm_deallocate(mach_task_self(), (vm_address_t)m->base, m->size);
        m->base = NULL; m->embedded = 0; return;
    }
#endif
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
