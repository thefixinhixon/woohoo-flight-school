/* Vulkan PICA200 renderer. Vertices are shaded on the CPU (pica_sw.c, worker pool); clipping, rasterization,
 * texturing, lighting, TEV, alpha/depth test and blending run on the GPU (shaders/pica.frag mirrors pica_sw.c).
 *
 * By default the guest vertex shader also runs on the GPU (shaders/pica_vs.vert interprets its bytecode; the CPU
 * only fetches attributes); PWR_VK_CPU_VS=1 uses the CPU interpreter's results instead (reference for A/B checks).
 *
 * Guest memory stays authoritative, because the game reads render targets back (DisplayTransfer to the LCD
 * framebuffers, TextureCopy, sampling a target as a texture):
 *   - draws are recorded as DrawRec and replayed grouped by colour target ("seal"), because the game interleaves
 *     draws to its two eye targets and a render pass per switch cost ~27 ms of GPU time per frame. Order per target
 *     is kept; a segment is sealed early when a draw samples a target drawn in it, or when two colour targets would
 *     share a depth buffer (their interleaving matters then);
 *   - a batch is one submission holding any number of render passes; it ends at the end of a command list, or
 *     early when a draw samples a texture overlapping a colour target drawn in this batch. Ending it: submit, wait,
 *     convert every colour target drawn in it back into the tiled guest framebuffer;
 *   - a target is uploaded from guest memory at the start of a batch only when the guest bytes changed since we last
 *     synced them (64-bit hash), e.g. after a MemoryFill clear. Depth is not written back (games here don't read it);
 *     its image stays authoritative until the guest bytes change;
 *   - textures are decoded to RGBA8 on the CPU and cached by (address, size, format), re-checked by hash once per
 *     command list. A texture whose bytes change after it was used in the current batch gets a new image.
 * Fog (mode 5) and stencil (D24S8) as in pica_sw.c. Not yet: gas, procedural textures, cube/shadow/projected textures, logic ops. */
#include "pica_int.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#define R(i) pica_regs[i]
VkShaderModule pica_vsgen_module(VkDevice dev);   /* pica_vsgen.c */

int video_vk(VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *family);
void video_queue_lock(void);
void video_queue_unlock(void);

static const uint32_t vert_spv[] =
#include "pica.vert.inc"
;
static const uint32_t vs_spv[] =
#include "pica_vs.vert.inc"
;
static const uint32_t frag_spv[] =
#include "pica.frag.inc"
;
static const uint32_t frag_nd_spv[] =   /* same shader without the gl_FragDepth write */
#include "pica_nd.frag.inc"
;

static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkCommandPool cpool; static VkCommandBuffer cb_up, cb_draw; static VkFence fence;
/* a batch is submitted in parts (see kick()): each part has its own upload and draw command buffers */
#define MAX_PARTS 32
static VkCommandBuffer cb_part[MAX_PARTS][2]; static int part;
static VkDescriptorSetLayout dsl; static VkPipelineLayout playout; static VkShaderModule vs_mod, vsi_mod, fs_mod, fs_nd_mod;
static int gpu_vs = 1;
static PFN_vkCmdPushDescriptorSetKHR push_desc;
static VkFormat depth_format;
static int has_stencil; static VkImageAspectFlags ds_aspect;   /* D32_SFLOAT_S8_UINT when available: stencil for D24S8 targets */
static int state = -1;   /* -1 unknown, 0 off, 1 on */
static int vtrace;       /* PWR_VK_TRACE=1: log target uploads and fills */
static int q_res = 1, q_aniso = 1;   /* video.c settings: 3D internal resolution, anisotropic filtering (1 = off) */
static int cur_scale = 1;            /* render targets are cur_scale times their guest size (q_res, applied between batches) */
static VkCommandBuffer cb_scan; static VkFence scan_fence; static int scan_used;   /* scan-out copies (below) */

/* ---------------- memory ---------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } Ring;
static int ring_init(Ring *r, VkDeviceSize size, VkBufferUsageFlags usage, int cached) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    if (vkCreateBuffer(dev, &bi, NULL, &r->buf)) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r->buf, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    /* CPU reads (downloads) need cached memory: reads from write-combined memory are very slow */
    if (cached) { uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT); if (t != UINT32_MAX) ai.memoryTypeIndex = t; }
    if (vkAllocateMemory(dev, &ai, NULL, &r->mem) || vkBindBufferMemory(dev, r->buf, r->mem, 0)) return -1;
    if (vkMapMemory(dev, r->mem, 0, size, 0, (void **)&r->ptr)) return -1;
    r->size = size; r->used = 0; return 0;
}
/* space in a ring for this batch, or UINT64_MAX when full (caller flushes and retries) */
static VkDeviceSize ring_alloc(Ring *r, VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = (r->used + align - 1) / align * align;
    if (o + n > r->size) return UINT64_MAX;
    r->used = o + n; return o;
}
static Ring vbuf, ibuf, ubuf, lbuf, upbuf, dlbuf, abuf, pbuf;   /* abuf: vertex attributes, pbuf: shader programs */

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;
static int img_create_mip(Img *im, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(dev, &ci, NULL, &im->img)) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (vkAllocateMemory(dev, &ai, NULL, &im->mem) || vkBindImageMemory(dev, im->img, im->mem, 0)) return -1;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .subresourceRange = { aspect, 0, levels, 0, 1 } };
    return vkCreateImageView(dev, &vi, NULL, &im->view) ? -1 : 0;
}
static int img_create(Img *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    return img_create_mip(im, w, h, 1, fmt, usage, aspect);
}
static Img dead[512]; static int ndead;   /* destroyed after the batch that may still use them */
static void img_retire(Img *im) {
    if (ndead == 512) { video_queue_lock(); vkQueueWaitIdle(queue); video_queue_unlock();
        for (int i = 0; i < ndead; i++) { vkDestroyImageView(dev, dead[i].view, NULL); vkDestroyImage(dev, dead[i].img, NULL); vkFreeMemory(dev, dead[i].mem, NULL); }
        ndead = 0; }
    dead[ndead++] = *im; memset(im, 0, sizeof *im);
}

static uint64_t hash_mem(uint32_t addr, uint32_t size) {
    /* four independent lanes: one multiply chain was latency-bound (~1.5 bytes per cycle), and every texture and
       target in use is hashed once per command list */
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ size, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    const uint8_t *p = g_mem + addr;
    uint32_t i = 0;
    for (; i + 32 <= size; i += 32)
        for (int l = 0; l < 4; l++) { uint64_t v; memcpy(&v, p + i + 8 * l, 8); h[l] = (h[l] ^ v) * 0x100000001B3ull; h[l] ^= h[l] >> 29; }
    uint64_t r = h[0] ^ (h[1] * 0x9E3779B97F4A7C15ull) ^ (h[2] * 0xC2B2AE3D27D4EB4Full) ^ (h[3] * 0x165667B19E3779F9ull);
    for (; i < size; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r;
}

/* ---------------- batch / command buffers ---------------- */
static int recording;
static int st_valid;   /* the dynamic state of the open draw command buffer is known (cleared when it begins) */
static int seal_before_draw;   /* set by tex_get: this draw samples a target drawn in the open segment */
static uint32_t lut_uploaded_version = UINT32_MAX; static VkDeviceSize lut_off;   /* LUT copy in this batch's ring */
static uint32_t prog_uploaded_version = UINT32_MAX; static VkDeviceSize prog_off;  /* same for the vertex shader program */
unsigned long long g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vk_passes, g_vk_gpu_us, g_vk_wait_us, g_vk_flush_us, g_vk_verts, g_vk_idx;
/* one command list's costs, for the "slow list" report in pica_vk_flush (lists of 50 ms and more) */
static struct { double t0, pipe_ms, tex_ms, surf_ms; unsigned pipes, texs, surfs; unsigned long long draws0, passes0; } bs;
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; }
static VkQueryPool qpool; static float ts_period;
static uint32_t batch_no, list_no = 1;   /* list_no: bumped at every flush; guest bytes are re-checked once per value */
static void barrier_all(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da };
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
static void to_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void begin_batch(void) {
    if (recording) return;
    vbuf.used = ibuf.used = ubuf.used = lbuf.used = upbuf.used = dlbuf.used = abuf.used = pbuf.used = 0;
    lut_uploaded_version = prog_uploaded_version = UINT32_MAX;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    part = 0; cb_up = cb_part[0][0]; cb_draw = cb_part[0][1];
    vkResetCommandBuffer(cb_up, 0); vkBeginCommandBuffer(cb_up, &bi);
    if (qpool) { vkCmdResetQueryPool(cb_up, qpool, 0, 2); vkCmdWriteTimestamp(cb_up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0); }
    vkResetCommandBuffer(cb_draw, 0); vkBeginCommandBuffer(cb_draw, &bi);
    st_valid = 0;
    recording = 1; batch_no++;
    memset(&bs, 0, sizeof bs); bs.t0 = now_ms(); bs.draws0 = g_vk_draws; bs.passes0 = g_vk_passes;
}

/* ---------------- render targets ---------------- */
typedef struct { uint32_t addr, w, h; int fmt, depth; Img im; uint64_t hash; int synced; uint32_t dirty_batch, checked_list, last_batch, seg_id; void *seg_color; VkDeviceSize dl_off;
                 int pending_clear; uint8_t clear_px[4], clear_st; Img down; int sub; uint32_t use_seq, frame_last, frames; } Surf;   /* clear_px: RGBA8, or float depth; clear_st: stencil;
                                                                          down: guest-size copy for write-back when cur_scale > 1;
                                                                          sub: a depth window into a larger depth target (see depth_window);
                                                                          frames: game frames in a row it was drawn to, up to frame_last */
#define MAX_SURF 64
static Surf surfs[MAX_SURF]; static int nsurf;
static Surf *cur_c, *cur_d;

