/* 宿主图形层 —— glslc 接管（NVIDIA 离线 GLSL 着色器编译器）。
 *
 * 【为什么必须接管】
 * NmplDLL.nro 的 8 个 glslc* 符号是**全项目唯一的未解析符号**（27 条重定位）。
 * glslc 不在任何模块里、也不在 RomFS 里 —— 真机上它是开发包/驱动的一部分，运行期才解析。
 * 不接管 → 这 27 条重定位填不上 → 引擎一调就崩。
 *
 * 【接管策略：不真去编 GLSL，返回离线已经翻好的结果】
 *   游戏要 "编" 某个着色器
 *        ↓ 宿主从它的控制段算 key = sha256(ctrl[0:0x880]).hexdigest()[:16]
 *   查 shader_map.tsv（126 条）
 *        ↓
 *   返回第 6 轮离线翻好的 <key>.metal
 *
 * 【为什么这条路成立 —— 三条已有证据】
 *   1. 第 25 轮验证：运行期在 nvnProgramInitialize 里算的 key 与离线翻着色器用的 key
 *      **12/12 命中、逐字节一致**
 *   2. 第 6 轮产出：116 个 <key>.metal + 126 条映射表
 *   3. 第 31 轮验证：**表里 126 条记录的 key 全部可由 ctrl_at 处的控制段重算得到**
 *      （126/126 符合），所以逆查（从控制段反推是哪个着色器）是可靠的
 *
 * 【glslcCompile 的输入是什么】
 *   引擎把**已编好的 NVN 着色器**（SPH + 指令 + 控制段）交给它做「特化」。
 *   所以我们不需要 GLSL 源码 —— 只要在参数里找到**控制段**（magic 0x98761234，
 *   且 +0x14 处 == 0x878 作为自校验），就能确定是哪个着色器。
 *
 * 【本文件做到什么程度（诚实）】
 *   · initialize / finalize / setallocator / getdefaultoptions / getversion  → 完整可用
 *   · compile / compileprespecialized / compilespecialized                   → 识别输入着色器、
 *     查表、把 Metal 着色器路径交给调用方；**不产生真的 NVN 二进制**
 *     （真机侧 Metal 路径本来就不需要 NVN 二进制 —— 见下）
 *
 *   TODO（真机侧）：若引擎确实要读回 NVN 二进制，则本模块要改为**把输入的
 *   SPH/指令/控制段原样回填**（passthrough）—— 因为引擎要的那份数据它自己已经给了。
 */
#include "hle_nvn.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_glslc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NVN 着色器二进制的两个 magic（第 31 轮实测） */
#define DM_SPH_MAGIC   0x12345678u   /* SPH +0x00 */
#define DM_CTRL_MAGIC  0x98761234u   /* 控制段 +0x00 */
#define DM_CTRL_LEN    0x880u        /* 控制段长 */
#define DM_CTRL_SELF   0x878u        /* 控制段 +0x14 处的自校验值 */

static struct {
    uint64_t n_init, n_finalize, n_compile, n_pre, n_spec;
    uint64_t n_identified, n_unknown;
    uint64_t n_reject_sph, n_reject_ctrl, n_reject_unreadable;
} g_glslc_stats;
const void *dm_glslc_stats_ptr(void) { return &g_glslc_stats; }

/* 最近一次成功识别的着色器（供诊断与自测） */
static char g_last_key[17];
static char g_last_name[64];
const char *dm_glslc_last_key(void)  { return g_last_key; }
const char *dm_glslc_last_name(void) { return g_last_name; }

/* ---------------------------------------------------------------- */
/* 内存可读性检查 —— 扫描前必须先确认，否则读到未映射页会访问违例      */
/* ---------------------------------------------------------------- */
/* 为什么需要它：glslc 的参数布局我们**没有验证过**，所以是「试各种
 * (指针, 长度) 组合」。如果某个参数其实不是指针（或长度远超实际缓冲），
 * 裸扫就会崩 —— 实测在自测里传假指针 (void*)0x100000 立刻 0xC0000005。
 * 引擎的真参数里混进非法值时同样会崩，所以这不是自测的毛病，是**必须有的防护**。 */
#if defined(_WIN32)
  #include <windows.h>
  static int mem_readable(const void *p, size_t n) {
      if (!p || !n) return 0;
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
      if (mbi.State != MEM_COMMIT) return 0;
      if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
      /* 跨区域时保守拒绝（简化：只要求这一段都在同一可读区内） */
      uintptr_t start = (uintptr_t)p;
      uintptr_t end   = start + n;
      uintptr_t rbeg  = (uintptr_t)mbi.BaseAddress;
      uintptr_t rend  = rbeg + mbi.RegionSize;
      return (start >= rbeg && end <= rend);
  }
