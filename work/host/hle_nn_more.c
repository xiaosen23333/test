/* 宿主 HLE —— nn::hid / nn::oe / nn::time / nn::fontll / 其余。
 *
 * nn::hid 是本游戏「能玩」的前提：iPad 的触摸要映射到客体期望的输入状态。
 * 本游戏导入的 hid 面有两块：
 *   ① NpadState（手柄风格）—— 我们用「虚拟手柄」实现：触摸位置 → 摇杆/按键
 *   ② TouchScreenState —— **iPad 原生就有触摸**，直接映射
 *
 * 客体结构布局说明：
 *   NpadState / TouchScreenState 的**布局**在 Horizon SDK 里是公开且固定的。
 *   宿主按公开布局填字段。这里按最常用的字段顺序填（见各函数的注释）。
 */
#include "hle.h"
#include "hle_nn.h"
/* 必须包含自己的头：dm_nn_InitializeSystemEvent() 等在 hle_nn_more.h 里声明
 * （实现在 hle_nn_thread.c）。原来漏了这行，clang 报
 *   error: call to undeclared function 'dm_nn_InitializeSystemEvent'
 * 顺带好处：这样本文件的定义会与头文件声明逐一对照，签名写错当场就能发现。 */
#include "hle_nn_more.h"
#include "hle_diag.h"      /* dm_diag_mark：服务层接管的诊断打点 */

#include <stdio.h>
#include <string.h>

/* ================= 输入状态（宿主前端写入） ================= */

typedef struct {
    /* 虚拟手柄 */
    uint32_t buttons;        /* NpadButton 位掩码 */
    int32_t  lx, ly, rx, ry; /* 摇杆，范围 -32768..32767 */
    /* 触摸屏 */
    int      touch_active;
    uint32_t touch_x, touch_y, touch_diameter, touch_angle;
    int      touch_count;
    /* 体感（不做真体感，给单位姿态） */
    float    quat[4];
    float    accel[3], gyro[3];
} dm_input_state;

/* 初始化：quat = {0,0,0,1}（单位四元数），其余全 0。
   数一下字段：buttons, lx, ly, rx, ry, touch_active, touch_x, touch_y,
   touch_diameter, touch_angle, touch_count, quat[4], accel[3], gyro[3] —— 共 22 个标量。
   上一版写成 10 个标量 + 3 个数组初始化，与字段数不匹配（MSVC 报「初始值设定项太多」）。 */
static dm_input_state g_in = {
    /* buttons */ 0u,
    /* lx, ly, rx, ry */ 0, 0, 0, 0,
    /* touch_active */ 0,
    /* touch_x, touch_y, touch_diameter, touch_angle */ 0u, 0u, 0u, 0u,
    /* touch_count */ 0,
    /* quat */ { 0.0f, 0.0f, 0.0f, 1.0f },
    /* accel */ { 0.0f, 0.0f, 0.0f },
    /* gyro */ { 0.0f, 0.0f, 0.0f },
};
static struct { uint64_t n_init_npad, n_get_npad, n_init_touch, n_get_touch, n_vib; } g_hid_stats;

/* 宿主前端调用这些来喂输入 */
void dm_input_set_buttons(uint32_t b) { g_in.buttons = b; }
void dm_input_set_stick(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    g_in.lx = lx; g_in.ly = ly; g_in.rx = rx; g_in.ry = ry;
}
void dm_input_set_touch(int active, uint32_t x, uint32_t y) {
    g_in.touch_active = active ? 1 : 0;
    g_in.touch_x = x; g_in.touch_y = y;
    g_in.touch_count = active ? 1 : 0;
    g_in.touch_diameter = active ? 10 : 0;
}
const void *dm_hid_stats_ptr(void) { return &g_hid_stats; }

/* ================= nn::hid ================= */

void dm_nn_hid_InitializeNpad(void) { g_hid_stats.n_init_npad++; }

/* nn::hid::NpadId::Handheld —— 这是一个**数据符号**（NpadIdType 常量）。
   值 = 0x20（Horizon 里 Handheld 是 0x20）。 */
const uint32_t dm_nn_hid_NpadId_Handheld = 0x20u;

void dm_nn_hid_InitializeTouchScreen(void) { g_hid_stats.n_init_touch++; }

void dm_nn_hid_SetSupportedNpadStyleSet(uint32_t style) { (void)style; }
void dm_nn_hid_SetSupportedNpadIdType(const uint32_t *ids, uint64_t n) { (void)ids; (void)n; }
uint32_t dm_nn_hid_GetNpadStyleSet(const uint32_t *id) { (void)id; return 1u; }  /* 1 = FullKey */