static const uint8_t mx[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
static inline uint32_t fb_off(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp) {
    y = h - 1 - y;
    return (mx[x & 7] + my[y & 7] + (x & ~7u) * 8 + (y & ~7u) * w) * bpp;
}
static uint32_t surf_bpp(const Surf *s) {
    static const uint32_t cb[5] = { 4, 3, 2, 2, 2 }, db[4] = { 2, 3, 3, 4 };
    return s->depth ? db[s->fmt & 3] : cb[s->fmt < 5 ? s->fmt : 0];
}
static uint32_t surf_size(const Surf *s) { return s->w * s->h * surf_bpp(s); }

static Surf *surf_get(uint32_t addr, uint32_t w, uint32_t h, int fmt, int depth) {
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth && !s->sub) return s;
    }
    /* surfaces of different sizes may share an address (the game reuses one depth buffer for several passes); each
       keeps its own image and is kept coherent through the guest bytes' hash. When full, evict the least recently used. */
    Surf *s = NULL;
    if (nsurf < MAX_SURF) s = &surfs[nsurf++];
    else {
        for (int i = 0; i < nsurf; i++) {
            Surf *c = &surfs[i];
            if (c == cur_c || c == cur_d || c->dirty_batch == batch_no) continue;
            if (!s || c->last_batch < s->last_batch) s = c;
        }
        if (!s) return NULL;
        img_retire(&s->im); if (s->down.img) img_retire(&s->down);
    }
    memset(s, 0, sizeof *s);
    s->addr = addr; s->w = w; s->h = h; s->fmt = fmt; s->depth = depth;
    uint32_t S = (uint32_t)cur_scale;
    if (!depth && S > 1) {
        img_create(&s->down, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        to_general(cb_up, s->down.img, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    w *= S; h *= S;   /* the image; s->w/h stay the guest size */
    if (depth) img_create(&s->im, w, h, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, ds_aspect);
    else img_create(&s->im, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb_up, s->im.img, depth ? ds_aspect : VK_IMAGE_ASPECT_COLOR_BIT);
    return s;
}

/* one guest pixel -> RGBA8 (colour) or float (depth) */
static void decode_px(const Surf *s, const uint8_t *p, uint8_t *o) {
    uint32_t v;
    if (s->depth) {
        float z = s->fmt == 0 ? (float)(p[0] | p[1] << 8) / 65535.f : (float)(p[0] | p[1] << 8 | p[2] << 16) / 16777215.f;
        memcpy(o, &z, 4); return;
    }
    switch (s->fmt) {
    case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case 2: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 11) * 255 / 31); o[1] = (uint8_t)((v >> 6 & 31) * 255 / 31); o[2] = (uint8_t)((v >> 1 & 31) * 255 / 31); o[3] = (v & 1) ? 255 : 0; break;
    case 3: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 11) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255; break;
    default: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 12) * 17); o[1] = (uint8_t)((v >> 8 & 15) * 17); o[2] = (uint8_t)((v >> 4 & 15) * 17); o[3] = (uint8_t)((v & 15) * 17); break;
    }
}

/* GX MemoryFill over exactly a known target: the next batch clears the image on the GPU instead of uploading */
void pica_vk_filled(uint32_t start, uint32_t end) {
    if (state != 1) return;
    if (vtrace) fprintf(stderr, "vk: fill %08x-%08x\n", start, end);
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->addr < start || s->addr + surf_size(s) > end) continue;   /* the fill covers the whole surface */
        decode_px(s, g_mem + s->addr, s->clear_px);
        s->clear_st = s->depth && s->fmt == 3 ? g_mem[s->addr + 3] : 0;   /* D24S8: stencil in the top byte */
        s->pending_clear = 1; s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1;
    }
}

/* guest bytes -> image, through the upload ring (recorded in cb_up, which runs before cb_draw) */
static int surf_upload_(Surf *s);
static int surf_upload(Surf *s) { double t = now_ms(); int r = surf_upload_(s); bs.surf_ms += now_ms() - t; bs.surfs++; return r; }
static int surf_upload_(Surf *s) {
    uint32_t w = s->w, h = s->h, bpp = surf_bpp(s);
    if (vtrace) fprintf(stderr, "vk: upload %s %08x %ux%u fmt%d size %x (synced %d)\n", s->depth ? "depth" : "color", s->addr, w, h, s->fmt, surf_size(s), s->synced);
    uint32_t S = (uint32_t)cur_scale, W = w * S;   /* higher internal resolution: each guest pixel fills S x S image pixels */
    VkDeviceSize off = ring_alloc(&upbuf, (VkDeviceSize)w * h * 4 * S * S, 16);
    if (off == UINT64_MAX) return -1;
    uint8_t *d = upbuf.ptr + off;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint8_t px[4]; decode_px(s, g_mem + s->addr + fb_off(x, y, w, h, bpp), px);
        for (uint32_t j = 0; j < S; j++) for (uint32_t i = 0; i < S; i++) memcpy(d + ((size_t)(y * S + j) * W + x * S + i) * 4, px, 4);
    }
    VkBufferImageCopy cp = { .bufferOffset = off, .imageSubresource = { s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, h * S, 1 } };
    vkCmdCopyBufferToImage(cb_up, upbuf.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    if (s->depth && has_stencil) {   /* stencil aspect: the top byte of D24S8 pixels, 0 for the other depth formats */
        VkDeviceSize so = ring_alloc(&upbuf, (VkDeviceSize)w * h * S * S, 16);
        if (so == UINT64_MAX) return -1;
        uint8_t *sd = upbuf.ptr + so;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint8_t v = s->fmt == 3 ? g_mem[s->addr + fb_off(x, y, w, h, bpp) + 3] : 0;
            for (uint32_t j = 0; j < S; j++) memset(sd + (size_t)(y * S + j) * W + x * S, v, S);
        }
        VkBufferImageCopy sc = { .bufferOffset = so, .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { W, h * S, 1 } };
        vkCmdCopyBufferToImage(cb_up, upbuf.buf, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &sc);
    }
    s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1; g_vk_uploads++;
    return 0;
}

/* image (downloaded to dlbuf) -> guest bytes */
static void surf_writeback(Surf *s, VkDeviceSize off) {
    uint32_t w = s->w, h = s->h, bpp = surf_bpp(s);
    const uint8_t *src = dlbuf.ptr + off;
    {   /* PWR_VK_DUMPSURF=<hex addr>[,<hex addr>] or all (every FCRAM target): write that target to surf_<addr>.ppm (rgb) and _a.pgm (alpha), top row first */
        static char *dl = (char *)1; if (dl == (char *)1) dl = getenv("PWR_VK_DUMPSURF");
        char key[16]; snprintf(key, sizeof key, "%08x", s->addr);
        if (dl && (strstr(dl, key) || (!strcmp(dl, "all") && !s->depth && (s->addr < 0x1f000000u)))) {   /* "all": FCRAM targets */
            /* written under a temporary name and renamed, so a run killed mid-write leaves the previous complete dump */
            static int seq_mode = -1; static unsigned seq, seq_cap = 300; if (seq_mode < 0) { const char *e = getenv("PWR_VK_DUMPSURF_SEQ"); seq_mode = e != NULL; if (e && atoi(e) > 1) seq_cap = (unsigned)atoi(e); }   /* every write-back to its own file, up to 300 */
            char fn[80], gn[80], kname[32]; snprintf(kname, sizeof kname, seq_mode ? "%s_%03u" : "%s", key, seq);
            if (seq_mode && seq++ >= seq_cap) goto dump_done;
            snprintf(fn, sizeof fn, "surf_%s.ppm.tmp", kname); FILE *f = fopen(fn, "wb");
            snprintf(gn, sizeof gn, "surf_%s_a.pgm.tmp", kname); FILE *g = fopen(gn, "wb");
            if (f && g) {
                fprintf(f, "P6 %u %u 255\n", w, h); fprintf(g, "P5 %u %u 255\n", w, h);
                for (uint32_t y = h; y-- > 0;) for (uint32_t x = 0; x < w; x++) { fwrite(src + ((size_t)y * w + x) * 4, 1, 3, f); fputc(src[((size_t)y * w + x) * 4 + 3], g); }
            }
            if (f) fclose(f); if (g) fclose(g);
            if (f && g) {
                char d[80]; snprintf(d, sizeof d, "surf_%s.ppm", kname); rename(fn, d);
                snprintf(d, sizeof d, "surf_%s_a.pgm", kname); rename(gn, d);
            }
        }
        dump_done:;
    }
    if (!(w & 7) && !(h & 7) && (s->fmt == 0 || s->fmt == 1)) {
        /* RGBA8/RGB8 targets (the eye and display targets, every frame): walk the guest tiles in memory order with the
           in-tile offsets precomputed, instead of fb_off per pixel */
        static uint8_t ox[64], oy[64];
        if (!oy[63]) for (int i = 0; i < 64; i++) { ox[i] = (uint8_t)((i & 1) | (i >> 1 & 2) | (i >> 2 & 4)); oy[i] = (uint8_t)((i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4)); }
        uint8_t *p = g_mem + s->addr;
        for (uint32_t ty = 0; ty < h; ty += 8) for (uint32_t tx = 0; tx < w; tx += 8)
            for (int i = 0; i < 64; i++, p += bpp) {
                const uint8_t *c = src + ((size_t)(h - 1 - (ty + oy[i])) * w + tx + ox[i]) * 4;
                if (bpp == 4) { p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; }
                else { p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; }
            }
        s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1;
        return;
    }
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint8_t *c = src + ((size_t)y * w + x) * 4;
        uint8_t *p = g_mem + s->addr + fb_off(x, y, w, h, bpp);
        uint32_t r = c[0], g = c[1], b = c[2], a = c[3], v;
        switch (s->fmt) {
        case 0: p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)g; p[3] = (uint8_t)r; break;
        case 1: p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; break;
        case 2: v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        case 3: v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        default: v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        }
    }
    s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1;
}

/* ---------------- textures ---------------- */
typedef struct { uint32_t addr, w, h; int fmt, levels; Img im; uint64_t hash; uint32_t checked_list, used_batch, last_batch; } Tex;
static Tex *texs; static int ntex, cap_tex;
static Img white;

/* bytes of one mip level; levels follow each other in guest memory, level 0 first */
static uint32_t tex_level_size(const Tex *t, int l) { return (t->w >> l) * (t->h >> l) * (uint32_t)pica_tex_bpp[t->fmt] / 8; }

static int tex_upload_(Tex *t);
static int tex_upload(Tex *t) { double t0 = now_ms(); int r = tex_upload_(t); bs.tex_ms += now_ms() - t0; bs.texs++; return r; }
static int tex_upload_(Tex *t) {
    if (vtrace) fprintf(stderr, "vk: tex upload %08x %ux%u fmt%d levels %d\n", t->addr, t->w, t->h, t->fmt, t->levels);
    VkDeviceSize total = 0;
    for (int l = 0; l < t->levels; l++) total += (VkDeviceSize)(t->w >> l) * (t->h >> l) * 4;
    VkDeviceSize off = ring_alloc(&upbuf, total, 16);
    if (off == UINT64_MAX) return -1;
    VkDeviceSize boff = off; uint32_t maddr = t->addr;
    for (int l = 0; l < t->levels; l++) {
        uint32_t lw = t->w >> l, lh = t->h >> l;
        uint32_t *d = (uint32_t *)(upbuf.ptr + boff);
        for (uint32_t j = 0; j < lh; j++) for (uint32_t x = 0; x < lw; x++)   /* image row j = texture coordinate t; memory rows run top-down */
            d[(size_t)j * lw + x] = pica_texel(maddr, t->fmt, (int)lw, (int)x, (int)(lh - 1 - j));
        if (l == 0) {   /* PWR_VK_DUMPTEX=<hex va>[,<hex va>] or "all": write each upload of that texture (level 0) to tex_<va>_<n>.pam (RGBA, top row = t 1) */
            static char *dl = (char *)1; if (dl == (char *)1) dl = getenv("PWR_VK_DUMPTEX");
            char key[16]; snprintf(key, sizeof key, "%08x", t->addr);
            static int seq;
            if (dl && (!strcmp(dl, "all") || strstr(dl, key)) && seq < (strcmp(dl, "all") ? 64 : 400)) {
                char fn[64]; snprintf(fn, sizeof fn, "tex_%s_%02d.pam", key, seq++);
                FILE *f = fopen(fn, "wb");
                if (f) {
                    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", t->w, t->h);
                    for (uint32_t j = t->h; j-- > 0;) fwrite(d + (size_t)j * t->w, 4, t->w, f);
                    fclose(f);
                }
            }
        }
        VkBufferImageCopy cp = { .bufferOffset = boff, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1 }, .imageExtent = { lw, lh, 1 } };
        vkCmdCopyBufferToImage(cb_up, upbuf.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        boff += (VkDeviceSize)lw * lh * 4; maddr += tex_level_size(t, l);
    }
    g_vk_tex_uploads++;
    return 0;
}

