/* 宿主图形层 —— Metal API 编码（Apple 专用）。
 *
 * ============================================================================
 * 这是整个项目里**唯一必须用 Mac 编译的文件**。
 * 其余各层都已在 Windows 上写完并单测过（见 work/porting/ 的各轮报告）。
 * ============================================================================
 *
 * 【本文件与 hle_nvn_mtl.c 的分工】
 *   hle_nvn_mtl.c（平台无关，已测）：状态机 —— 去重、pass 分组、一致性检查、
 *                                    把「命令」翻成「后端操作」
 *   本文件（Apple 专用）：把后端操作翻成 MTL* 调用
 *
 * 【为什么状态机不在这里】
 *   真机上「画面不对」最常见的原因是状态机错（该 setState 的没 set、该开新 pass 的
 *   没开），而这类错**不报错、只画错**。把状态机放在可测的一侧，调试时间能砍一半。
 *
 * 【本文件里 TODO 的位置】
 *   下面每个 TODO 都是「需要真机跑起来才能确定」的决策点（枚举映射的具体取值、
 *   NVN 对象 → MTL 对象的查找表等）。**不要凭猜填这些**，
 *   第 26 轮的枚举映射表（hle_nvn_metal.c）已经把能确定的都确定了。
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "hle_nvn_mtl.h"
#include "hle_nvn_metal.h"      /* NVN → Metal 枚举映射（已有 49 个取值，Windows 上单测过） */
#include "hle_nvn_obj.h"        /* NVN 对象层（device/pool/buffer/texture，已实现） */

#include <string.h>

/* ---------------- 状态 ---------------- */

static id<MTLDevice>              g_dev = nil;
static CAMetalLayer              *g_layer = nil;
static id<MTLCommandQueue>        g_queue = nil;
static id<MTLCommandBuffer>       g_cb = nil;
static id<MTLRenderCommandEncoder> g_enc = nil;
static id<CAMetalDrawable>        g_drawable = nil;

int dm_mtl_apply_init(void *mtl_device, void *mtl_layer) {
    if (!mtl_device) return -1;
    g_dev   = (__bridge id<MTLDevice>)mtl_device;
    g_queue = [g_dev newCommandQueue];
    if (!g_queue) return -2;
    if (mtl_layer) {
        g_layer = (__bridge CAMetalLayer *)mtl_layer;
        g_layer.device = g_dev;
    }
    return 0;
}

/* ---------------- 通道 ---------------- */

void dm_mtl_apply_pass_begin(void) {
    if (g_enc) return;                       /* 已有通道 */
    if (!g_cb) g_cb = [g_queue commandBuffer];
    if (!g_drawable && g_layer) g_drawable = [g_layer nextDrawable];

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    /* TODO(真机): 渲染目标要来自客体绑定的纹理（NVN 的 RenderTarget），
       而不是这里的 drawable —— 客体通常先画到自己的纹理，最后才呈现到 drawable。
       需要把 nvnCommandBufferBindTexture 里认出来的「渲染目标」记下来，
       在这里按 NVN 的格式查 MTLPixelFormat（用 dm_nvn_format_to_metal）。 */
    if (g_drawable) {
        rp.colorAttachments[0].texture     = g_drawable.texture;
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;   /* clear 由 clear 命令决定 */
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
}

void dm_mtl_apply_pass_end(void) {
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
}

void dm_mtl_apply_frame_end(void) {
    dm_mtl_apply_pass_end();
    if (g_cb) {
        if (g_drawable) { [g_cb presentDrawable:g_drawable]; g_drawable = nil; }
        [g_cb commit];
        g_cb = nil;
    }
}
