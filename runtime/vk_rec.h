// The recorder interface of TotK Recomp runtime/vk_rec.h @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), with
// the recorder thread (vk_rec.c) not ported: every call is made at once on the calling thread, as TotK Recomp does with
// TOTK_VK_RECORDER=0. vk_sync.c begins each part with rec_begin, ends it with rec_end_cb and submits it with
// rec_submit; the extension commands called through function pointers go through the rec_* wrappers. Porting vk_rec.c
// replaces this header with the original one.
#pragma once
#include <vulkan/vulkan.h>

void vk_queue_lock(void);   // vk.h; gfx_present.c
void vk_queue_unlock(void);
VkCtx *vk_ctx(void);

// vkBeginCommandBuffer for one submission. fence, crumb and n are the recorder's and the breadcrumbs' (not ported).
static inline void rec_begin(VkCommandBuffer cb, uint64_t fence, int crumb, uint64_t n) {
    (void)fence, (void)crumb, (void)n;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
}
static inline void rec_end(void) {}
// 0: the caller ends cb itself.
static inline int rec_end_cb(VkCommandBuffer cb, int crumb, uint64_t n) { (void)cb, (void)crumb, (void)n; return 0; }
static inline VkResult rec_submit(const VkSubmitInfo2 *si) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(vk_ctx()->gfx_queue, 1, si, VK_NULL_HANDLE);
    vk_queue_unlock();
    return r;
}
static inline void rec_drain(void) {}
static inline void rec_queue_wait(void) {}
static inline int rec_owns(VkCommandBuffer cb) { (void)cb; return 0; }
static inline void rec_stats_log(void) {}
enum { REC_IMAGE, REC_VIEW, REC_BUFFER, REC_KINDS };
static inline void rec_check_handle(int kind, uint64_t handle, int destroy, uint64_t tl, uint64_t gpu) {
    (void)kind, (void)handle, (void)destroy, (void)tl, (void)gpu;
}
static inline void rec_check_log(void) {}

// Extension commands called through device function pointers.
static inline void rec_push_tpl(PFN_vkCmdPushDescriptorSetWithTemplateKHR fn, VkCommandBuffer cb,
                                VkDescriptorUpdateTemplate tpl, VkPipelineLayout layout, uint32_t set, const void *data,
                                uint32_t bytes) {
    (void)bytes;
    fn(cb, tpl, layout, set, data);
}
static inline void rec_push(PFN_vkCmdPushDescriptorSetKHR fn, VkCommandBuffer cb, VkPipelineBindPoint bp,
                            VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    fn(cb, bp, layout, set, n, w);
}
static inline void rec_vertex_input(PFN_vkCmdSetVertexInputEXT fn, VkCommandBuffer cb, uint32_t nb,
                                    const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                                    const VkVertexInputAttributeDescription2EXT *a) {
    fn(cb, nb, b, na, a);
}
static inline void rec_bool(PFN_vkCmdSetDepthClampEnableEXT fn, VkCommandBuffer cb, VkBool32 v) { fn(cb, v); }
static inline void rec_polygon_mode(PFN_vkCmdSetPolygonModeEXT fn, VkCommandBuffer cb, VkPolygonMode m) { fn(cb, m); }
static inline void rec_blend_enable(PFN_vkCmdSetColorBlendEnableEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                                    const VkBool32 *v) {
    fn(cb, first, n, v);
}
static inline void rec_blend_equation(PFN_vkCmdSetColorBlendEquationEXT fn, VkCommandBuffer cb, uint32_t first,
                                      uint32_t n, const VkColorBlendEquationEXT *v) {
    fn(cb, first, n, v);
}
static inline void rec_write_mask(PFN_vkCmdSetColorWriteMaskEXT fn, VkCommandBuffer cb, uint32_t first, uint32_t n,
                                  const VkColorComponentFlags *v) {
    fn(cb, first, n, v);
}