/* image view for a texture unit's current registers; NULL when the upload ring is full (caller flushes) */
static VkImageView tex_get(int unit, int *full) {
    static const int dimr[3] = { 0x082, 0x092, 0x09A }, addrr[3] = { 0x085, 0x095, 0x09D }, fmtr[3] = { 0x08E, 0x096, 0x09E };
    uint32_t dim = R(dimr[unit]);
    uint32_t h = dim & 0x7ff, w = (dim >> 16) & 0x7ff, addr = pa_to_va(R(addrr[unit]) << 3);
    int fmt = (int)(R(fmtr[unit]) & 15);
    if (!w || !h || !addr || fmt > 13) return white.view;
    static const int lodr[3] = { 0x084, 0x094, 0x09C };
    static int nomip = -1; if (nomip < 0) nomip = getenv("PWR_VK_NOMIP") != NULL;   /* A/B switch: level 0 only */
    int levels = nomip ? 1 : (int)(R(lodr[unit]) >> 16 & 15) + 1;   /* max level (highest index) + 1 */
    while (levels > 1 && ((w >> (levels - 1)) < 8 || (h >> (levels - 1)) < 8)) levels--;   /* 8x8 is the smallest swizzle tile */
    uint32_t size = 0; for (int l = 0; l < levels; l++) size += (w >> l) * (h >> l) * (uint32_t)pica_tex_bpp[fmt] / 8;
    static long tdbg = -2; if (tdbg == -2) { const char *e = getenv("PWR_VK_TEXDBG"); tdbg = e ? strtol(e, NULL, 16) : -1; }   /* which path a texture takes */
    if (tdbg == (long)addr) {
        static int nt; if (nt++ < 200) {
            fprintf(stderr, "vk tex %08x %ux%u fmt %d list %u batch %u:", addr, w, h, fmt, list_no, batch_no);
            for (int i = 0; i < nsurf; i++) { Surf *s = &surfs[i];
                if (addr >= s->addr + surf_size(s) || s->addr >= addr + size) continue;
                fprintf(stderr, " [surf %08x %ux%u fmt %d depth %d dirty_batch %u%s]", s->addr, s->w, s->h, s->fmt, s->depth, s->dirty_batch, s == cur_c ? " current" : ""); }
            fputc('\n', stderr); }
    }
    for (int i = 0; i < nsurf; i++) {   /* render-to-texture */
        Surf *s = &surfs[i];
        if (s->depth || s->dirty_batch != batch_no || !recording || addr >= s->addr + surf_size(s) || s->addr >= addr + size) continue;
        /* the whole target in its own format, and not the target being drawn: sample its image (same row order) */
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s != cur_c) { seal_before_draw = 1; return s->im.view; }
        *full = 1; return NULL;   /* otherwise its pixels must reach guest memory first */
    }
    Tex *t = NULL;
    for (int i = 0; i < ntex; i++) if (texs[i].addr == addr && texs[i].w == w && texs[i].h == h && texs[i].fmt == fmt && texs[i].levels == levels) { t = &texs[i]; break; }
    int need = 0;
    if (!t) {
        if (ntex == cap_tex) { cap_tex = cap_tex ? cap_tex * 2 : 256; texs = realloc(texs, (size_t)cap_tex * sizeof *texs); }
        t = &texs[ntex++]; memset(t, 0, sizeof *t);
        t->addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->levels = levels;
        img_create_mip(&t->im, w, h, (uint32_t)levels, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        to_general(cb_up, t->im.img, VK_IMAGE_ASPECT_COLOR_BIT);
        t->hash = hash_mem(addr, size); t->checked_list = list_no; need = 1;
    } else if (t->checked_list != list_no) {
        t->checked_list = list_no;
        uint64_t hh = hash_mem(addr, size);
        if (hh != t->hash) {
            t->hash = hh; need = 1;
            if (t->used_batch == batch_no) {   /* already sampled in this batch: the new bytes go to a new image */
                img_retire(&t->im);
                img_create_mip(&t->im, w, h, (uint32_t)levels, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
                to_general(cb_up, t->im.img, VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }
    }
    if (need && tex_upload(t)) { t->checked_list = 0; t->hash = 0; *full = 1; return NULL; }   /* hash 0: re-upload after the flush */
    t->used_batch = t->last_batch = batch_no;
    return t->im.view;
}

/* ---------------- samplers / pipelines ---------------- */
static VkSampler samplers[1 << 16];
void pica_vk_quality(int resolution, int aniso) { q_res = resolution < 1 ? 1 : resolution > 4 ? 4 : resolution; q_aniso = aniso < 1 ? 1 : aniso; }
static VkSampler sampler_for(int unit) {
    static const int parr[3] = { 0x083, 0x093, 0x09B };
    uint32_t par = R(parr[unit]);
    static const int lodr[3] = { 0x084, 0x094, 0x09C };
    int ws = (int)((par >> 12) & 3), wt = (int)((par >> 8) & 3), mag = (int)((par >> 1) & 1), min = (int)((par >> 2) & 1), mip = (int)((par >> 24) & 1);
    static int nomip = -1; if (nomip < 0) nomip = getenv("PWR_VK_NOMIP") != NULL;
    int maxl = nomip ? 0 : (int)(R(lodr[unit]) >> 16 & 15);   /* tex_get clamps the image's level count; maxLod beyond it is harmless */
    /* border mode: filtering near the edge mixes in the sampler's border colour, so it must be the game's (0x081/0x091/0x099;
       the shadow projected onto the water uses white, and transparent black drew dotted dark lines along the projection's edges).
       Core Vulkan has three fixed border colours: the closest one here; the shader returns the exact colour outside [0,1] */
    uint32_t bcol = R(unit == 0 ? 0x081 : unit == 1 ? 0x091 : 0x099);
    int bord = (bcol >> 24) < 128 ? 0 : ((bcol & 255) + (bcol >> 8 & 255) + (bcol >> 16 & 255)) >= 384 ? 2 : 1;
    if (ws != 1 && wt != 1) bord = 0;
    /* anisotropic filtering: only for linearly filtered textures, capped at what the device allows */
    static float max_aniso = -1; if (max_aniso < 0) { VkPhysicalDeviceFeatures df; VkPhysicalDeviceProperties dp; vkGetPhysicalDeviceFeatures(phys, &df);
        vkGetPhysicalDeviceProperties(phys, &dp); max_aniso = df.samplerAnisotropy ? dp.limits.maxSamplerAnisotropy : 1; }
    int an = (min || mag) && q_aniso > 1 && max_aniso > 1 ? (q_aniso >= 16 ? 4 : q_aniso >= 8 ? 3 : q_aniso >= 4 ? 2 : 1) : 0;
    int key = ws | wt << 2 | mag << 4 | min << 5 | mip << 6 | maxl << 7 | bord << 11 | an << 13;
    if (!samplers[key]) {
        static const VkBorderColor bc[3] = { VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE };
        static const VkSamplerAddressMode am[4] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                                    VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT };
        VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .mipmapMode = mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = am[ws], .addressModeV = am[wt], .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = (float)maxl,
            .borderColor = bc[bord], .anisotropyEnable = an != 0, .maxAnisotropy = an ? fminf((float)(1 << an), max_aniso) : 1.0f };
        vkCreateSampler(dev, &ci, NULL, &samplers[key]);
    }
    return samplers[key];
}

/* vs: VkShaderModule. nd: the fragment shader without its depth write. sc: the fragment shader's specialisation constants
   (pica.frag SC_*: [0] on, then the 6 TEV stages' sources, operands, combiners, scales, 0x0E0, 0x104, 0x080, lighting, Z-buffer).
   Built with memset so the padding compares equal. */
#define NSC 30
typedef struct { uint64_t vs; uint32_t blend; uint8_t blend_on, cmask, has_depth, gpu_vs, nd, pad[3]; uint32_t sc[NSC]; } PKey;
/* Pipelines: specialised ones are compiled on a worker thread; until one is ready its draws use the generic pipeline for the same
   fixed-function state (sc all 0: the shader reads its configuration from the uniforms), queued ahead of the others when missing;
   while neither is ready the draw is skipped (an object missing for a few frames instead of the game stopping). Skipping is only
   for screen-sized targets (eyes, screens: redrawn every frame) and ones drawn in each of the last 8 frames (shadows, bloom);
   anything else (the profile list's Mii icon, faces, the signature) may be drawn once and kept, so its draws wait for a
   pipeline compiled on the spot. Compiled pipelines are kept in
   pwr_pipelines.bin, so this mostly happens the first time an effect appears. PWR_VK_SYNC_PIPES=1: compile every pipeline when
   first needed, as before. */
typedef struct { PKey k; VkPipeline p; atomic_int ready; VkPipeline sp; } Pipe;   /* sp: compiled on the spot while p is still queued */
#define MAX_PIPES 4096
static Pipe pipes[MAX_PIPES]; static int npipes;
static int pipe_hash[2 * MAX_PIPES];   /* open addressing: index + 1 into pipes, 0 = empty */
static VkPipelineCache pcache; static atomic_int pcache_dirty; static double pcache_saved;   /* pwr_pipelines.bin in the working directory */
static int pq[MAX_PIPES], pq_head, pq_tail; static pthread_mutex_t pq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t pq_cond = PTHREAD_COND_INITIALIZER;
static atomic_int pipes_pending;
int pica_vk_npipes(void) { return npipes; }
static uint32_t pkey_hash(const PKey *k) { const uint8_t *b = (const uint8_t *)k; uint32_t h = 2166136261u; for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 16777619u; return h; }
static VkBlendFactor bf(uint32_t f) {
    static const VkBlendFactor t[15] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE };
    return f < 15 ? t[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp bop(uint32_t e) {
    static const VkBlendOp t[5] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
    return e < 5 ? t[e] : VK_BLEND_OP_ADD;
}
static VkPipeline make_pipeline(const PKey *kp) {
    PKey k = *kp;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = (VkShaderModule)(uintptr_t)k.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k.nd ? fs_nd_mod : fs_mod, .pName = "main" } };
    VkSpecializationMapEntry me[NSC]; for (uint32_t i = 0; i < NSC; i++) me[i] = (VkSpecializationMapEntry){ i, i * 4, 4 };
    VkSpecializationInfo spi = { NSC, me, sizeof k.sc, k.sc };
    st[1].pSpecializationInfo = &spi;
    VkVertexInputBindingDescription vb = { 0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[6] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vtx, pos) }, { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vtx, col) },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vtx, uv) }, { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, uv[2]) },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vtx, quat) }, { 5, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, view) } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = k.gpu_vs ? 0 : 1, .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = k.gpu_vs ? 0 : 6, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = k.blend_on,
        .srcColorBlendFactor = bf(k.blend >> 16 & 15), .dstColorBlendFactor = bf(k.blend >> 20 & 15), .colorBlendOp = bop(k.blend & 7),
        .srcAlphaBlendFactor = bf(k.blend >> 24 & 15), .dstAlphaBlendFactor = bf(k.blend >> 28 & 15), .alphaBlendOp = bop(k.blend >> 8 & 7),
        .colorWriteMask = k.cmask };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf,
        .depthAttachmentFormat = k.has_depth ? depth_format : VK_FORMAT_UNDEFINED, .stencilAttachmentFormat = k.has_depth && has_stencil ? depth_format : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = playout };
    VkPipeline p = VK_NULL_HANDLE; double t0 = now_ms();
    if (vkCreateGraphicsPipelines(dev, pcache, 1, &ci, NULL, &p)) fprintf(stderr, "pica_vk: pipeline creation failed\n");
    static int ptrace = -1; if (ptrace < 0) ptrace = getenv("PWR_VK_PIPE_TRACE") != NULL;   /* each new pipeline's key, to spot keys that change every frame */
    if (ptrace) { fprintf(stderr, "vk: pipeline (%.0f ms) vs %llx blend %x/%u cmask %x depth %u nd %u sc", now_ms() - t0, (unsigned long long)k.vs, k.blend, k.blend_on, k.cmask, k.has_depth, k.nd);
                  for (int i = 0; i < NSC; i++) fprintf(stderr, " %x", k.sc[i]); fputc('\n', stderr); }
    atomic_store(&pcache_dirty, 1);
    return p;
}