/* 摇杆的 AxisEncoder：Horizon 用 s16（-32768..32767）表示模拟轴。 */
static void fill_common(void *st) {
    /* NpadCommonState（含 NpadFullKeyState / HandheldState / JoyDualState…）的布局：
       +0x00 u64 Buttons
       +0x08 u64 ButtonsRaw
       +0x10 s32 LStickX, +0x14 s32 LStickY, +0x18 s32 RStickX, +0x1C s32 RStickY
       +0x20 u32 Attributes / 之后是六轴等
       不同变体的偏移略有差异，但**按钮与摇杆都在最前面**，这是所有变体一致的。 */
    uint8_t *p = (uint8_t *)st;
    if (!p) return;
    *(uint64_t *)(p + 0x00) = (uint64_t)g_in.buttons;
    *(uint64_t *)(p + 0x08) = (uint64_t)g_in.buttons;
    *(int32_t  *)(p + 0x10) = g_in.lx;
    *(int32_t  *)(p + 0x14) = g_in.ly;
    *(int32_t  *)(p + 0x18) = g_in.rx;
    *(int32_t  *)(p + 0x1C) = g_in.ry;
}

/* void nn::hid::GetNpadState(NpadXxxState *out, const NpadIdType &id) —— 5 个变体 */
void dm_nn_hid_GetNpadState_FullKey(void *out, const uint32_t *id)   { (void)id; g_hid_stats.n_get_npad++; fill_common(out); }
void dm_nn_hid_GetNpadState_Handheld(void *out, const uint32_t *id)  { (void)id; g_hid_stats.n_get_npad++; fill_common(out); }
void dm_nn_hid_GetNpadState_JoyDual(void *out, const uint32_t *id)   { (void)id; g_hid_stats.n_get_npad++; fill_common(out); }
void dm_nn_hid_GetNpadState_JoyLeft(void *out, const uint32_t *id)   { (void)id; g_hid_stats.n_get_npad++; fill_common(out); }
void dm_nn_hid_GetNpadState_JoyRight(void *out, const uint32_t *id)  { (void)id; g_hid_stats.n_get_npad++; fill_common(out); }

/* void nn::hid::GetTouchScreenState<16>(TouchScreenState<16> *out)
   TouchScreenState 布局（Horizon）：
     +0x00 u64 SamplingNumber
     +0x08 u32 Attribute
     +0x0C u32 TouchCount（模板参数 N 决定上限）
     +0x10 起 每个触摸点：{u32 X, u32 Y, u32 DiameterX, u32 DiameterY, u32 RotationAngle, u32 Reserved}
   不同模板参数（16/8/…)只影响 TouchCount 的上限与结构体总大小，前 0x10 与点布局一致。 */
void dm_nn_hid_GetTouchScreenState16(void *out) {
    g_hid_stats.n_get_touch++;
    uint8_t *p = (uint8_t *)out;
    if (!p) return;
    memset(p, 0, 0x10);
    *(uint64_t *)(p + 0x00) = 1;                 /* SamplingNumber */
    *(uint32_t *)(p + 0x08) = 0;                 /* Attribute */
    *(uint32_t *)(p + 0x0C) = (uint32_t)g_in.touch_count;
    if (g_in.touch_count) {
        uint32_t *t = (uint32_t *)(p + 0x10);
        t[0] = g_in.touch_x;
        t[1] = g_in.touch_y;
        t[2] = g_in.touch_diameter;
        t[3] = g_in.touch_diameter;
        t[4] = g_in.touch_angle;
        t[5] = 0;
    }
}

/* 震动：记录即可（iPad 的触觉反馈可后续接 UIImpactFeedbackGenerator） */
void dm_nn_hid_SendVibrationValue(const void *h, const void *v) { (void)h; (void)v; g_hid_stats.n_vib++; }
void dm_nn_hid_InitializeVibrationDevice(const void *h) { (void)h; }
void dm_nn_hid_GetVibrationDeviceHandles(void *out, int n, const uint32_t *id, uint32_t style) {
    (void)id; (void)style;
    if (out) memset(out, 0, (size_t)n * 4);
}

