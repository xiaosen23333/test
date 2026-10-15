/* 宿主 HLE —— nn::hid / nn::oe / nn::time / 其余小组的声明。
 * 与 hle_nn.h 配套（那边是 os/fs/ro/oe 的基础部分）。
 */
#ifndef DM_HLE_NN_MORE_H
#define DM_HLE_NN_MORE_H

#include <stdint.h>

/* ---------- nn::hid ---------- */
void     dm_nn_hid_InitializeNpad(void);
extern const uint32_t dm_nn_hid_NpadId_Handheld;
void     dm_nn_hid_InitializeTouchScreen(void);
void     dm_nn_hid_SetSupportedNpadStyleSet(uint32_t style);
void     dm_nn_hid_SetSupportedNpadIdType(const uint32_t *ids, uint64_t n);
uint32_t dm_nn_hid_GetNpadStyleSet(const uint32_t *id);
void     dm_nn_hid_GetNpadState_FullKey(void *out, const uint32_t *id);
void     dm_nn_hid_GetNpadState_Handheld(void *out, const uint32_t *id);
void     dm_nn_hid_GetNpadState_JoyDual(void *out, const uint32_t *id);
void     dm_nn_hid_GetNpadState_JoyLeft(void *out, const uint32_t *id);
void     dm_nn_hid_GetNpadState_JoyRight(void *out, const uint32_t *id);
void     dm_nn_hid_GetTouchScreenState16(void *out);
void     dm_nn_hid_SendVibrationValue(const void *h, const void *v);
void     dm_nn_hid_InitializeVibrationDevice(const void *h);
void     dm_nn_hid_GetVibrationDeviceHandles(void *out, int n, const uint32_t *id, uint32_t style);
void     dm_nn_hid_StartSixAxisSensor(const void *h);
void     dm_nn_hid_GetSixAxisSensorHandles(void *out, int n, const uint32_t *id, uint32_t style);
void     dm_nn_hid_GetSixAxisSensorState(void *out, const void *h);

/* 宿主前端喂输入（iPad 的触摸/虚拟手柄从这里进来） */
void dm_input_set_buttons(uint32_t b);
void dm_input_set_stick(int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void dm_input_set_touch(int active, uint32_t x, uint32_t y);
const void *dm_hid_stats_ptr(void);

/* ---------- nn::oe 扩展 ---------- */
void dm_nn_oe_Initialize(void);
int  dm_nn_oe_GetOperationMode(void);
int  dm_nn_oe_GetCurrentFocusState(void);
void dm_nn_oe_SetCpuBoostMode(int mode);
void dm_nn_oe_GetDefaultDisplayResolution(int *w, int *h);
void dm_nn_oe_GetDefaultDisplayResolutionChangeEvent(void *ev);
void dm_nn_InitializeSystemEvent(void *e, int clear_mode);
void dm_nn_DestroySystemEvent(void *e);
void dm_nn_SignalSystemEvent(void *e);
void dm_nn_ClearSystemEvent(void *e);
void dm_nn_TimedWaitSystemEvent(void *e, int64_t ns);
int  dm_nn_TimedWaitSystemEvent_rc(void *e, int64_t ns);

/* ---------- nn::time ---------- */
void  dm_nn_time_Initialize(void);
void  dm_nn_time_GetCurrentTime(void *out);
void *dm_nn_time_ToCalendarTime(void *out_cal, void *out_add, const void *posix);

/* ---------- 其余小组（最小可用实现） ---------- */
int  dm_nn_fontll_stub(void *a1, void *a2, void *a3, void *a4);
int  dm_nn_codec_stub(void *a1, void *a2, void *a3, void *a4);
int  dm_nn_web_stub(void *a1, void *a2, void *a3, void *a4);
void dm_nn_web_ctor(void *self, const char *s);
void dm_nn_web_dtor(void *self);
int  dm_nn_swkbd_stub(void *a1, void *a2, void *a3, void *a4);
int  dm_nn_account_stub(void *a1, void *a2, void *a3, void *a4);
void dm_nn_diag_AbortImpl(const void *result);
int  dm_nn_init_Start(uint64_t a1, uint64_t a2, void *f1, void *f2, void *f3);
int  dm_nn_nifm_stub(void *a1, void *a2, void *a3, void *a4);
void dm_nn_nifm_Initialize(void);
const void *dm_misc_stats_ptr(void);

/* 服务层初始化：nninitInitializeSdkModule 调的「连 Horizon 服务进程」那批，
   宿主没有服务进程 → 一律空操作（依据 D2R Mobile dm/host/hle_sdkinit.c） */
void dm_nn_fs_InitializeForApplication(void);
void dm_nn_diag_AbortObserver(void);
void dm_nn_err_InitCtx(void);
void dm_nn_aoc_Initialize(void);
void dm_nn_pctl_Initialize(void);
void dm_nn_nndiagStartup(void);

/* ---------- 程序化生成的实现表 ---------- */
void *dm_nn_impl_lookup(const char *name);
unsigned dm_nn_impl_count(void);
const char *dm_nn_impl_note(unsigned i);

#endif /* DM_HLE_NN_MORE_H */