static void *pipe_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pq_lock);
        while (pq_head == pq_tail) pthread_cond_wait(&pq_cond, &pq_lock);
        int i = pq[pq_head]; pq_head = (pq_head + 1) % MAX_PIPES;
        pthread_mutex_unlock(&pq_lock);
        pipes[i].p = make_pipeline(&pipes[i].k);
        atomic_store_explicit(&pipes[i].ready, 1, memory_order_release);
        atomic_fetch_sub(&pipes_pending, 1);
    }
    return NULL;
}

/* the pipe for k, created now (sync) or queued for the worker (urgent: ahead of the others); NULL when the table is full */
static Pipe *pipe_get(const PKey *k, int sync, int urgent) {
    uint32_t hs = pkey_hash(k) & (2 * MAX_PIPES - 1), j;
    for (j = hs; pipe_hash[j]; j = (j + 1) & (2 * MAX_PIPES - 1)) if (!memcmp(&pipes[pipe_hash[j] - 1].k, k, sizeof *k)) return &pipes[pipe_hash[j] - 1];
    if (npipes >= MAX_PIPES) return NULL;
    Pipe *e = &pipes[npipes]; e->k = *k; e->p = e->sp = VK_NULL_HANDLE; atomic_store(&e->ready, 0);
    pipe_hash[j] = ++npipes;
    if (sync) {
        double t0 = now_ms(); e->p = make_pipeline(k); bs.pipe_ms += now_ms() - t0; bs.pipes++;
        atomic_store_explicit(&e->ready, 1, memory_order_release);
        if (getenv("PWR_VK_PIPE_TRACE")) fprintf(stderr, "vk: pipeline compiled on the spot for target %08x\n", cur_c ? cur_c->addr : 0);
    } else {
        static pthread_t th; static int started;
        if (!started) { started = 1; pthread_create(&th, NULL, pipe_worker, NULL); pthread_detach(th); }
        atomic_fetch_add(&pipes_pending, 1);
        pthread_mutex_lock(&pq_lock);
        if (urgent) { pq_head = (pq_head + MAX_PIPES - 1) % MAX_PIPES; pq[pq_head] = (int)(e - pipes); }
        else { pq[pq_tail] = (int)(e - pipes); pq_tail = (pq_tail + 1) % MAX_PIPES; }
        pthread_cond_signal(&pq_cond); pthread_mutex_unlock(&pq_lock);
    }
    return e;
}

/* must: the draw cannot be skipped */
static VkPipeline pipeline_for(PKey k, int must) {
    static int sync_all = -1; if (sync_all < 0) sync_all = getenv("PWR_VK_SYNC_PIPES") != NULL;
    Pipe *e = pipe_get(&k, sync_all || must, 0);
    if (e && atomic_load_explicit(&e->ready, memory_order_acquire)) return e->p;
    if (e && must) {
        if (!e->sp) {
            double t0 = now_ms(); e->sp = make_pipeline(&k); bs.pipe_ms += now_ms() - t0; bs.pipes++;
            if (getenv("PWR_VK_PIPE_TRACE")) fprintf(stderr, "vk: pipeline compiled on the spot for target %08x\n", cur_c ? cur_c->addr : 0);
        }
        return e->sp;
    }
    if (k.sc[0] == 0) return VK_NULL_HANDLE;   /* a generic pipeline that is still compiling */
    PKey g = k; memset(g.sc, 0, sizeof g.sc);   /* not ready yet: the generic pipeline for the same fixed-function state */
    Pipe *f = pipe_get(&g, 0, 1);
    return f && atomic_load_explicit(&f->ready, memory_order_acquire) ? f->p : VK_NULL_HANDLE;   /* neither: the draw is skipped */
}

/* ---------------- init ---------------- */
static int init(void) {
    if (video_vk(&phys, &dev, &queue, &qfam)) return -1;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT; has_stencil = 1; }
    else {
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D32_SFLOAT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return -1;
        depth_format = VK_FORMAT_D32_SFLOAT; fprintf(stderr, "pica_vk: no D32_SFLOAT_S8_UINT: stencil test unavailable\n");
    }
    ds_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!push_desc) return -1;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    if (vkCreateCommandPool(dev, &pci, NULL, &cpool)) return -1;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 * MAX_PARTS };
    if (vkAllocateCommandBuffers(dev, &cai, &cb_part[0][0])) return -1;
    cb_up = cb_part[0][0]; cb_draw = cb_part[0][1];
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(dev, &fi, NULL, &fence) || vkCreateFence(dev, &fi, NULL, &scan_fence)) return -1;
    VkCommandBufferAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (vkAllocateCommandBuffers(dev, &sai, &cb_scan)) return -1;
    VkDescriptorSetLayoutBinding b[8] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 8, .pBindings = b };
    if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl)) return -1;
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    if (vkCreatePipelineLayout(dev, &lci, NULL, &playout)) return -1;
    VkShaderModuleCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vert_spv, .pCode = vert_spv };
    if (vkCreateShaderModule(dev, &sci, NULL, &vs_mod)) return -1;
    sci.codeSize = sizeof vs_spv; sci.pCode = vs_spv;
    if (vkCreateShaderModule(dev, &sci, NULL, &vsi_mod)) return -1;
    sci.codeSize = sizeof frag_spv; sci.pCode = frag_spv;
    if (vkCreateShaderModule(dev, &sci, NULL, &fs_mod)) return -1;
    sci.codeSize = sizeof frag_nd_spv; sci.pCode = frag_nd_spv;
    if (vkCreateShaderModule(dev, &sci, NULL, &fs_nd_mod)) return -1;
    {   /* pipelines compiled in earlier runs (the driver checks that the data fits this GPU and driver) */
        size_t n = 0; void *data = NULL; FILE *f = fopen("pwr_pipelines.bin", "rb");
        if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); if (sz > 0 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz; fclose(f); }
        VkPipelineCacheCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n, .pInitialData = n ? data : NULL };
        if (vkCreatePipelineCache(dev, &pci, NULL, &pcache)) { pci.initialDataSize = 0; pci.pInitialData = NULL; vkCreatePipelineCache(dev, &pci, NULL, &pcache); }
        free(data);
    }
    if (ring_init(&vbuf, 64u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0) || ring_init(&ibuf, 16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0) ||
        ring_init(&ubuf, 16u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0) || ring_init(&lbuf, 4u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        ring_init(&upbuf, 64u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) || ring_init(&dlbuf, 16u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1) ||
        ring_init(&abuf, 64u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) || ring_init(&pbuf, 4u << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) return -1;
    gpu_vs = getenv("PWR_VK_CPU_VS") == NULL;
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp); ts_period = pp.limits.timestampPeriod;
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    if (!pp.limits.timestampComputeAndGraphics || vkCreateQueryPool(dev, &qi, NULL, &qpool)) qpool = VK_NULL_HANDLE;
    /* 1x1 white texture for disabled units */
    begin_batch();
    img_create(&white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb_up, white.img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDeviceSize off = ring_alloc(&upbuf, 4, 4); memset(upbuf.ptr + off, 255, 4);
    VkBufferImageCopy cp = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 1, 1, 1 } };
    vkCmdCopyBufferToImage(cb_up, upbuf.buf, white.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    pica_vk_flush();
    fprintf(stderr, "pica_vk: hardware renderer on\n");
    return 0;
}

int pica_vk_gpu_vs(void) { return gpu_vs; }

int pica_vk_enabled(void) {
    if (state < 0) {
        const char *e = getenv("PWR_RENDERER");
        vtrace = getenv("PWR_VK_TRACE") != NULL;
        state = (e && !strcmp(e, "sw")) ? 0 : init() ? 0 : 1;
        if (!state && !(e && !strcmp(e, "sw"))) fprintf(stderr, "pica_vk: unavailable, using the software renderer\n");
    }
    return state;
}

/* ---------------- passes ---------------- */
/* ---------------- deferred draws ---------------- */
typedef struct {
    Surf *c, *d; VkPipeline p; VkViewport vp; VkRect2D sc; float bc[4]; VkCullModeFlags cull; VkCompareOp dop;
    uint8_t dt, dw, gpu, ste, sref, scmask, swmask; VkCompareOp sfn; VkStencilOp sop[3];   /* sop: fail, depth fail, pass */
    VkDescriptorBufferInfo b[5]; VkDescriptorImageInfo ii[3]; VkDeviceSize voff, ioff; uint32_t nidx;
} DrawRec;
static DrawRec *recs; static uint32_t nrec, caprec, seg = 1;