/* 六轴：不做真体感，给单位姿态即可（引擎一般只用于镜头微调，缺失不影响可玩性） */
void dm_nn_hid_StartSixAxisSensor(const void *h) { (void)h; }
void dm_nn_hid_GetSixAxisSensorHandles(void *out, int n, const uint32_t *id, uint32_t style) {
    (void)id; (void)style;
    if (out) memset(out, 0, (size_t)n * 4);
}
void dm_nn_hid_GetSixAxisSensorState(void *out, const void *h) {
    (void)h;
    if (!out) return;
    /* SixAxisSensorState：
         +0x00 u64 SamplingNumber, +0x08 u64 DeltaTime
         +0x10 float[3] Accel, +0x1C float[3] Gyro, +0x28 float[3] Angle
         +0x34 float[4] Rotation（四元数）
       「不动」的姿态：加速度 = 重力向下 (0,0,-1)，角速度 0，四元数单位。 */
    memset(out, 0, 0x50);
    float *f = (float *)((uint8_t *)out + 0x10);
    f[0] = 0.0f; f[1] = 0.0f; f[2] = -1.0f;      /* 重力 */
    float *q = (float *)((uint8_t *)out + 0x34);
    q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f;
}

/* ================= nn::oe ================= */

static void *g_oe_events[4] = { 0, 0, 0, 0 };   /* 分辨率变化事件（宿主自建） */

void dm_nn_oe_Initialize(void) { }

/* ================= 服务层初始化（强制接管，一律空操作） =================
 * 依据：D2R Mobile 真机源码 dm/host/hle_sdkinit.c（MIT，work/ref/d2rm-src）。
 * nninitInitializeSdkModule 会依次调下面这几个「连 Horizon 系统服务（IPC）」的
 * 初始化函数。iOS 上没有 sm: 之外的那些服务进程，真机连不上；而 SDK 里那份实现
 * 会用 nn::sf::hipc 的服务描述表去 ConnectToHipcService —— 那张表的名字指针由
 * 全局构造填，没填就是裸偏移（第 61 轮 .ips 实证：strlen(0x8ffad7) SIGSEGV）。
 * 所以照 D2R：**不建会话，直接返回**；各服务的后续接口在用到时再逐个接管。 */
void dm_nn_fs_InitializeForApplication(void) { dm_diag_mark("init 接管：fs::InitializeForApplication（空操作）"); }
void dm_nn_diag_AbortObserver(void)          { dm_diag_mark("init 接管：diag::InitializeApplicationAbortObserver（空操作）"); }
void dm_nn_err_InitCtx(void)                 { dm_diag_mark("init 接管：err::InitializeContextWriterApi（空操作）"); }
void dm_nn_aoc_Initialize(void)              { dm_diag_mark("init 接管：aoc::Initialize（空操作）"); }
void dm_nn_pctl_Initialize(void)             { dm_diag_mark("init 接管：pctl::Initialize（空操作）"); }
void dm_nn_nndiagStartup(void)               { dm_diag_mark("init 接管：nndiagStartup（空操作）"); }

/* ================= 客体 libc 的 DSO 初始化（第 72 轮接管） =================
 *
 * 各模块的 _init（DT_INIT）形状都一样（sdk+0x400 / main+0x10 / …）：
 *   ① __nnmusl_init_dso(模块信息表…)        ← 若返回 1 → 跳过 ②③ 直接收尾
 *   ② nn::ro::ProtectRelro(…)                ← 宿主已接管成空操作
 *   ③ 自己遍历 init_array 调构造函数
 *
 * 为什么必须接管（本轮 .ips）：那张「模块信息表」是 **rtld 在装载模块时**填的
 * （本工程没有 rtld，见第 71 轮），表里全是裸偏移 —— ① 的第一个参数
 * （sdk+0xb685f0）现在是 0，而 SDK 那份 __nnmusl_init_dso 开头就是
 * `cbz x0 → return 2` ⇒ 返回 2 ⇒ 标志位 ≠ 1 ⇒ _init 走 ③ ⇒
 * 拿裸偏移 0xb2f000 当构造函数表指针 → 读 0xb2f000（未映射）SIGSEGV。
 * 真机上 ① 做的是 musl 的 DSO 登记（dlopen/dlclose/atexit 的 dso 链），
 * 我们不模拟它；而 ③ 与我们自己的 init_array 循环**重复**（条目基址已由
 * dm_loader_fix_init_arrays 补好）。
 * ⇒ 接管成「返回 1」：_init 收尾，构造函数只由宿主跑一遍。 */
int dm_nn_nnmusl_init_dso(void) {
    dm_diag_mark("init 接管：__nnmusl_init_dso → 1"
                 "（DSO 登记不模拟；构造函数由宿主的 init_array 循环跑）");
    return 1;
}