#else
  #include <unistd.h>
  /* mincore() 声明在 <sys/mman.h> 里（Darwin 与 Linux 都是），
   * 只包含 <unistd.h> 时 clang 会报：
   *   error: call to undeclared function 'mincore';
   *          ISO C99 and later do not support implicit function declarations */
  #include <sys/mman.h>
  static int mem_readable(const void *p, size_t n) {
      if (!p || !n) return 0;
      long ps = sysconf(_SC_PAGESIZE);
      if (ps <= 0) ps = 4096;
      uintptr_t start = (uintptr_t)p;
      uintptr_t end   = start + n;
      /* 逐页检查（页对齐后 mincore 可判断是否已映射） */
      for (uintptr_t a = start & ~(uintptr_t)(ps - 1); a < end; a += (uintptr_t)ps) {
          unsigned char vec = 0;
          /* 第三个参数按 (void *) 传：Darwin 的原型是 char *，
           * glibc 的是 unsigned char * —— 传 void* 两边都不报警告。 */
          if (mincore((void *)a, (size_t)ps, (void *)&vec) != 0) return 0;
      }
      return 1;
  }
#endif

/* ---------------------------------------------------------------- */
/* 从一段内存里找控制段并识别着色器                                  */
/* ---------------------------------------------------------------- */

/* 判据（两条都要满足，实测过）：
 *   1. +0x00 == 0x98761234
 *   2. +0x14 == 0x878        （控制段自校验字段，126/126 记录都满足）
 * 第 2 条把误判概率压到极低 —— 单靠 magic 有 2^-32，加自校验后实际不可能撞。 */
static int looks_like_control(const uint8_t *p) {
    if (*(const uint32_t *)p != DM_CTRL_MAGIC) return 0;
    if (*(const uint32_t *)(p + 0x14) != DM_CTRL_SELF) return 0;
    return 1;
}

/* 在 [base, base+size) 里以 4 字节对齐找控制段。
 * 返回找到的偏移，-1 = 没找到。
 * 注意：调用前必须已确认 [base, base+size) 可读（见 mem_readable）。 */
static long scan_for_control(const uint8_t *base, size_t size) {
    if (!base || size < DM_CTRL_LEN) return -1;
    for (size_t off = 0; off + DM_CTRL_LEN <= size; off += 4) {
        if (looks_like_control(base + off)) return (long)off;
    }
    return -1;
}

/* 识别：从「引擎给的数据」里定位控制段 → 算 key → 查表。
 * 成功返回 Metal 着色器路径（宿主内静态存储），失败返回 NULL。
 * 同时把 key 与 ShaderMap 里的名字写进 g_last_*。 */
const char *dm_glslc_identify(const void *data, size_t size, int *out_stage_is_vs) {
    if (!data || size < DM_CTRL_LEN) { g_glslc_stats.n_unknown++; return NULL; }
    /* **先确认可读** —— 不能对未知指针裸扫（会 0xC0000005） */
    if (!mem_readable(data, size)) {
        g_glslc_stats.n_reject_unreadable++;
        g_glslc_stats.n_unknown++;
        return NULL;
    }

    /* 先看开头是不是 SPH：若是，控制段就在其后（SPH 0x50 + 指令段） */
    const uint8_t *p = (const uint8_t *)data;
    long ctrl_off = -1;
    if (size >= 4 && *(const uint32_t *)p == DM_SPH_MAGIC) {
        /* SPH 之后找 —— 但更稳的是直接全扫，因为指令段长度未知 */
        ctrl_off = scan_for_control(p, size);
    } else if (looks_like_control(p)) {
        ctrl_off = 0;
    } else {
        ctrl_off = scan_for_control(p, size);
    }
    if (ctrl_off < 0) {
        g_glslc_stats.n_reject_ctrl++;
        g_glslc_stats.n_unknown++;
        return NULL;
    }

    char key[17];
    dm_nvn_shader_key(p + ctrl_off, key);
    const void *e = dm_nvn_shader_find(key);
    if (!e) {
        g_glslc_stats.n_unknown++;
        return NULL;
    }
    g_glslc_stats.n_identified++;
    snprintf(g_last_key, sizeof g_last_key, "%s", key);
    snprintf(g_last_name, sizeof g_last_name, "%s", dm_nvn_shader_name(e));
    if (out_stage_is_vs) {
        const char *st = dm_nvn_shader_stage(e);
        *out_stage_is_vs = (st && st[0] == 'v') ? 1 : 0;
    }
    return dm_nvn_shader_metal_path(e);
}

/* ---------------------------------------------------------------- */
/* glslc 的 8 个入口                                                 */
/* ---------------------------------------------------------------- */