static void replay(const DrawRec *r, VkPipeline *bound) {
    /* GPU-cost experiments: PWR_VK_DBG=scissor (1x1 scissor: no fragment work), nodraw (state and descriptors only), half (every second draw) */
    static int dbg = -1; static unsigned dn; if (dbg < 0) { const char *e = getenv("PWR_VK_DBG"); dbg = !e ? 0 : !strcmp(e, "scissor") ? 1 : !strcmp(e, "nodraw") ? 2 : !strcmp(e, "half") ? 3 : 0; }
    if (dbg == 3 && (dn++ & 1)) return;
    VkRect2D one = { { 0, 0 }, { 1, 1 } };
    if (r->p != *bound) { vkCmdBindPipeline(cb_draw, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p); *bound = r->p; }
    /* dynamic state: only what changed since the previous draw of this command buffer (st_valid is cleared when a command buffer begins) */
    static VkViewport lvp; static VkRect2D lsc; static float lbc[4]; static VkCullModeFlags lcull; static VkCompareOp ldop; static uint8_t ldt, ldw;
    if (!st_valid || memcmp(&lvp, &r->vp, sizeof lvp)) { vkCmdSetViewport(cb_draw, 0, 1, &r->vp); lvp = r->vp; }
    if (!st_valid || memcmp(&lsc, &r->sc, sizeof lsc)) { vkCmdSetScissor(cb_draw, 0, 1, dbg == 1 ? &one : &r->sc); lsc = r->sc; }
    if (!st_valid || memcmp(lbc, r->bc, sizeof lbc)) { vkCmdSetBlendConstants(cb_draw, r->bc); memcpy(lbc, r->bc, sizeof lbc); }
    if (!st_valid) vkCmdSetFrontFace(cb_draw, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    if (!st_valid || lcull != r->cull) { vkCmdSetCullMode(cb_draw, r->cull); lcull = r->cull; }
    if (!st_valid || ldt != r->dt) { vkCmdSetDepthTestEnable(cb_draw, r->dt); ldt = r->dt; }
    if (!st_valid || ldw != r->dw) { vkCmdSetDepthWriteEnable(cb_draw, r->dw); ldw = r->dw; }
    if (!st_valid || ldop != r->dop) { vkCmdSetDepthCompareOp(cb_draw, r->dop); ldop = r->dop; }
    static uint8_t lste, lsref, lscm, lswm; static VkCompareOp lsfn; static VkStencilOp lsop[3];
    if (!st_valid || lste != r->ste) { vkCmdSetStencilTestEnable(cb_draw, r->ste); lste = r->ste; }
    if (r->ste || !st_valid) {
        if (!st_valid || lsfn != r->sfn || memcmp(lsop, r->sop, sizeof lsop)) {
            vkCmdSetStencilOp(cb_draw, VK_STENCIL_FACE_FRONT_AND_BACK, r->sop[0], r->sop[2], r->sop[1], r->sfn); lsfn = r->sfn; memcpy(lsop, r->sop, sizeof lsop); }
        if (!st_valid || lsref != r->sref) { vkCmdSetStencilReference(cb_draw, VK_STENCIL_FACE_FRONT_AND_BACK, r->sref); lsref = r->sref; }
        if (!st_valid || lscm != r->scmask) { vkCmdSetStencilCompareMask(cb_draw, VK_STENCIL_FACE_FRONT_AND_BACK, r->scmask); lscm = r->scmask; }
        if (!st_valid || lswm != r->swmask) { vkCmdSetStencilWriteMask(cb_draw, VK_STENCIL_FACE_FRONT_AND_BACK, r->swmask); lswm = r->swmask; }
    }
    st_valid = 1;
    VkWriteDescriptorSet w[8];
    static const uint32_t bind[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    static const VkDescriptorType ty[8] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    for (int i = 0; i < 8; i++) {
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = bind[i], .descriptorCount = 1, .descriptorType = ty[i] };
        if (i < 2) w[i].pBufferInfo = &r->b[i]; else if (i < 5) w[i].pImageInfo = &r->ii[i - 2]; else w[i].pBufferInfo = &r->b[i - 3];
    }
    push_desc(cb_draw, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, r->gpu ? 8 : 5, w);
    if (!r->gpu) vkCmdBindVertexBuffers(cb_draw, 0, 1, &vbuf.buf, &r->voff);
    vkCmdBindIndexBuffer(cb_draw, ibuf.buf, r->ioff, VK_INDEX_TYPE_UINT32);
    if (dbg != 2) vkCmdDrawIndexed(cb_draw, r->nidx, 1, 0, 0, 0);
}

static void begin_rendering(Surf *c, Surf *d) {
    /* an earlier pass may have written these attachments, or a target this pass samples */
    barrier_all(cb_draw, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = c->im.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d ? d->im.view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { c->w * (uint32_t)cur_scale, c->h * (uint32_t)cur_scale } }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = d ? &da : NULL, .pStencilAttachment = d && has_stencil ? &da : NULL };
    vkCmdBeginRendering(cb_draw, &ri);
    g_vk_passes++;
    if (vtrace) fprintf(stderr, "vk: pass %08x %ux%u fmt%d depth %08x\n", c->addr, c->w, c->h, c->fmt, d ? d->addr : 0);
}

/* emit the open segment's draws grouped by colour target (first-use order), one pass per (colour, depth) run */
static void seal(void) {
    static uint8_t *done; static uint32_t cap_done;
    if (!nrec) return;
    if (cap_done < nrec) { cap_done = caprec; done = realloc(done, cap_done); }
    memset(done, 0, nrec);
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < nrec; i++) {
        if (done[i]) continue;
        Surf *c = recs[i].c, *d = NULL; int open = 0;
        for (uint32_t j = i; j < nrec; j++) {
            if (done[j] || recs[j].c != c) continue;
            if (!open || recs[j].d != d) {
                if (open) vkCmdEndRendering(cb_draw);
                d = recs[j].d; begin_rendering(c, d); open = 1; bound = VK_NULL_HANDLE;
            }
            replay(&recs[j], &bound); done[j] = 1;
        }
        if (open) vkCmdEndRendering(cb_draw);
    }
    nrec = 0; seg++;
}

/* uploads recorded in cb_up must be visible to every later use */
static void end_uploads(void) {
    barrier_all(cb_up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

/* Submit the draws recorded so far without waiting and go on recording the same batch in the next part's command
   buffers, so the GPU works while the CPU prepares the rest of the command list (a frame's list took ~12 ms of CPU
   and ~8 ms of GPU one after the other, more than a 60 Hz frame). Ring space is not reused within a batch, and the
   next part starts with a full barrier, so it runs after this one (uploads included). The batch's fence and
   write-backs stay at its end (pica_vk_flush). PWR_VK_KICK=<draws> per part (default 200, 0 = one part per batch). */
static uint32_t kick_draws;
static void kick(void) {
    static int every = -1; if (every < 0) { const char *e = getenv("PWR_VK_KICK"); every = e ? atoi(e) : 200; }
    if (!every || !recording || ++kick_draws < (uint32_t)every || part + 1 >= MAX_PARTS) return;
    kick_draws = 0;
    seal();
    end_uploads();
    vkEndCommandBuffer(cb_up); vkEndCommandBuffer(cb_draw);
    VkCommandBuffer cbs[2] = { cb_up, cb_draw };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2, .pCommandBuffers = cbs };
    video_queue_lock(); VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); video_queue_unlock();
    if (r) fprintf(stderr, "pica_vk: submit failed (%d)\n", r);
    part++; cb_up = cb_part[part][0]; cb_draw = cb_part[part][1];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cb_up, 0); vkBeginCommandBuffer(cb_up, &bi);
    vkResetCommandBuffer(cb_draw, 0); vkBeginCommandBuffer(cb_draw, &bi);
    barrier_all(cb_up, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    st_valid = 0;
}

void pica_vk_flush(void) {
    if (!recording) return;
    kick_draws = 0;
    seal();
    barrier_all(cb_draw, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        s->dl_off = UINT64_MAX;
        if (s->depth || s->dirty_batch != batch_no) continue;
        s->dl_off = ring_alloc(&dlbuf, (VkDeviceSize)s->w * s->h * 4, 16);
        if (s->dl_off == UINT64_MAX) { fprintf(stderr, "pica_vk: download ring full, target %08x not written back\n", s->addr); continue; }
        VkImage from = s->im.img;
        if (cur_scale > 1 && s->down.img) {   /* higher internal resolution: filter down to the guest size first */
            int S = cur_scale;
            VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int)s->w * S, (int)s->h * S, 1 } },
                              .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { (int)s->w, (int)s->h, 1 } } };
            vkCmdBlitImage(cb_draw, s->im.img, VK_IMAGE_LAYOUT_GENERAL, s->down.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            barrier_all(cb_draw, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            from = s->down.img;
        }
        VkBufferImageCopy cp = { .bufferOffset = s->dl_off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { s->w, s->h, 1 } };
        vkCmdCopyImageToBuffer(cb_draw, from, VK_IMAGE_LAYOUT_GENERAL, dlbuf.buf, 1, &cp);
    }
    barrier_all(cb_draw, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    end_uploads();
    if (qpool) vkCmdWriteTimestamp(cb_draw, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    vkEndCommandBuffer(cb_up); vkEndCommandBuffer(cb_draw);
    VkCommandBuffer cbs[2] = { cb_up, cb_draw };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2, .pCommandBuffers = cbs };
    g_vk_batches++;
    video_queue_lock(); VkResult r = vkQueueSubmit(queue, 1, &si, fence); video_queue_unlock();
    if (r) fprintf(stderr, "pica_vk: submit failed (%d)\n", r);
    else { struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0); vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fence);
           clock_gettime(CLOCK_MONOTONIC, &w1); g_vk_wait_us += (unsigned long long)((w1.tv_sec - w0.tv_sec) * 1000000LL + (w1.tv_nsec - w0.tv_nsec) / 1000); }
    recording = 0; list_no++;
    double gpu_ms = 0, t_wb = now_ms();
    if (qpool && !r) { uint64_t ts[2]; if (!vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT)) { gpu_ms = (double)(ts[1] - ts[0]) * ts_period / 1e6; g_vk_gpu_us += (unsigned long long)(gpu_ms * 1000.0); } }
    for (int i = 0; i < nsurf; i++) if (!r && surfs[i].dl_off != UINT64_MAX) surf_writeback(&surfs[i], surfs[i].dl_off);
    {   /* a list that took 50 ms or more from its first draw to here: where the time went (at most 100 reports) */
        double end = now_ms(), total = end - bs.t0; static int nslow;
        if (total >= 50.0 && nslow++ < 100)
            fprintf(stderr, "pica_vk: slow list %.0f ms: new pipelines %u (%.0f ms), texture uploads %u (%.0f ms), target uploads %u (%.0f ms), "
                    "write-back %.0f ms, gpu %.0f ms (first part to fence), draws %llu, passes %llu\n", total, bs.pipes, bs.pipe_ms, bs.texs, bs.tex_ms,
                    bs.surfs, bs.surf_ms, end - t_wb, gpu_ms, g_vk_draws - bs.draws0, g_vk_passes - bs.passes0);
    }
    for (int i = 0; i < ndead; i++) { vkDestroyImageView(dev, dead[i].view, NULL); vkDestroyImage(dev, dead[i].img, NULL); vkFreeMemory(dev, dead[i].mem, NULL); }
    ndead = 0;
    cur_c = cur_d = NULL;
    if (atomic_load(&pcache_dirty)) {   /* new pipelines: keep them for the next run, at most every 5 s */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
        if (now - pcache_saved >= 5.0) {
            size_t n = 0; pcache_saved = now; atomic_store(&pcache_dirty, 0);
            if (!vkGetPipelineCacheData(dev, pcache, &n, NULL) && n) {
                void *data = malloc(n); FILE *f;
                if (data && !vkGetPipelineCacheData(dev, pcache, &n, data) && (f = fopen("pwr_pipelines.bin.tmp", "wb"))) {
                    int ok = fwrite(data, 1, n, f) == n; fclose(f);
                    extern int rt_replace_file(const char *tmp, const char *dst);
                    if (ok) rt_replace_file("pwr_pipelines.bin.tmp", "pwr_pipelines.bin"); else remove("pwr_pipelines.bin.tmp");
                }
                free(data);
            }
        }
    }
}