/* ================= 别名区的栈地址分配器（第 77 轮接管） =================
 *
 * 真机 .ips 实证的完整链条：nnMain 里游戏创建 nn::os 线程 →
 * ThreadManagerImplByHorizon::MapAliasStack →
 * StackGuardManager::MapAtRandomAddress（在别名区找随机位置放线程栈）→
 * AddressSpaceAllocatorBase::AllocateSpace —— **返回 0（没有空间）** ⇒
 * MapAtRandomAddress 返回 1 ⇒ SDK 视为不可能状态 → nn::diag::AbortImpl
 * → Break → 停车循环（b #0）→ 主线程卡死 → 退后台 10 秒被看门狗杀。
 *
 * 为什么返回 0：AllocateSpace 依赖 StackGuardManager 里一段「区域范围」
 * 内部状态（manager+0xB0/0xC0），真机上由装载器/初始化用别名区信息填好；
 * 我们的移植没人填（空区间）⇒ AllocateSpace 开头的
 * `cmp [start],[end]; b.eq → return 0` 直接命中。
 *
 * ⇒ 接管成「宿主从别名区（512MB）顺序分配」：64K 对齐、记一个游标。
 * 返回的地址随后经 commit 回调走 svc 0x2C（MapPhysicalMemory，宿主已实现
 * —— 把别名页 mprotect 成 R|W）⇒ 线程栈真实可用。 */
uint64_t dm_nn_addr_alloc_space(uint64_t self, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4) {
    static uint64_t cur;
    static int n_log;
    void *base = 0; size_t cap = 0;
    extern void dm_guest_alias_range(void **base, size_t *size);   /* hle.c */
    dm_guest_alias_range(&base, &cap);
    if (!base || !cap) return 0;
    if (!cur) cur = (uint64_t)base;
    uint64_t size = (a1 + 0xffff) & ~(uint64_t)0xffff;   /* 64K 对齐（含 guard 余量） */
    if (size < 0x10000) size = 0x10000;
    if (cur + size > (uint64_t)base + cap) return 0;      /* 别名区耗尽 */
    uint64_t addr = cur;
    cur += size;
    (void)self; (void)a2; (void)a3; (void)a4;
    if (n_log++ < 8)
        dm_diag_mark("init 接管：AllocateSpace → %#llx（+%#llx，别名区顺序分配）",
                     (unsigned long long)addr, (unsigned long long)size);
    return addr;
}

/* int nn::oe::GetOperationMode() —— 0 = Handheld, 1 = Console(Docked) */
int dm_nn_oe_GetOperationMode(void) { return 0; }   /* iPad 当作掌机 */

/* nn::oe::GetCurrentFocusState() —— 0 = NotInFocus, 1 = InFocus */
int dm_nn_oe_GetCurrentFocusState(void) { return 1; }

void dm_nn_oe_SetCpuBoostMode(int mode) { (void)mode; }

/* nn::oe::GetDefaultDisplayResolution(int *w, int *h) */
void dm_nn_oe_GetDefaultDisplayResolution(int *w, int *h) {
    if (w) *w = 1920;      /* iPad 的常见分辨率；真机侧应读实际屏幕 */
    if (h) *h = 1080;
}

/* nn::oe::GetDefaultDisplayResolutionChangeEvent(SystemEventType *) */
void dm_nn_oe_GetDefaultDisplayResolutionChangeEvent(void *ev) {
    /* 客体要一个「分辨率变化事件」。宿主给一个永不触发的事件即可 ——
       iPad 的分辨率在运行期不会变。 */
    dm_nn_InitializeSystemEvent(ev, 1);
}

/* ================= nn::time ================= */

#include <time.h>

void dm_nn_time_Initialize(void) { }

/* 第 65 轮：下面三个照 D2R Mobile 的 hle_time.c 接管。
 * 为什么必须接管：SDK 的 nn::time 客户端内部会去 sm: 要 "time:u" 服务会话
 * （真机日志实证：diag::AbortObserver 之后立刻 ConnectToNamedPort("sm:")、
 *   SendSyncRequest 里带着 "time:u"，随后 [Break] reason=0 致命中止）。
 * 宿主没有 Horizon 服务进程，假 IPC 只回成功 → SDK 拿到空会话 → 走错误路径。
 * D2R 的做法是**根本不让它连**：整族强制接管成宿主实现（dm/host/hle_time.c）。
 *   IsInitialized → 恒 1（已初始化）
 *   now()          → 宿主 time()（秒）
 *   to_time_t(tp) → *tp（与 now() 配套，TimePoint 就是秒数） */