/* glslcInitialize() / glslcFinalize() —— 无参、返回 void */
void dm_glslc_Initialize(void) { g_glslc_stats.n_init++; }
void dm_glslc_Finalize(void)   { g_glslc_stats.n_finalize++; }

/* glslcGetVersion() -> const char*（NVIDIA 的版本串）。
   返回一个固定串即可 —— 引擎一般只打日志或做比较。 */
const char *dm_glslc_GetVersion(void) { return "glslc 1.0 (dm host stub)"; }

/* glslcSetAllocator(alloc, free) —— 记录但不用（宿主自己用 malloc）。
   引擎给的分配器我们无法安全调用（它的签名与语义都未验证），
   所以**不用它**，返回 void 让引擎继续。 */
void dm_glslc_SetAllocator(void *alloc, void *freefn) { (void)alloc; (void)freefn; }

/* glslcGetDefaultOptions(GlslcOptions *out) —— 填一份「默认选项」。
   引擎若读这些字段决定编译行为，全 0 是安全下限（不启用任何可选特性）。 */
void dm_glslc_GetDefaultOptions(void *out) {
    if (out) memset(out, 0, 0x100);   /* 大小未验证；清零 0x100 字节是保守做法 */
}

/* 三个编译入口。它们的参数布局未在二进制里验证过，所以**用扫描**：
   在参数给出的每个指针 + 长度对里找控制段。
   参数形如 (in_data, in_size, out_blob, out_size) 的各种排列 —— 逐个试。 */
static const char *compile_common(void *a0, void *a1, void *a2, void *a3,
                                  void *a4, void *a5) {
    const void *cand[6]  = { a0, a1, a2, a3, a4, a5 };
    /* 把「看起来像长度」的参数也收集起来，配对试 */
    size_t lens[6] = { 0 };
    for (int i = 0; i < 6; i++) {
        uintptr_t v = (uintptr_t)cand[i];
        if (v < (1u << 20) && v > 0x100) lens[i] = (size_t)v;   /* 当长度 */
    }
    /* 试所有 (指针, 长度) 组合 */
    for (int i = 0; i < 6; i++) {
        if (!cand[i]) continue;
        uintptr_t v = (uintptr_t)cand[i];
        if (v < 0x10000) continue;
        /* 传假指针必须被挡住而不是崩 —— 这一步是上一版崩掉后加的 */                  /* 不像指针 */
        for (int k = 0; k < 6; k++) {
            if (!lens[k]) continue;
            if (lens[k] < DM_CTRL_LEN) continue;
            const char *m = dm_glslc_identify(cand[i], lens[k], NULL);
            if (m) return m;
        }
        /* 长度没配对时，给一个上界试（控制段一定在数据里，但长度未知） */
        const char *m = dm_glslc_identify(cand[i], 1u << 20, NULL);
        if (m) return m;
    }
    g_glslc_stats.n_unknown++;
    return NULL;
}

void *dm_glslc_Compile(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    g_glslc_stats.n_compile++;
    return (void *)compile_common(a0, a1, a2, a3, a4, a5);
}
void *dm_glslc_CompilePreSpecialized(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    g_glslc_stats.n_pre++;
    return (void *)compile_common(a0, a1, a2, a3, a4, a5);
}
void *dm_glslc_CompileSpecialized(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    g_glslc_stats.n_spec++;
    return (void *)compile_common(a0, a1, a2, a3, a4, a5);
}

/* ---------------------------------------------------------------- */
/* 注册进装载器的符号解析                                            */
/* ---------------------------------------------------------------- */

/* 装载器未解析 glslc* 时调用这里拿实现。
 * 返回 NULL = 宿主也没有 → 记进未解析清单（保持可见，不静默）。 */
void *dm_glslc_lookup(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "glslcInitialize"))          return (void *)dm_glslc_Initialize;
    if (!strcmp(name, "glslcFinalize"))            return (void *)dm_glslc_Finalize;
    if (!strcmp(name, "glslcGetVersion"))          return (void *)dm_glslc_GetVersion;
    if (!strcmp(name, "glslcSetAllocator"))        return (void *)dm_glslc_SetAllocator;
    if (!strcmp(name, "glslcGetDefaultOptions"))   return (void *)dm_glslc_GetDefaultOptions;
    if (!strcmp(name, "glslcCompile"))             return (void *)dm_glslc_Compile;
    if (!strcmp(name, "glslcCompilePreSpecialized")) return (void *)dm_glslc_CompilePreSpecialized;
    if (!strcmp(name, "glslcCompileSpecialized"))  return (void *)dm_glslc_CompileSpecialized;
    return NULL;
}