/* internal resolution changed (between batches): drop every target; each is re-created at the new size and re-uploaded
   from guest memory on its next use (colour targets are written back after every batch, depth/stencil start over) */
static void scanouts_drop(void);
static void rescale(void) {
    video_queue_lock(); vkQueueWaitIdle(queue); video_queue_unlock();
    for (int i = 0; i < nsurf; i++) { img_retire(&surfs[i].im); if (surfs[i].down.img) img_retire(&surfs[i].down); }
    for (int i = 0; i < ndead; i++) { vkDestroyImageView(dev, dead[i].view, NULL); vkDestroyImage(dev, dead[i].img, NULL); vkFreeMemory(dev, dead[i].mem, NULL); }
    ndead = 0; nsurf = 0; cur_c = cur_d = NULL;
    scanouts_drop();
    cur_scale = q_res;
    fprintf(stderr, "pica_vk: 3D resolution %dx\n", cur_scale);
}

/* ---------------- scan-out at the internal resolution ----------------
 * The game copies its display targets into the LCD framebuffers with DisplayTransfer (gsp.c), which reads guest memory:
 * the guest-size result. When the internal resolution is above 1x, the transfer also copies the target's image into an
 * image kept for that framebuffer address, and video.c presents that image instead of the framebuffer bytes as long as
 * no other GX operation wrote the framebuffer since. The copy has its own small submission (the batch that drew the
 * target has finished: DisplayTransfer runs after the command list); video.c's submission comes later on the same queue. */
#define MAX_SCAN 8
static struct { uint32_t addr, end, w, h; int valid; Img im; } scans[MAX_SCAN];
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static void scanouts_drop(void) {   /* rescale; video.c may have submitted a use of these after the queue went idle: wait again under the lock */
    pthread_mutex_lock(&scan_lock);
    video_queue_lock(); vkQueueWaitIdle(queue); video_queue_unlock();
    for (int i = 0; i < MAX_SCAN; i++) { if (scans[i].im.img) { vkDestroyImageView(dev, scans[i].im.view, NULL); vkDestroyImage(dev, scans[i].im.img, NULL); vkFreeMemory(dev, scans[i].im.mem, NULL); } memset(&scans[i], 0, sizeof scans[i]); }
    pthread_mutex_unlock(&scan_lock);
}
void pica_vk_scanout_invalidate(uint32_t start, uint32_t end) {   /* a GX operation wrote guest bytes in [start, end) */
    pthread_mutex_lock(&scan_lock);
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].valid && start < scans[i].end && scans[i].addr < end) scans[i].valid = 0;
    pthread_mutex_unlock(&scan_lock);
}
void pica_vk_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    uint32_t iw = in_dim & 0xffff, ih = in_dim >> 16, ow = out_dim & 0xffff, oh = out_dim >> 16;
    static const uint32_t obpp[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };
    /* exactly the bytes written: the game's two framebuffers sit back to back, and a larger range (4 bytes per RGB8 pixel
       before) invalidated the one on screen, which then showed at guest resolution for a frame (a visible pop at 2x/3x) */
    pica_vk_scanout_invalidate(out, out + ow * oh * obpp[(flags >> 12) & 7]);
    if (state != 1 || cur_scale == 1 || recording) return;
    /* the per-frame scan-out copy only: tiled RGBA8 -> linear RGB8, same size, no flip/scale (see gsp.c display_transfer) */
    if (((flags >> 8) & 7) != 0 || ((flags >> 12) & 7) != 1 || (flags & 0x0300000Bu) || iw != ow || ih != oh) return;
    Surf *src = NULL;
    for (int i = 0; i < nsurf; i++) if (!surfs[i].depth && surfs[i].addr == in && surfs[i].w == iw && surfs[i].h == ih && surfs[i].fmt == 0 && surfs[i].synced) { src = &surfs[i]; break; }
    if (!src) return;
    pthread_mutex_lock(&scan_lock);
    int k = -1;
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].addr == out && scans[i].w == iw && scans[i].h == ih) { k = i; break; }
    for (int i = 0; k < 0 && i < MAX_SCAN; i++) if (!scans[i].im.img) k = i;
    if (k < 0) { pthread_mutex_unlock(&scan_lock); return; }
    uint32_t W = iw * (uint32_t)cur_scale, H = ih * (uint32_t)cur_scale;
    if (!scans[k].im.img) {
        if (img_create(&scans[k].im, W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) { memset(&scans[k], 0, sizeof scans[k]); pthread_mutex_unlock(&scan_lock); return; }
        scans[k].addr = out; scans[k].w = iw; scans[k].h = ih; scans[k].end = out + ow * oh * 3;
    }
    if (scan_used) { vkWaitForFences(dev, 1, &scan_fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &scan_fence); }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cb_scan, 0); vkBeginCommandBuffer(cb_scan, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,   /* fully overwritten */
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = scans[k].im.img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb_scan, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);   /* after earlier reads of it (video.c) */
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { W, H, 1 } };
    vkCmdCopyImage(cb_scan, src->im.img, VK_IMAGE_LAYOUT_GENERAL, scans[k].im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    barrier_all(cb_scan, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    vkEndCommandBuffer(cb_scan);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb_scan };
    video_queue_lock(); VkResult r = vkQueueSubmit(queue, 1, &si, scan_fence); video_queue_unlock();
    scan_used = r == VK_SUCCESS;
    scans[k].valid = r == VK_SUCCESS;
    pthread_mutex_unlock(&scan_lock);
}
/* video.c, between pica_vk_scan_lock(1) and (0) (held until its submission, so a rescale cannot free the image first):
   the image for an LCD framebuffer address, stored like the framebuffer (w = 240 lines, h = screen width), layout GENERAL */
void pica_vk_scan_lock(int on) { if (on) pthread_mutex_lock(&scan_lock); else pthread_mutex_unlock(&scan_lock); }
int pica_vk_scanout(uint32_t addr, VkImageView *view, uint32_t *w, uint32_t *h) {
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].valid && scans[i].addr == addr) { *view = scans[i].im.view; *w = scans[i].w * (uint32_t)cur_scale; *h = scans[i].h * (uint32_t)cur_scale; return 1; }
    return 0;
}

/* guest bytes -> image when they changed since this target was last seen, or a pending MemoryFill clear (in cb_up) */
static void surf_sync(Surf *t) {
    if (t->checked_list == list_no) return;   /* guest bytes only change between command lists (or by our own write-back) */
    t->checked_list = list_no;
    if (!t->synced || hash_mem(t->addr, surf_size(t)) != t->hash) { surf_upload(t); t->pending_clear = 0; }
    if (t->pending_clear) {
        t->pending_clear = 0;
        if (t->depth) {
            VkClearDepthStencilValue dv; memcpy(&dv.depth, t->clear_px, 4); dv.stencil = t->clear_st;
            VkImageSubresourceRange rg = { ds_aspect, 0, 1, 0, 1 };
            vkCmdClearDepthStencilImage(cb_up, t->im.img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rg);
        } else {
            VkClearColorValue cv = { .float32 = { t->clear_px[0] / 255.f, t->clear_px[1] / 255.f, t->clear_px[2] / 255.f, t->clear_px[3] / 255.f } };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb_up, t->im.img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg);
        }
    }
}

/* A depth buffer that lies inside a larger depth target: the game measures how much of the sun is visible by pointing an
   8x8 depth buffer at the sun's tile of the scene's depth buffer and drawing a quad with a depth test. Depth targets are
   never written back to guest memory, so the window's pixels are copied from the larger target's image (the most recently
   bound one containing it), after the draws recorded so far. Returns the window surface, or NULL when there is no such
   target or the window is not made of whole tiles. Depth written through the window is not copied back. */