int dm_nn_time_IsInitialized(void) { return 1; }
int64_t dm_nn_time_now(void) { return (int64_t)time(NULL); }
int64_t dm_nn_time_to_time_t(const int64_t *tp) { return tp ? *tp : 0; }

/* nn::time::StandardUserSystemClock::GetCurrentTime(PosixTime *out)
   PosixTime 是 int64 秒 + int32 纳秒（共 16 字节，按 8 字节对齐）。 */
void dm_nn_time_GetCurrentTime(void *out) {
    if (!out) return;
    time_t sec = time(NULL);
    *(int64_t *)out = (int64_t)sec;
    *(int32_t *)((uint8_t *)out + 8) = 0;
}

void *dm_nn_time_ToCalendarTime(void *out_cal, void *out_add, const void *posix) {
    if (!out_cal || !posix) return out_cal;
    time_t sec = (time_t)(*(const int64_t *)posix);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &sec);
#else
    gmtime_r(&sec, &tmv);
#endif
    /* CalendarTime：{u16 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u8 pad} */
    uint8_t *p = (uint8_t *)out_cal;
    *(uint16_t *)(p + 0) = (uint16_t)(tmv.tm_year + 1900);
    p[2] = (uint8_t)(tmv.tm_mon + 1);
    p[3] = (uint8_t)tmv.tm_mday;
    p[4] = (uint8_t)tmv.tm_hour;
    p[5] = (uint8_t)tmv.tm_min;
    p[6] = (uint8_t)tmv.tm_sec;
    p[7] = 0;
    if (out_add) memset(out_add, 0, 0x20);
    return out_cal;
}

/* ================= nn::fontll / codec / web / swkbd / account / diag / init / nifm =================
 *
 * 这些子系统的完整实现工作量很大，但对「能启动、能玩」不构成阻塞。
 * 宿主先给**最小可用实现**（成功返回 + 合理的空状态），并在诊断里计数，
 * 以便后续按实际调用频次决定要不要真做。
 */

static struct {
    uint64_t n_fontll, n_codec, n_web, n_swkbd, n_account, n_diag, n_init, n_nifm;
} g_misc_stats;
const void *dm_misc_stats_ptr(void) { return &g_misc_stats; }

/* nn::fontll —— 字体库。宿主侧没有实现文本光栅化，
   返回成功的空结果；引擎若真需要字形会走别的路径（或用系统字体）。 */
int  dm_nn_fontll_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_fontll++;
    return 0;
}

/* nn::codec —— 编解码（JPEG/PNG 等）。宿主可后续接 stb_image 之类。 */
int  dm_nn_codec_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_codec++;
    return 0;
}

/* nn::web —— 显示网页（一般是错误页/手册）。宿主侧不实现浏览器，返回成功。 */
int  dm_nn_web_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_web++;
    return 0;
}
void dm_nn_web_ctor(void *self, const char *s) { (void)s; if (self) memset(self, 0, 0x20); }
void dm_nn_web_dtor(void *self) { if (self) memset(self, 0, 0x20); }

/* nn::swkbd —— 软键盘。iPad 上用系统键盘；宿主先返回「已取消」。 */
int  dm_nn_swkbd_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_swkbd++;
    return 0;      /* 0 = 成功但未输入（引擎一般按空串处理） */
}

/* nn::account —— 账号。离线时给一个空账号。 */
int  dm_nn_account_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_account++;
    return 0;
}

/* nn::diag —— 断言失败入口。宿主应当把它接到自己的崩溃报告上，
   而不是真的 abort（否则一个可恢复的断言会直接杀掉进程）。 */
void dm_nn_diag_AbortImpl(const void *result) {
    (void)result;
    g_misc_stats.n_diag++;
    /* 真机侧：记录 + 抛出宿主异常，由宿主决定是否终止 */
}

/* nn::init —— SDK 初始化入口（Start）。宿主只需成功返回。 */
int  dm_nn_init_Start(uint64_t a1, uint64_t a2, void *f1, void *f2, void *f3) {
    (void)a1; (void)a2; (void)f1; (void)f2; (void)f3;
    g_misc_stats.n_init++;
    return 0;
}

/* nn::nifm —— 网络接口管理。离线不需要，返回「无网络」。 */
int  dm_nn_nifm_stub(void *a1, void *a2, void *a3, void *a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    g_misc_stats.n_nifm++;
    return 0;
}
void dm_nn_nifm_Initialize(void) { g_misc_stats.n_nifm++; }