static uint32_t use_seq;
static Surf *depth_window(uint32_t daddr, uint32_t w, uint32_t h, int dfmt) {
    static const uint32_t db[4] = { 2, 3, 3, 4 };
    uint32_t bpp = db[dfmt & 3], tile = 64 * bpp, size = w * h * bpp;
    if ((w & 7) || (h & 7)) return NULL;
    Surf *P = NULL, *W = NULL;
    for (int pass = 0; pass < 2; pass++) {
        P = NULL;
        for (int i = 0; i < nsurf; i++) {
            Surf *s = &surfs[i];
            if (!s->depth || s->sub || s->fmt != dfmt || (s->w <= w && s->h <= h) || daddr <= s->addr || daddr + size > s->addr + surf_size(s)) continue;
            if ((daddr - s->addr) % tile || (s->w & 7)) continue;
            if (!P || s->use_seq > P->use_seq) P = s;
        }
        if (!P || W) break;
        /* one window surface per size, moved to each new address (the sun's tile changes as it moves); creating it can
           evict a surface, so P is looked up again */
        for (int i = 0; i < nsurf && !W; i++) if (surfs[i].sub && surfs[i].w == w && surfs[i].h == h && surfs[i].fmt == dfmt) W = &surfs[i];
        if (W) break;
        W = surf_get(daddr, w, h, dfmt, 1); if (!W) return NULL;
        W->sub = 1;
    }
    if (!P) return NULL;
    W->addr = daddr; W->checked_list = list_no; W->synced = 1; W->pending_clear = 0;
    surf_sync(P);   /* P's own pending clear or guest upload goes first (cb_up) */
    seal();         /* the draws recorded so far (P's included) reach cb_draw before the copy */
    barrier_all(cb_draw, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy rg[64]; uint32_t n = 0, S = (uint32_t)cur_scale, tw = w / 8, ptw = P->w / 8, t0 = (daddr - P->addr) / tile;
    for (uint32_t k = 0; k < h / 8; k++) for (uint32_t j = 0; j < tw;) {   /* tile row k of the window, split where it wraps in P */
        uint32_t T = t0 + k * tw + j, col = T % ptw, row = T / ptw, run = tw - j < ptw - col ? tw - j : ptw - col;
        if (n == 64) break;
        rg[n++] = (VkImageCopy){ .srcSubresource = { ds_aspect, 0, 0, 1 }, .srcOffset = { (int)(col * 8 * S), (int)((P->h - 8 * (row + 1)) * S), 0 },
                                 .dstSubresource = { ds_aspect, 0, 0, 1 }, .dstOffset = { (int)(j * 8 * S), (int)((h - 8 * (k + 1)) * S), 0 },
                                 .extent = { run * 8 * S, 8 * S, 1 } };
        j += run;
    }
    vkCmdCopyImage(cb_draw, P->im.img, VK_IMAGE_LAYOUT_GENERAL, W->im.img, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier_all(cb_draw, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    return W;
}

/* make (colour, depth) from the framebuffer registers the active pass; 0 on success */
static int ensure_pass(void) {
    uint32_t w = R(0x11E) & 0x7ff, h = ((R(0x11E) >> 12) & 0x3ff) + 1;
    uint32_t caddr = pa_to_va((R(0x11D) & 0x0fffffff) << 3), daddr = pa_to_va((R(0x11C) & 0x0fffffff) << 3);
    int cfmt = (int)((R(0x117) >> 16) & 7), dfmt = (int)(R(0x116) & 3);
    if (cfmt > 4) cfmt = 0;
    if (!caddr || !w || !h) return -1;
    if (recording && cur_c && cur_c->addr == caddr && cur_c->w == w && cur_c->h == h && cur_c->fmt == cfmt &&
        ((!daddr && !cur_d) || (cur_d && cur_d->addr == daddr && cur_d->fmt == dfmt))) return 0;
    if (!recording && q_res != cur_scale) rescale();
    begin_batch();
    cur_c = surf_get(caddr, w, h, cfmt, 0);
    static int nowin = -1; if (nowin < 0) nowin = getenv("PWR_VK_NO_DEPTH_WINDOW") != NULL;   /* A/B: depth windows read guest memory */
    cur_d = !daddr ? NULL : (!nowin ? depth_window(daddr, w, h, dfmt) : NULL);
    if (daddr && !cur_d) cur_d = surf_get(daddr, w, h, dfmt, 1);
    if (!cur_c || (daddr && !cur_d)) { cur_c = cur_d = NULL; return -1; }
    cur_c->last_batch = batch_no; cur_c->use_seq = ++use_seq;
    if (cur_d) { cur_d->last_batch = batch_no; cur_d->use_seq = ++use_seq; }
    surf_sync(cur_c);
    if (cur_d) surf_sync(cur_d);
    return 0;
}

/* ---------------- draws ---------------- */
#define UBO_SIZE 2304   /* 104 + 25 vec4, rounded up to the 256-byte offset alignment */

#define VS_UBO_SIZE 2048   /* 96 + 5 + 12 + 12 vec4, rounded up to 256 */
void pica_vk_draw(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                  const uint32_t *slots, uint32_t n, int topo) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ensure_pass()) return;
        /* indices: lists, strips (alternate winding like pica_sw.c) and fans -> triangle list over the distinct vertices */
        static uint32_t remap[65536], tri_idx[3 * 65536];
        uint32_t nidx = 0;
        #define PUT(a, b, c) (tri_idx[nidx++] = (a), tri_idx[nidx++] = (b), tri_idx[nidx++] = (c))
        if (topo == 1) { for (uint32_t i = 2; i < n; i++) { if (i & 1) PUT(i - 1, i - 2, i); else PUT(i - 2, i - 1, i); } }
        else if (topo == 2) { for (uint32_t i = 2; i < n; i++) PUT(0, i - 1, i); }
        else { for (uint32_t i = 0; i + 2 < n; i += 3) PUT(i, i + 1, i + 2); }
        #undef PUT
        if (!nidx) return;
        for (uint32_t i = 0; i < nvlist; i++) remap[vlist_slot[i]] = i;
        int full = 0, gpu = vcache == NULL;
        uint32_t nattr = (uint32_t)pica_num_attrs();
        VkDeviceSize voff = 0, aoff = 0, vsoff = 0;
        /* uniforms are built in host memory first; a draw whose uniforms equal the previous draw's in this batch reuses
           its copy (most consecutive draws only change vertex data or textures) */
        static uint8_t vsu[VS_UBO_SIZE], fsu[UBO_SIZE], last_vsu[VS_UBO_SIZE], last_fsu[UBO_SIZE];
        static VkDeviceSize last_vsoff, last_uoff; static uint32_t last_ubatch;
        VkShaderModule vsm = vs_mod;
        if (gpu) { vsm = pica_vsgen_module(dev); if (!vsm) vsm = vsi_mod; }
        /* Attributes for the GPU vertex shader. Translated shaders read the game's vertex bytes themselves (fetch() in
           pica_vsgen.c): each loader's used range of vertices is copied as is, about 5x less work than converting every
           vertex to vec4 on the CPU. The CPU conversion stays for immediate mode, the interpreter shader, unaligned data
           and sparse index ranges; its vec4 layout is described to fetch() the same way (f32 x4, stride nattr * 16). */
        uint32_t *fa = (uint32_t *)(vsu + 101 * 16); float *fdef = (float *)(vsu + 113 * 16);
        struct { uint32_t src, len, roff, used; } ld[12];
        int raw = 0; uint32_t imin = 0; VkDeviceSize asize = (VkDeviceSize)nvlist * nattr * 16;
        static int noraw = -1; if (noraw < 0) noraw = getenv("PWR_VK_CPU_FETCH") != NULL;   /* A/B switch */
        if (gpu && vsm != vsi_mod && !noraw) {
            FetchOp op[48]; V4 def[16];
            int nop = pica_fetch_ops(op, def), ok = nop >= 0 && nattr <= 12;
            uint32_t seen = 0, imax = 0; imin = UINT32_MAX;
            for (uint32_t i = 0; ok && i < nvlist; i++) { if (vlist_index[i] < imin) imin = vlist_index[i]; if (vlist_index[i] > imax) imax = vlist_index[i]; }
            if (ok && nvlist && imax - imin + 1 > 4 * nvlist + 64) ok = 0;   /* sparse: most of the range unused */
            memset(ld, 0, sizeof ld);
            for (int o = 0; ok && o < nop; o++) {
                uint32_t sz = op[o].type < 2 ? 1 : op[o].type == 2 ? 2 : 4, al = sz == 4 ? 3 : sz == 2 ? 1 : 0;
                if ((seen >> op[o].dst & 1) || (op[o].addr & al) || (op[o].stride & al)) { ok = 0; break; }
                seen |= 1u << op[o].dst;
                uint32_t lo = op[o].addr + imin * op[o].stride, hi = op[o].addr + imax * op[o].stride + op[o].n * sz;
                if (!ld[op[o].loader].used) { ld[op[o].loader].used = 1; ld[op[o].loader].src = lo; ld[op[o].loader].len = hi; }
                else { if (lo < ld[op[o].loader].src) ld[op[o].loader].src = lo; if (hi > ld[op[o].loader].len) ld[op[o].loader].len = hi; }
            }
            if (ok) {
                asize = 0;
                for (int l = 0; l < 12; l++) if (ld[l].used) {
                    ld[l].src &= ~3u; ld[l].len = (ld[l].len - ld[l].src + 3) & ~3u;   /* len: from src (word aligned) to the end */
                    ld[l].roff = (uint32_t)asize; asize += ld[l].len;
                }
                for (uint32_t a = 0; a < 12; a++) { fa[4 * a] = fa[4 * a + 1] = fa[4 * a + 2] = fa[4 * a + 3] = 0; memcpy(fdef + 4 * a, a < nattr ? &def[a] : &(V4){ 0, 0, 0, 1 }, 16); }
                for (int o = 0; o < nop; o++) {
                    uint32_t *f = fa + 4 * op[o].dst;
                    f[0] = ld[op[o].loader].roff + (op[o].addr + imin * op[o].stride - ld[op[o].loader].src);
                    f[1] = op[o].stride; f[2] = op[o].type | (uint32_t)op[o].n << 4 | 0x100u;
                }
                raw = 1;
            }
        }
        if (gpu && !raw) {
            for (uint32_t a = 0; a < 12; a++) { fa[4 * a] = a * 16; fa[4 * a + 1] = nattr * 16; fa[4 * a + 2] = 3 | 4 << 4 | 0x100u; fa[4 * a + 3] = 0; memset(fdef + 4 * a, 0, 16); }
            asize = (VkDeviceSize)nvlist * nattr * 16;
        }
        if (gpu) {
            memcpy(vsu, pica_vs_cf, 96 * 16);
            uint32_t *w = (uint32_t *)(vsu + 96 * 16);
            w[0] = R(0x2B0); w[1] = R(0x2BA) & 0xffff; w[2] = R(0x2BD); w[3] = R(0x04F);
            w[4] = R(0x2B1); w[5] = R(0x2B2); w[6] = R(0x2B3); w[7] = R(0x2B4);
            w[8] = R(0x2BB); w[9] = R(0x2BC); w[10] = nattr; w[11] = 0;
            for (int i = 0; i < 7; i++) w[12 + i] = R(0x050 + i);
            w[19] = 0;
        }
        {   /* per-draw uniforms: raw registers 0x040-0x1DF + decoded light parameters */
            memcpy(fsu, &pica_regs[0x040], 104 * 16);
            float *lf = (float *)(fsu + 104 * 16);
            for (int l = 0; l < 8; l++) {
                int b = 0x140 + 0x10 * l;
                lf[4 * l + 0] = pica_f16(R(b + 4)); lf[4 * l + 1] = pica_f16(R(b + 4) >> 16); lf[4 * l + 2] = pica_f16(R(b + 5)); lf[4 * l + 3] = 0;
                int sx = (int)(R(b + 6) & 0x1fff), sy = (int)((R(b + 6) >> 16) & 0x1fff), sz = (int)(R(b + 7) & 0x1fff);
                if (sx & 0x1000) sx -= 0x2000; if (sy & 0x1000) sy -= 0x2000; if (sz & 0x1000) sz -= 0x2000;
                lf[32 + 4 * l + 0] = (float)sx / 2047.f; lf[32 + 4 * l + 1] = (float)sy / 2047.f; lf[32 + 4 * l + 2] = (float)sz / 2047.f; lf[32 + 4 * l + 3] = 0;
                lf[64 + 4 * l + 0] = pica_f20(R(b + 0xA)); lf[64 + 4 * l + 1] = pica_f20(R(b + 0xB)); lf[64 + 4 * l + 2] = lf[64 + 4 * l + 3] = 0;
            }
            lf[96] = f24(R(0x04D)); lf[97] = f24(R(0x04E)); lf[98] = lf[99] = 0;
        }
        int same_batch = recording && last_ubatch == batch_no;
        int vs_new = gpu && !(same_batch && last_vsoff != UINT64_MAX && !memcmp(vsu, last_vsu, sizeof vsu));
        int fs_new = !(same_batch && last_uoff != UINT64_MAX && !memcmp(fsu, last_fsu, sizeof fsu));
        if (gpu) { aoff = ring_alloc(&abuf, asize ? asize : 16, 256); vsoff = vs_new ? ring_alloc(&ubuf, VS_UBO_SIZE, 256) : last_vsoff; if (aoff == UINT64_MAX || vsoff == UINT64_MAX) full = 1; }
        else { voff = ring_alloc(&vbuf, (VkDeviceSize)nvlist * sizeof(Vtx), 16); if (voff == UINT64_MAX) full = 1; }
        VkDeviceSize ioff = ring_alloc(&ibuf, (VkDeviceSize)nidx * 4, 4);
        VkDeviceSize uoff = fs_new ? ring_alloc(&ubuf, UBO_SIZE, 256) : last_uoff;
        if (ioff == UINT64_MAX || uoff == UINT64_MAX) full = 1;
        VkDescriptorImageInfo ii[3];
        uint32_t tex_en = R(0x080);
        for (int u = 0; u < 3 && !full; u++) {
            VkImageView v = (tex_en >> u) & 1 ? tex_get(u, &full) : white.view;
            ii[u] = (VkDescriptorImageInfo){ sampler_for(u), v ? v : white.view, VK_IMAGE_LAYOUT_GENERAL };
        }
        if (!full && lut_uploaded_version != light_lut_version) {
            VkDeviceSize lo = ring_alloc(&lbuf, sizeof light_lut + sizeof fog_lut, 256);
            if (lo == UINT64_MAX) full = 1;
            else { memcpy(lbuf.ptr + lo, light_lut, sizeof light_lut); memcpy(lbuf.ptr + lo + sizeof light_lut, fog_lut, sizeof fog_lut);   /* shader: lut[6144 + i] */
                   lut_off = lo; lut_uploaded_version = light_lut_version; }
        }
        if (!full && gpu && prog_uploaded_version != pica_vs_version) {
            VkDeviceSize po = ring_alloc(&pbuf, sizeof pica_vs_prog, 256);
            if (po == UINT64_MAX) full = 1;
            else { memcpy(pbuf.ptr + po, pica_vs_prog, sizeof pica_vs_prog); prog_off = po; prog_uploaded_version = pica_vs_version; }
        }
        if (full) {
            if (attempt) { fprintf(stderr, "pica_vk: draw does not fit an empty batch, dropped\n"); return; }
            pica_vk_flush(); last_ubatch = 0; continue;
        }
        /* vertices (CPU-shaded) or attributes + vertex shader state (GPU), and indices */
        if (gpu) {
            if (raw) { for (int l = 0; l < 12; l++) if (ld[l].used) memcpy(abuf.ptr + aoff + ld[l].roff, g_mem + ld[l].src, ld[l].len); }
            else {   /* built in cached memory, then one copy into the (write-combined) ring */
                static V4 *stage; static size_t cap_stage;
                if (cap_stage < (size_t)nvlist * nattr) { cap_stage = (size_t)nvlist * nattr; stage = realloc(stage, cap_stage * sizeof(V4)); }
                pica_fetch_attrs_n(vlist_index, nvlist, stage);
                memcpy(abuf.ptr + aoff, stage, (size_t)nvlist * nattr * sizeof(V4));
            }
            if (vs_new) { memcpy(ubuf.ptr + vsoff, vsu, sizeof vsu); memcpy(last_vsu, vsu, sizeof vsu); last_vsoff = vsoff; }
        } else {
            Vtx *vd = (Vtx *)(vbuf.ptr + voff);
            for (uint32_t i = 0; i < nvlist; i++) vd[i] = vcache[vlist_slot[i]];
        }
        uint32_t *id = (uint32_t *)(ibuf.ptr + ioff);
        if (raw) for (uint32_t i = 0; i < nidx; i++) id[i] = vlist_index[remap[slots[tri_idx[i]]]] - imin;   /* raw: the vertex's own index */
        else for (uint32_t i = 0; i < nidx; i++) id[i] = remap[slots[tri_idx[i]]];
        if (fs_new) { memcpy(ubuf.ptr + uoff, fsu, sizeof fsu); memcpy(last_fsu, fsu, sizeof fsu); last_uoff = uoff; }
        last_ubatch = batch_no;

        /* fixed-function state */
        uint32_t om = R(0x107), bl = R(0x101);
        int color_on = (R(0x113) & 15) != 0, depth_on = cur_d && ((R(0x114) & 3) || (R(0x115) & 3));
        PKey k; memset(&k, 0, sizeof k);
        k.vs = (uint64_t)(uintptr_t)vsm; k.blend = (R(0x100) >> 8) & 1 ? bl : 0; k.blend_on = (uint8_t)((R(0x100) >> 8) & 1);
        k.cmask = (uint8_t)(color_on ? (om >> 8) & 15 : 0); k.has_depth = (uint8_t)(cur_d != NULL); k.gpu_vs = (uint8_t)gpu;
        /* the game's usual depth mapping (Z-buffer, scale -1, offset 0) is exactly the rasterizer's depth: no gl_FragDepth write */
        k.nd = (uint8_t)((R(0x06D) & 1) && (R(0x04D) & 0xffffff) == 0xBF0000 && (R(0x04E) & 0x7fffff) == 0);
        static int nospec = -1; if (nospec < 0) nospec = getenv("PWR_VK_NOSPEC") != NULL;   /* A/B: one generic fragment shader */
        if (!nospec) {
            static const int tb[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };
            k.sc[0] = 1;
            for (int i = 0; i < 6; i++) { k.sc[1 + i] = R(tb[i]) & 0x0FFF0FFF; k.sc[7 + i] = R(tb[i] + 1) & 0x777FFF; k.sc[13 + i] = R(tb[i] + 2) & 0x000F000F; k.sc[19 + i] = R(tb[i] + 4) & 0x00030003; }
            k.sc[25] = R(0x0E0) & 0x1FF07; k.sc[26] = R(0x104) & 0x71;   /* alpha test enable + function: the animated reference stays a uniform */ k.sc[27] = R(0x080) & 0x2007;
            k.sc[28] = (R(0x08F) & 1) && !(R(0x1C6) & 1); k.sc[29] = R(0x06D) & 1;
        }
        /* skippable only for screen-sized targets (eyes, screens) and ones drawn in each of the last 8 frames */
        extern unsigned g_gsp_top_swaps; uint32_t fr = g_gsp_top_swaps;
        if (fr != cur_c->frame_last) { cur_c->frames = fr == cur_c->frame_last + 1 ? cur_c->frames + 1 : 1; cur_c->frame_last = fr; }
        VkPipeline p = pipeline_for(k, cur_c->frames < 8 && cur_c->w * cur_c->h < 240 * 320);
        if (!p) return;
        if (seal_before_draw) { seal(); seal_before_draw = 0; }
        if (cur_d) {   /* two colour targets sharing a depth buffer in one segment: keep their interleaving */
            if (cur_d->seg_id == seg && cur_d->seg_color != cur_c) seal();
            cur_d->seg_id = seg; cur_d->seg_color = cur_c;
        }
        if (nrec == caprec) { caprec = caprec ? caprec * 2 : 4096; recs = realloc(recs, (size_t)caprec * sizeof *recs); }
        DrawRec *rc = &recs[nrec++];
        rc->c = cur_c; rc->d = cur_d; rc->p = p; rc->gpu = (uint8_t)gpu; rc->voff = voff; rc->ioff = ioff; rc->nidx = nidx;
        float vw = f24(R(0x041)), vh = f24(R(0x043));
        int vx = (int)(R(0x068) & 0x3ff), vy = (int)(R(0x068) >> 16 & 0x3ff);
        if (vx & 0x200) vx -= 0x400; if (vy & 0x200) vy -= 0x400;
        float S = (float)cur_scale;
        rc->vp = (VkViewport){ (float)vx * S, (float)vy * S, 2 * vw * S, 2 * vh * S, 0, 1 };
        rc->sc = (VkRect2D){ { 0, 0 }, { cur_c->w * (uint32_t)cur_scale, cur_c->h * (uint32_t)cur_scale } };
        if (R(0x065) & 3) {
            int ax = (int)(R(0x066) & 0x3ff), ay = (int)(R(0x066) >> 16 & 0x3ff), bx = (int)(R(0x067) & 0x3ff), by = (int)(R(0x067) >> 16 & 0x3ff);
            if (bx >= (int)cur_c->w) bx = (int)cur_c->w - 1; if (by >= (int)cur_c->h) by = (int)cur_c->h - 1;
            int K = cur_scale;
            rc->sc = (VkRect2D){ { ax * K, ay * K }, { bx >= ax ? (uint32_t)((bx - ax + 1) * K) : 0, by >= ay ? (uint32_t)((by - ay + 1) * K) : 0 } };
        }
        uint32_t kc = R(0x103);
        rc->bc[0] = (kc & 255) / 255.f; rc->bc[1] = (kc >> 8 & 255) / 255.f; rc->bc[2] = (kc >> 16 & 255) / 255.f; rc->bc[3] = (kc >> 24) / 255.f;
        /* pica_sw.c: cull mode 1 drops triangles with positive window-space area, 2 negative ones; Vulkan calls a
           negative area (in its sign convention) front-facing for COUNTER_CLOCKWISE. Checked on the Mii heads (cull
           mode 1): the other way round drops the face and keeps the inside of the hair */
        int cm = (int)(R(0x040) & 3);
        rc->cull = cm == 1 ? VK_CULL_MODE_BACK_BIT : cm == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        { static int nocull = -1; if (nocull < 0) nocull = getenv("PWR_VK_NOCULL") != NULL; if (nocull) rc->cull = VK_CULL_MODE_NONE; }
        static const VkCompareOp cmp[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
                                            VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL };
        /* PICA writes depth with the test disabled (the game's depth-clearing quads rely on it); Vulkan writes depth only
           while the test is enabled, so a disabled test becomes ALWAYS */
        rc->dt = (uint8_t)(depth_on && (om & 1)); rc->dw = (uint8_t)(depth_on && ((om >> 12) & 1)); rc->dop = cmp[om >> 4 & 7];
        if (rc->dw && !rc->dt) { rc->dt = 1; rc->dop = VK_COMPARE_OP_ALWAYS; }
        /* stencil (0x105 test, 0x106 ops; same numbering as Vulkan) only with a D24S8 buffer; writes need 0x115 */
        uint32_t sf = R(0x105), so = R(0x106);
        rc->ste = (uint8_t)(has_stencil && depth_on && (sf & 1) && cur_d->fmt == 3);
        rc->sfn = cmp[sf >> 4 & 7]; rc->sref = (uint8_t)(sf >> 16); rc->scmask = (uint8_t)(sf >> 24); rc->swmask = (R(0x115) & 3) ? (uint8_t)(sf >> 8) : 0;
        rc->sop[0] = (VkStencilOp)(so & 7); rc->sop[1] = (VkStencilOp)(so >> 4 & 7); rc->sop[2] = (VkStencilOp)(so >> 8 & 7);
        rc->b[0] = (VkDescriptorBufferInfo){ ubuf.buf, uoff, UBO_SIZE };
        rc->b[1] = (VkDescriptorBufferInfo){ lbuf.buf, lut_off, sizeof light_lut + sizeof fog_lut };
        rc->b[2] = (VkDescriptorBufferInfo){ ubuf.buf, vsoff, VS_UBO_SIZE };
        rc->b[3] = (VkDescriptorBufferInfo){ pbuf.buf, prog_off, sizeof pica_vs_prog };
        rc->b[4] = (VkDescriptorBufferInfo){ abuf.buf, aoff, gpu && asize ? asize : 16 };
        memcpy(rc->ii, ii, sizeof ii);
        cur_c->dirty_batch = batch_no;
        g_vk_draws++; g_vk_verts += nvlist; g_vk_idx += nidx;
        kick();
        return;
    }
}
