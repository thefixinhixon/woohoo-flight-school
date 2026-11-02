/* Presentation: SDL2 window + Vulkan swapchain.
 * Each frame the two LCD framebuffers (stored rotated, 240 pixels per line) are decoded into a 400x240 (top) and a
 * 320x240 (bottom) RGBA image. The GPU presenter uploads them as textures and draws them into an off-screen image the
 * size of the window, placed by the screen layout and filtered (nearest, linear, sharp bilinear; FSR/FXAA, see below),
 * then copies that image into the swapchain image. A copy, not a blit or a render into the swapchain: software/linear
 * swapchains (MESA_VK_WSI_DEBUG=sw on Xvfb) support transfers into them. Without Vulkan 1.3 dynamic rendering the
 * screens are scaled on the CPU (nearest) into a staging buffer instead, with the same layouts.
 *
 * Layouts (video_option "layout", F9 cycles): stacked (top above bottom), side (side by side), large (big top screen,
 * small bottom screen at its lower right), top / bottom (one screen), auto: a small state machine picks "stacked"
 * in menus and the auto_gameplay layout (large, or top = bottom screen hidden) while flying:
 *     MENU -> GAMEPLAY   the circle pad has been held for 1.2 s (steering)
 *     GAMEPLAY -> MENU   Start or Select (pause menu), a click/touch on the bottom screen, or 8 s without steering
 * Aspect: keep (letterbox), integer (largest whole-number scale), stretch. Fullscreen: borderless (desktop) or
 * exclusive; F11 or Alt+Enter toggles. Mouse/touch input follows the bottom screen wherever the layout puts it.
 * Filters: nearest, linear, sharp bilinear, FXAA (present.frag), FSR 1 = AMD FidelityFX EASU upscale into a per-screen image
 * of the output size, then RCAS sharpening into the window (fsr_easu/fsr_rcas.frag, shaders/fsr/, MIT). F1 opens the settings
 * overlay (below); 3D resolution and anisotropic filtering are passed to pica_vk.c. */
#include "gpu.h"
#include "input.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
extern int g_input_none;

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240
#define MAX_IMAGES 8

static SDL_Window *win;
static VkInstance inst;
static VkSurfaceKHR surf;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSwapchainKHR swap;
static VkImage sc_images[MAX_IMAGES];
static uint32_t sc_count;
static VkExtent2D sc_extent;
static VkFormat sc_format;
static VkDeviceMemory stage_mem;
static VkBuffer stage;
static uint8_t *stage_ptr;
static VkDeviceSize stage_size;
static int sc_bgra;
static VkCommandPool pool;
static VkCommandBuffer cmdbuf;
static VkSemaphore sem_acquire, sem_done[MAX_IMAGES];   /* one per swapchain image: reused only after that image is re-acquired */
static VkFence fence;
static uint8_t top_px[TOP_W * SCR_H * 4], bot_px[BOT_W * SCR_H * 4];
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;   /* the queue is shared with the PICA renderer */
static int vk13;
unsigned g_vid_acquire_fail, g_vid_present_fail, g_vid_presented; int g_vid_last_err;

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "video: %s failed (%d)\n", #x, r_); return -1; } } while (0)

/* ---------------- options ---------------- */
enum { L_STACKED, L_SIDE, L_LARGE, L_TOP, L_BOTTOM, L_AUTO, L_COUNT };
static const char *layout_names[L_COUNT] = { "stacked", "side", "large", "top", "bottom", "auto" };
enum { F_NEAREST, F_LINEAR, F_SHARP, F_FSR, F_FXAA, F_COUNT };
static const char *filter_names[F_COUNT] = { "nearest", "linear", "sharp", "fsr", "fxaa" };
static const char *aspect_names[3] = { "keep", "integer", "stretch" };
static const char *fs_names[3] = { "off", "borderless", "exclusive" };
static int opt_layout = L_STACKED, opt_filter = F_NEAREST, opt_aspect = 0, opt_fs = 0, opt_auto_gameplay = L_LARGE;
static int opt_res = 1, opt_aniso = 1, opt_fps = 0; static float opt_sharp = 0.25f;
static int opt_select_layout = 1;   /* the gamepad's Back/Select button cycles the screen layout instead of pressing 3DS Select */
static int opt_free_look = 0;       /* right stick orbits the chase camera (freelook.c) */
static int opt_drawdist = 1;        /* draw distance multiplier (drawdist.c), read when the game starts */
static int fs_last = 1;   /* the fullscreen mode F11 toggles to */

static int pick(const char *v, const char *const *names, int n) { for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i; return -1; }
int video_option(const char *name, const char *value) {
    int i;
    if (!strcmp(name, "layout") && (i = pick(value, layout_names, L_COUNT)) >= 0) opt_layout = i;
    else if (!strcmp(name, "filter") && (i = pick(value, filter_names, F_COUNT)) >= 0) opt_filter = i;
    else if (!strcmp(name, "aspect") && (i = pick(value, aspect_names, 3)) >= 0) opt_aspect = i;
    else if (!strcmp(name, "fullscreen") && (i = pick(value, fs_names, 3)) >= 0) { opt_fs = i; if (i) fs_last = i; }
    else if (!strcmp(name, "auto_gameplay") && (i = pick(value, layout_names, L_AUTO)) >= 0) opt_auto_gameplay = i;
    else if (!strcmp(name, "fsr_sharpness")) { char *e; double v = strtod(value, &e); if (*e || v < 0 || v > 2) return -1; opt_sharp = (float)v; }
    else if (!strcmp(name, "resolution")) { i = atoi(value); if (i < 1 || i > 4) return -1; opt_res = i; pica_vk_quality(opt_res, opt_aniso); }
    else if (!strcmp(name, "aniso")) { i = atoi(value); if (i != 1 && i != 2 && i != 4 && i != 8 && i != 16) return -1; opt_aniso = i; pica_vk_quality(opt_res, opt_aniso); }
    else if (!strcmp(name, "show_fps")) opt_fps = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "select_layout")) opt_select_layout = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "draw_distance")) { i = atoi(value); if (i < 1 || i > 4) return -1; opt_drawdist = i; drawdist_set(i); }
    else if (!strcmp(name, "free_look")) { opt_free_look = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true"); freelook_enable(opt_free_look); }
    else return -1;
    return 0;
}

static void apply_fullscreen(void) {
    SDL_SetWindowFullscreen(win, opt_fs == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : opt_fs == 2 ? SDL_WINDOW_FULLSCREEN : 0);
}

/* ---------------- layouts ---------------- */
typedef struct { float x, y, w, h; int on; } Rect;   /* window pixels */
static int auto_state;   /* 0 = menu, 1 = gameplay */

static int effective_layout(void) { return opt_layout == L_AUTO ? (auto_state ? opt_auto_gameplay : L_STACKED) : opt_layout; }

/* the layout's canvas in 3DS pixels, scaled into the window by the aspect mode */
static void layout_rects(int ww, int wh, Rect *top, Rect *bot) {
    float cw, ch; Rect t = { 0, 0, TOP_W, SCR_H, 1 }, b = { 40, SCR_H, BOT_W, SCR_H, 1 };
    switch (effective_layout()) {
    case L_SIDE:   cw = TOP_W + BOT_W; ch = SCR_H; b = (Rect){ TOP_W, 0, BOT_W, SCR_H, 1 }; break;
    case L_LARGE:  cw = TOP_W + BOT_W / 2; ch = SCR_H; b = (Rect){ TOP_W, SCR_H / 2, BOT_W / 2, SCR_H / 2, 1 }; break;
    case L_TOP:    cw = TOP_W; ch = SCR_H; b.on = 0; break;
    case L_BOTTOM: cw = BOT_W; ch = SCR_H; t.on = 0; b = (Rect){ 0, 0, BOT_W, SCR_H, 1 }; break;
    default:       cw = TOP_W; ch = 2 * SCR_H; break;
    }
    float sx = (float)ww / cw, sy = (float)wh / ch;
    if (opt_aspect != 2) { float s = sx < sy ? sx : sy; if (opt_aspect == 1 && s >= 1) s = floorf(s); sx = sy = s; }
    float ox = ((float)ww - cw * sx) / 2, oy = ((float)wh - ch * sy) / 2;
    *top = (Rect){ ox + t.x * sx, oy + t.y * sy, t.w * sx, t.h * sy, t.on };
    *bot = (Rect){ ox + b.x * sx, oy + b.y * sy, b.w * sx, b.h * sy, b.on };
}

/* auto layout: see the comment at the top */
static void auto_update(double now, int bottom_click) {
    static double stick_since = -1, last_stick; static uint32_t prev_btn;
    uint32_t btn = input_buttons(), pressed = btn & ~prev_btn; prev_btn = btn;
    int stick = input_stick_active();
    if (stick) last_stick = now;
    if (!auto_state) {
        if (!stick) stick_since = -1;
        else if (stick_since < 0) stick_since = now;
        else if (now - stick_since >= 1.2) auto_state = 1;
    } else if ((pressed & 0xC) || bottom_click || now - last_stick > 8.0) { auto_state = 0; stick_since = -1; }
}

/* ---------------- Vulkan setup ---------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; } Img;
static int img_make(Img *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ci, NULL, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(dev, &ai, NULL, &im->mem)); VK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vi, NULL, &im->view));
    im->w = w; im->h = h;
    return 0;
}
static void img_free(Img *im) {
    if (!im->img) return;
    vkDestroyImageView(dev, im->view, NULL); vkDestroyImage(dev, im->img, NULL); vkFreeMemory(dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* GPU presenter */
static int gpu_ok;
static Img tex_top, tex_bot, target;             /* screens, and the window-sized image drawn into */
static VkFormat target_fmt;
static VkSampler smp_linear;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout play;
static VkPipeline pipe, pipe_easu, pipe_rcas, pipe_rgba;   /* screens/overlay; FSR upscale into fsr_tmp; FSR sharpen; un-rotate into hi_scr */
static Img fsr_tmp[2], tex_ov, tex_fps, hi_scr[2], tex_toast;          /* FSR intermediates (top, bottom); settings overlay; frame counter;
                                                               the screens at the 3D internal resolution (pica_vk scan-out, screen-oriented) */
int pica_vk_scanout(uint32_t addr, VkImageView *view, uint32_t *w, uint32_t *h);
void pica_vk_scan_lock(int on);
static VkBuffer up_buf; static VkDeviceMemory up_mem; static uint8_t *up_ptr;   /* uploads: screens, then overlay, then frame counter */
/* on-screen UI panels, in 1x units: drawn at a whole-number scale k (1..4, font_ui.h) that suits the window and shown pixel
   for pixel, so text stays sharp at any window size; the pixel buffers hold the largest scale */
#define OV_W 420
#define OV_H 292
#define FPS_W 72
#define FPS_H 18
#define TOAST_W 240
#define TOAST_H 22
#define UI_KMAX 4
static uint8_t toast_px[TOAST_W * TOAST_H * 4 * UI_KMAX * UI_KMAX];   /* layout label (next_layout) */
static uint8_t ov_px[OV_W * OV_H * 4 * UI_KMAX * UI_KMAX], fps_px[FPS_W * FPS_H * 4 * UI_KMAX * UI_KMAX];
#define UP_OV (sizeof top_px + sizeof bot_px)
#define UP_FPS (UP_OV + sizeof ov_px)
#define UP_TOAST (UP_FPS + sizeof fps_px)
static PFN_vkCmdPushDescriptorSetKHR push_desc;
typedef struct { float dst[4], src[4]; int mode[4]; float extra[4]; } Push;

static const uint32_t present_vert[] =
#include "present.vert.inc"
;

static VkShaderModule shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(dev, &ci, NULL, &m); return m;
}

/* one pipeline per fragment shader: present.vert quad, alpha blending (the screens write alpha 1), dynamic viewport/scissor */
static VkPipeline make_pipe(const uint32_t *fs, size_t fs_size, VkFormat fmt) {
    VkShaderModule vm = shader(present_vert, sizeof present_vert), fm = shader(fs, fs_size);
    VkPipeline p = VK_NULL_HANDLE;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState ba = { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
        VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cb, .pDynamicState = &dy, .layout = play };
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &p)) p = VK_NULL_HANDLE;
    }
    if (vm) vkDestroyShaderModule(dev, vm, NULL);
    if (fm) vkDestroyShaderModule(dev, fm, NULL);
    return p;
}

static int gpu_init(void) {
    if (!vk13) return -1;
    push_desc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!push_desc) return -1;
    if (img_make(&tex_top, TOP_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        img_make(&tex_bot, BOT_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = UP_TOAST + sizeof toast_px, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &up_buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, up_buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &up_mem)); VK(vkBindBufferMemory(dev, up_buf, up_mem, 0));
    VK(vkMapMemory(dev, up_mem, 0, bi.size, 0, (void **)&up_ptr));
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VK(vkCreateSampler(dev, &si, NULL, &smp_linear));
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 1, .pBindings = &b };
    VK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &play));
    target_fmt = sc_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : sc_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : sc_format;
    static const uint32_t fs_present[] =
#include "present.frag.inc"
    ;
    static const uint32_t fs_easu[] =
#include "fsr_easu.frag.inc"
    ;
    static const uint32_t fs_rcas[] =
#include "fsr_rcas.frag.inc"
    ;
    pipe = make_pipe(fs_present, sizeof fs_present, target_fmt);
    pipe_easu = make_pipe(fs_easu, sizeof fs_easu, VK_FORMAT_R8G8B8A8_UNORM);
    pipe_rcas = make_pipe(fs_rcas, sizeof fs_rcas, target_fmt);
    pipe_rgba = make_pipe(fs_present, sizeof fs_present, VK_FORMAT_R8G8B8A8_UNORM);
    if (!pipe || !pipe_easu || !pipe_rcas || !pipe_rgba) return -1;
    /* tex_ov, tex_fps, tex_toast: made at their first use, at the scale of that moment (ui_img) */
    return 0;
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "video: swapchain cannot be a copy target\n"); return -1; }
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[64]; if (nf > 64) nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fmts);
    sc_format = fmts[0].format; VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { sc_format = fmts[i].format; cs = fmts[i].colorSpace; }
    sc_extent = caps.currentExtent;
    if (sc_extent.width == UINT32_MAX) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); sc_extent.width = (uint32_t)w; sc_extent.height = (uint32_t)h; }
    if (!sc_extent.width || !sc_extent.height) return 0;   /* minimised: try again next frame */
    uint32_t n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n,
        .imageFormat = sc_format, .imageColorSpace = cs, .imageExtent = sc_extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = swap };
    VkSwapchainKHR old = swap;
    VK(vkCreateSwapchainKHR(dev, &ci, NULL, &swap));
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    sc_count = MAX_IMAGES; VK(vkGetSwapchainImagesKHR(dev, swap, &sc_count, sc_images));
    sc_bgra = sc_format == VK_FORMAT_B8G8R8A8_UNORM || sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    if (gpu_ok) {   /* the window-sized image the presenter draws into */
        img_free(&target);
        if (img_make(&target, sc_extent.width, sc_extent.height, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return -1;
        return 0;
    }
    /* CPU path: staging buffer sized to the swapchain */
    if (stage) { vkUnmapMemory(dev, stage_mem); vkDestroyBuffer(dev, stage, NULL); vkFreeMemory(dev, stage_mem, NULL); stage = VK_NULL_HANDLE; }
    stage_size = (VkDeviceSize)sc_extent.width * sc_extent.height * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = stage_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &stage));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stage, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &stage_mem)); VK(vkBindBufferMemory(dev, stage, stage_mem, 0));
    VK(vkMapMemory(dev, stage_mem, 0, stage_size, 0, (void **)&stage_ptr));
    return 0;
}

static int recreate_swapchain(void) {
    pthread_mutex_lock(&queue_lock); vkDeviceWaitIdle(dev); pthread_mutex_unlock(&queue_lock);
    return create_swapchain();
}

int video_init(int scale) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (g_input_none ? 0 : SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK))) { fprintf(stderr, "video: SDL_Init: %s\n", SDL_GetError()); return -1; }
    static const int cw[L_COUNT] = { TOP_W, TOP_W + BOT_W, TOP_W + BOT_W / 2, TOP_W, BOT_W, TOP_W }, chh[L_COUNT] = { 2 * SCR_H, SCR_H, SCR_H, SCR_H, SCR_H, 2 * SCR_H };
    win = SDL_CreateWindow("Woohoo! Flight School", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cw[opt_layout] * scale, chh[opt_layout] * scale, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "video: SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    SDL_SetWindowMinimumSize(win, BOT_W, SCR_H);
    if (opt_fs) apply_fullscreen();
    unsigned ne = 0; SDL_Vulkan_GetInstanceExtensions(win, &ne, NULL);
    const char *exts[16]; if (ne > 16) ne = 16; SDL_Vulkan_GetInstanceExtensions(win, &ne, exts);
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pwr", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts };
    VK(vkCreateInstance(&ici, NULL, &inst));
    if (!SDL_Vulkan_CreateSurface(win, inst, &surf)) { fprintf(stderr, "video: surface: %s\n", SDL_GetError()); return -1; }
    uint32_t np = 8; VkPhysicalDevice pds[8]; VK(vkEnumeratePhysicalDevices(inst, &np, pds));
    for (uint32_t i = 0; i < np && !phys; i++) {
        uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &pres);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { phys = pds[i]; qfam = q; break; }
        }
    }
    if (!phys) { fprintf(stderr, "video: no Vulkan device can present to the window\n"); return -1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    fprintf(stderr, "video: Vulkan device %s\n", pp.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    /* the hardware PICA renderer (pica_vk.c) and the GPU presenter need Vulkan 1.3 dynamic rendering + extended dynamic state and push descriptors */
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, NULL);
    VkExtensionProperties *dep = calloc(nde ? nde : 1, sizeof *dep); vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, dep);
    int has_push = 0; for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) has_push = 1;
    free(dep);
    if (pp.apiVersion >= VK_API_VERSION_1_3) vkGetPhysicalDeviceFeatures2(phys, &f2);
    vk13 = pp.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering && f13.shaderDemoteToHelperInvocation && has_push;
    VkPhysicalDeviceVulkan13Features en13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceFeatures2 en2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &en13 };
    en2.features.samplerAnisotropy = f2.features.samplerAnisotropy;   /* pica_vk.c: anisotropic filtering option */
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = vk13 ? &en2 : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = vk13 ? 2 : 1, .ppEnabledExtensionNames = dext };
    if (!vk13) fprintf(stderr, "video: no Vulkan 1.3 dynamic rendering/push descriptors: hardware PICA renderer and GPU presenter unavailable\n");
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    /* swapchain format first (the presenter's target uses it), then the presenter, then the swapchain-sized resources */
    { uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, NULL); VkSurfaceFormatKHR fm[64]; if (nf > 64) nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fm);
      sc_format = fm[0].format; for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) sc_format = fm[i].format; }
    gpu_ok = !gpu_init();
    if (!gpu_ok) fprintf(stderr, "video: GPU presenter unavailable, scaling on the CPU (nearest)\n");
    if (create_swapchain()) return -1;

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &cai, &cmdbuf));
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(dev, &si, NULL, &sem_acquire)); for (int i = 0; i < MAX_IMAGES; i++) VK(vkCreateSemaphore(dev, &si, NULL, &sem_done[i]));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VK(vkCreateFence(dev, &fi, NULL, &fence));
    return 0;
}

/* LCD framebuffers are rotated: each line holds 240 pixels running bottom-to-top of one screen column */
static void decode_screen(const ScreenSource *s, int w, uint8_t *out) {
    static const int bpp_of[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };
    int bpp = bpp_of[s->format & 7];
    if (!s->valid || !s->addr || !bpp) {   /* no scan-out source yet: dark grey, so screenshots show the screen area */
        for (int i = 0; i < w * SCR_H; i++) { out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 40; out[4 * i + 3] = 255; }
        return;
    }
    uint32_t stride = s->stride ? s->stride : 240u * (uint32_t)bpp;
    for (int x = 0; x < w; x++) for (int y = 0; y < SCR_H; y++) {
        const uint8_t *p = g_mem + s->addr + (uint32_t)x * stride + (uint32_t)(239 - y) * (uint32_t)bpp;
        uint8_t *d = out + ((size_t)y * (size_t)w + (size_t)x) * 4; uint32_t v;
        switch (s->format & 7) {
        case 0: d[0] = p[3]; d[1] = p[2]; d[2] = p[1]; break;
        case 1: d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; break;
        case 2: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 5) & 63) << 2); d[2] = (uint8_t)((v & 31) << 3); break;
        case 3: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 6) & 31) << 3); d[2] = (uint8_t)(((v >> 1) & 31) << 3); break;
        case 4: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 12) * 17); d[1] = (uint8_t)(((v >> 8) & 15) * 17); d[2] = (uint8_t)(((v >> 4) & 15) * 17); break;
        }
        d[3] = 255;
    }
}

static void barrier_img(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* gamepads: only without --input=none. Controllers with an SDL mapping use the controller API; others fall back to raw joystick
   buttons/axes/hat (0..3 = south,east,west,north as most pads number them, 4/5 = L/R, 6/7 = select/start) */
static SDL_GameController *gc; static SDL_Joystick *joy;
static void pad_open(int idx) {
    if (gc || joy) return;
    if (SDL_IsGameController(idx) && (gc = SDL_GameControllerOpen(idx))) { fprintf(stderr, "pwr: controller opened: %s\n", SDL_GameControllerName(gc)); return; }
    if ((joy = SDL_JoystickOpen(idx))) fprintf(stderr, "pwr: controller opened: %s (joystick, %d buttons, %d axes, %d hats)\n", SDL_JoystickName(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumAxes(joy), SDL_JoystickNumHats(joy));
}
static int pad_pos(int cb) {   /* SDL controller button -> input_pad_button index */
    switch (cb) {
    case SDL_CONTROLLER_BUTTON_A: return 0; case SDL_CONTROLLER_BUTTON_B: return 1; case SDL_CONTROLLER_BUTTON_X: return 2; case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
    case SDL_CONTROLLER_BUTTON_BACK: return 6; case SDL_CONTROLLER_BUTTON_START: return 7;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 8; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 9;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 10; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 11;
    }
    return -1;
}
static void pad_axes(void) {
    int x, y, rx, ry;
    if (gc) {
        x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX); y = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX); ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    } else if (joy) {   /* raw pads: axes 0/1 left stick; right stick 3/4 on six-axis pads (2 is a trigger), else 2/3 */
        int na = SDL_JoystickNumAxes(joy), r0 = na >= 6 ? 3 : 2;
        x = na > 0 ? SDL_JoystickGetAxis(joy, 0) : 0; y = na > 1 ? -SDL_JoystickGetAxis(joy, 1) : 0;
        rx = na > r0 ? SDL_JoystickGetAxis(joy, r0) : 0; ry = na > r0 + 1 ? -SDL_JoystickGetAxis(joy, r0 + 1) : 0;
    } else return;
    input_pad_stick(x, y);
    freelook_stick(rx, ry);
}

/* ---------------- settings overlay (F1) ----------------
 * A panel drawn on the CPU into an RGBA texture (font_ui.h, at the window's UI scale) and blended over the screens. While it is open the
 * game gets no input (held keys are released when it opens); arrows / D-pad pick and change items, Return or A changes, Escape,
 * B or F1 closes. Changes apply at once; on close they are written to the profile's settings.json ($PWR_SETTINGS, set by
 * tools/pipeline.py), keeping the keys this program does not know. */
#include "font_ui.h"
enum { OI_LAYOUT, OI_AUTO, OI_ASPECT, OI_FS, OI_FILTER, OI_SHARP, OI_RES, OI_ANISO, OI_FPS, OI_SELECT, OI_FREELOOK, OI_DRAWDIST, OI_COUNT };
static const char *ov_labels[OI_COUNT] = { "Screen layout", "Auto layout in flight", "Aspect ratio", "Fullscreen", "Scaling filter",
                                           "FSR sharpness", "3D resolution", "Anisotropic filter", "Frame counter", "Pad Back/Select",
                                           "Free look", "Draw distance" };
static int ov_open, ov_sel, ov_dirty = 1, ov_changed; static char ov_status[48];
static const int aniso_steps[5] = { 1, 2, 4, 8, 16 };

static void ov_value(int i, char *b, size_t n) {
    switch (i) {
    case OI_LAYOUT: snprintf(b, n, "%s", layout_names[opt_layout]); break;
    case OI_AUTO: snprintf(b, n, "%s", opt_auto_gameplay == L_TOP ? "top only (hide bottom)" : "large top + small bottom"); break;
    case OI_ASPECT: snprintf(b, n, "%s", aspect_names[opt_aspect]); break;
    case OI_FS: snprintf(b, n, "%s", fs_names[opt_fs]); break;
    case OI_FILTER: snprintf(b, n, "%s", opt_filter == F_FSR ? "FSR 1 (EASU + RCAS)" : opt_filter == F_FXAA ? "FXAA" : filter_names[opt_filter]); break;
    case OI_SHARP: snprintf(b, n, "%.2f (0 = sharpest)", opt_sharp); break;
    case OI_RES: snprintf(b, n, "%dx%s", opt_res, opt_res == 1 ? " (native 400x240)" : ""); break;
    case OI_ANISO: if (opt_aniso == 1) snprintf(b, n, "off"); else snprintf(b, n, "%dx", opt_aniso); break;
    case OI_FPS: snprintf(b, n, "%s", opt_fps ? "on" : "off"); break;
    case OI_SELECT: snprintf(b, n, "%s", opt_select_layout ? "next screen layout" : "3DS Select"); break;
    case OI_FREELOOK: snprintf(b, n, "%s", opt_free_look ? "on (right stick orbits)" : "off"); break;
    case OI_DRAWDIST: snprintf(b, n, "%dx%s", opt_drawdist, opt_drawdist == 1 ? " (original)" : " (next flight)"); break;
    }
}
static int wrap(int v, int d, int n) { return ((v + d) % n + n) % n; }
static void ov_change(int i, int d) {
    int k;
    switch (i) {
    case OI_LAYOUT: opt_layout = wrap(opt_layout, d, L_COUNT); break;
    case OI_AUTO: opt_auto_gameplay = opt_auto_gameplay == L_TOP ? L_LARGE : L_TOP; break;
    case OI_ASPECT: opt_aspect = wrap(opt_aspect, d, 3); break;
    case OI_FS: opt_fs = wrap(opt_fs, d, 3); if (opt_fs) fs_last = opt_fs; apply_fullscreen(); break;
    case OI_FILTER: opt_filter = wrap(opt_filter, d, F_COUNT); break;
    case OI_SHARP: opt_sharp += 0.25f * (float)d; if (opt_sharp < 0) opt_sharp = 0; if (opt_sharp > 2) opt_sharp = 2; break;
    case OI_RES: opt_res = wrap(opt_res - 1, d, 4) + 1; pica_vk_quality(opt_res, opt_aniso); break;
    case OI_ANISO: for (k = 0; k < 5 && aniso_steps[k] != opt_aniso; k++); opt_aniso = aniso_steps[wrap(k < 5 ? k : 0, d, 5)]; pica_vk_quality(opt_res, opt_aniso); break;
    case OI_FPS: opt_fps = !opt_fps; break;
    case OI_SELECT: opt_select_layout = !opt_select_layout; break;
    case OI_FREELOOK: opt_free_look = !opt_free_look; freelook_enable(opt_free_look); break;
    case OI_DRAWDIST: opt_drawdist = wrap(opt_drawdist - 1, d, 4) + 1; drawdist_set(opt_drawdist); break;
    }
    ov_changed = ov_dirty = 1; ov_status[0] = 0;
}

/* text and boxes in pixels of a panel drawn at scale k (cells of 8k x 16k) */
static void text(uint8_t *px, int pw, int ph, int x, int y, const char *str, uint32_t rgb, int k) {
    for (; *str; str++, x += 8 * k) {
        int c = (unsigned char)*str; if (c < 32 || c > 126) c = '?';
        const unsigned char *g = font_ui_glyph(k, c);
        for (int gy = 0; gy < 16 * k; gy++) for (int gx = 0; gx < 8 * k; gx++) {
            int X = x + gx, Y = y + gy; unsigned a = g[gy * 8 * k + gx];
            if (!a || X < 0 || Y < 0 || X >= pw || Y >= ph) continue;
            uint8_t *d = px + ((size_t)Y * (size_t)pw + (size_t)X) * 4;
            for (int ch = 0; ch < 3; ch++) d[ch] = (uint8_t)((d[ch] * (255 - a) + ((rgb >> (16 - 8 * ch)) & 255) * a) / 255);
            if (d[3] < a) d[3] = (uint8_t)a;
        }
    }
}
static void fill(uint8_t *px, int pw, int x0, int y0, int w, int h, uint32_t rgb, int a) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) {
        uint8_t *d = px + ((size_t)y * (size_t)pw + (size_t)x) * 4; d[0] = (uint8_t)(rgb >> 16); d[1] = (uint8_t)(rgb >> 8); d[2] = (uint8_t)rgb; d[3] = (uint8_t)a;
    }
}
static void ov_render(int k) {   /* the panel at scale k into ov_px (OV_W*k x OV_H*k) */
    int w = OV_W * k, h = OV_H * k;
    fill(ov_px, w, 0, 0, w, h, 0x101820, 225);
    fill(ov_px, w, 0, 0, w, 22 * k, 0x2b4a7a, 240);
    text(ov_px, w, h, 8 * k, 3 * k, "Settings", 0xffffff, k);
    text(ov_px, w, h, (OV_W - 8 * 12 - 8) * k, 3 * k, "F1 to close", 0xb0c4de, k);
    for (int i = 0; i < OI_COUNT; i++) {
        int y = 30 + i * 20; char v[40]; ov_value(i, v, sizeof v);
        if (i == ov_sel) fill(ov_px, w, 4 * k, (y - 2) * k, (OV_W - 8) * k, 20 * k, 0x3d6db5, 235);
        text(ov_px, w, h, 10 * k, y * k, ov_labels[i], i == ov_sel ? 0xffffff : 0xc8d2e0, k);
        text(ov_px, w, h, 186 * k, y * k, v, i == ov_sel ? 0xffffff : 0x9fd3ff, k);
    }
    text(ov_px, w, h, 8 * k, (OV_H - 20) * k, ov_status[0] ? ov_status : "Up/Down pick  Left/Right change", 0x9aa7b8, k);
}

/* settings.json: a flat JSON object; values of the keys below are replaced, other keys kept */
static void settings_save(void) {
    const char *path = getenv("PWR_SETTINGS"); if (!path || !*path) { snprintf(ov_status, sizeof ov_status, "not saved (no profile)"); return; }
    enum { MAXK = 48 }; static char keys[MAXK][32], vals[MAXK][160]; int n = 0;
    FILE *f = fopen(path, "rb"); char buf[8192]; size_t len = 0;
    if (f) { len = fread(buf, 1, sizeof buf - 1, f); fclose(f); } buf[len] = 0;
    for (char *c = buf; *c && n < MAXK; ) {   /* "key": value pairs */
        char *k = strchr(c, '"'); if (!k) break; char *ke = strchr(k + 1, '"'); if (!ke) break;
        char *col = strchr(ke, ':'); if (!col) break;
        char *v = col + 1; while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        char *ve = v;
        if (*v == '"') { ve = v + 1; while (*ve && *ve != '"') { if (*ve == '\\' && ve[1]) ve++; ve++; } if (*ve) ve++; }
        else while (*ve && *ve != ',' && *ve != '}' && *ve != '\n') ve++;
        snprintf(keys[n], sizeof keys[n], "%.*s", (int)(ke - k - 1), k + 1);
        snprintf(vals[n], sizeof vals[n], "%.*s", (int)(ve - v), v);
        while (vals[n][0] && strchr(" \t\r", vals[n][strlen(vals[n]) - 1])) vals[n][strlen(vals[n]) - 1] = 0;
        n++; c = ve;
    }
    char tmp[64]; const char *mine[12][2] = {
        { "layout", layout_names[opt_layout] }, { "auto_gameplay", layout_names[opt_auto_gameplay] }, { "aspect", aspect_names[opt_aspect] },
        { "fullscreen", fs_names[opt_fs] }, { "filter", filter_names[opt_filter] }, { "fsr_sharpness", NULL }, { "resolution", NULL }, { "aniso", NULL }, { "show_fps", NULL },
        { "select_layout", NULL }, { "free_look", NULL }, { "draw_distance", NULL } };
    for (int i = 0; i < 12; i++) {
        char v[64];
        if (i < 5) snprintf(v, sizeof v, "\"%s\"", mine[i][1]);
        else if (i == 5) snprintf(v, sizeof v, "%.2f", opt_sharp);
        else if (i == 6) snprintf(v, sizeof v, "%d", opt_res);
        else if (i == 7) snprintf(v, sizeof v, "%d", opt_aniso);
        else if (i == 8) snprintf(v, sizeof v, "%s", opt_fps ? "true" : "false");
        else if (i == 9) snprintf(v, sizeof v, "%s", opt_select_layout ? "true" : "false");
        else if (i == 10) snprintf(v, sizeof v, "%s", opt_free_look ? "true" : "false");
        else snprintf(v, sizeof v, "%d", opt_drawdist);
        int k; for (k = 0; k < n && strcmp(keys[k], mine[i][0]); k++);
        if (k == n) { if (n == MAXK) continue; snprintf(keys[n], sizeof keys[n], "%s", mine[i][0]); n++; }
        snprintf(vals[k], sizeof vals[k], "%s", v);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (strlen(path) + 5 > sizeof tmp || !(f = fopen(tmp, "wb"))) { snprintf(ov_status, sizeof ov_status, "could not save settings"); return; }
    fprintf(f, "{\n");
    for (int i = 0; i < n; i++) fprintf(f, "  \"%s\": %s%s\n", keys[i], vals[i], i + 1 < n ? "," : "");
    fprintf(f, "}\n"); fclose(f);
    extern int rt_replace_file(const char *tmp, const char *dst);
    if (rt_replace_file(tmp, path)) { remove(tmp); snprintf(ov_status, sizeof ov_status, "could not save settings"); return; }
    snprintf(ov_status, sizeof ov_status, "saved to the profile");
}

static void ov_toggle(void) {
    ov_open = !ov_open; ov_dirty = 1;
    if (ov_open) { input_release_all(); ov_status[0] = 0; ov_changed = 0; }
    else if (ov_changed) settings_save();
}
/* keyboard / controller navigation while open; returns 1 when the event was used */
static int ov_input(int up, int down, int left, int right, int close) {
    if (!ov_open) return 0;
    if (close) { ov_toggle(); return 1; }
    if (up) { ov_sel = wrap(ov_sel, -1, OI_COUNT); ov_dirty = 1; }
    if (down) { ov_sel = wrap(ov_sel, 1, OI_COUNT); ov_dirty = 1; }
    if (left) ov_change(ov_sel, -1);
    if (right) ov_change(ov_sel, 1);
    return 1;
}

/* next screen layout (F9, the gamepad's Back/Select): a label shows the new layout for 1.5 s, and the choice is saved */
static double toast_until; static int toast_dirty; static const char *toast_text;
static double now_s(void);
static void next_layout(void) {
    static const char *nice[L_COUNT] = { "Layout: stacked", "Layout: side by side", "Layout: large top", "Layout: top only", "Layout: bottom only", "Layout: auto" };
    opt_layout = (opt_layout + 1) % L_COUNT; ov_dirty = 1;
    toast_text = nice[opt_layout];   /* drawn at the next present, at the UI scale */
    toast_until = now_s() + 1.5; toast_dirty = 1;
    settings_save();
    fprintf(stderr, "pwr: layout %s\n", layout_names[opt_layout]);
}

/* hotkeys (never passed to the game): F11 / Alt+Enter fullscreen, F9 next layout. Returns 1 when the key was a hotkey. */
static int hotkey(const SDL_KeyboardEvent *k) {
    if (k->type != SDL_KEYDOWN || k->repeat) return ov_open || k->keysym.sym == SDLK_F11 || k->keysym.sym == SDLK_F9 || k->keysym.sym == SDLK_F1 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT));
    if (k->keysym.sym == SDLK_F11 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT))) {
        opt_fs = opt_fs ? 0 : fs_last; apply_fullscreen(); return 1;
    }
    if (k->keysym.sym == SDLK_F9) { next_layout(); return 1; }
    if (k->keysym.sym == SDLK_F1) { ov_toggle(); return 1; }
    if (ov_open) { SDL_Keycode c = k->keysym.sym;
        ov_input(c == SDLK_UP, c == SDLK_DOWN, c == SDLK_LEFT, c == SDLK_RIGHT || c == SDLK_RETURN, c == SDLK_ESCAPE); return 1; }
    return 0;
}

/* CPU path: nearest scale of one screen into its rectangle of the staging buffer */
static void cpu_blit(const uint8_t *src, int sw, Rect r) {
    if (!r.on) return;
    int x0 = (int)r.x, y0 = (int)r.y, w = (int)r.w, h = (int)r.h;
    for (int y = 0; y < h; y++) {
        int dy = y0 + y; if (dy < 0 || dy >= (int)sc_extent.height) continue;
        const uint8_t *srow = src + (size_t)(y * SCR_H / h) * (size_t)sw * 4;
        uint8_t *drow = stage_ptr + (size_t)dy * sc_extent.width * 4;
        for (int x = 0; x < w; x++) {
            int dx = x0 + x; if (dx < 0 || dx >= (int)sc_extent.width) continue;
            const uint8_t *p = srow + (size_t)(x * sw / w) * 4; uint8_t *d = drow + (size_t)dx * 4;
            if (sc_bgra) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; } else { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; }
            d[3] = 255;
        }
    }
}

/* GPU path: draw a texture into a rectangle of the current attachment (W x H) with a pipeline */
static void gpu_quad_view(VkPipeline pl, VkImageView view, uint32_t tw, uint32_t th, VkImageLayout lay, int rot, Rect r, float W, float H, int mode, int alpha, const float extra[4]) {
    if (!r.on || r.w < 1 || r.h < 1) return;
    Push p = { { r.x / W * 2 - 1, r.y / H * 2 - 1, (r.x + r.w) / W * 2 - 1, (r.y + r.h) / H * 2 - 1 },
               { (float)tw, (float)th, r.w / (float)tw, r.h / (float)th }, { mode, alpha, rot, 0 }, { 0, 0, 0, 0 } };
    if (extra) memcpy(p.extra, extra, sizeof p.extra);
    VkDescriptorImageInfo ii = { smp_linear, view, lay };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    push_desc(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &w);
    vkCmdPushConstants(cmdbuf, play, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}
static void gpu_quad(VkPipeline pl, const Img *t, Rect r, float W, float H, int mode, int alpha, const float extra[4]) {
    gpu_quad_view(pl, t->view, t->w, t->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, r, W, H, mode, alpha, extra);
}
static void begin_pass(const Img *att, VkAttachmentLoadOp load) {
    barrier_img(att->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = att->view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 1 } } } };
    VkExtent2D ex = { att->w, att->h };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, ex }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmdbuf, &ri);
    VkViewport vp = { 0, 0, (float)att->w, (float)att->h, 0, 1 }; VkRect2D sc = { { 0, 0 }, ex };
    vkCmdSetViewport(cmdbuf, 0, 1, &vp); vkCmdSetScissor(cmdbuf, 0, 1, &sc);
}

/* FSR: EASU upscale of a screen into fsr_tmp[i] at the output size (integer rectangle), before the main pass */
static void fsr_upscale(int i, const Img *src, Rect *r) {
    if (!r->on) return;
    r->x = floorf(r->x + 0.5f); r->y = floorf(r->y + 0.5f); r->w = floorf(r->w + 0.5f); r->h = floorf(r->h + 0.5f);
    if (r->w < 1 || r->h < 1) { r->on = 0; return; }
    Img *t = &fsr_tmp[i];
    if (t->w != (uint32_t)r->w || t->h != (uint32_t)r->h) {
        img_free(t);
        if (img_make(t, (uint32_t)r->w, (uint32_t)r->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) { r->on = 0; return; }
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    float ex[4] = { r->w, r->h, 0, 0 };
    gpu_quad(pipe_easu, src, (Rect){ 0, 0, r->w, r->h, 1 }, r->w, r->h, 0, 0, ex);
    vkCmdEndRendering(cmdbuf);
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

/* a screen at the 3D internal resolution: un-rotate pica_vk's scan-out image (framebuffer orientation) into hi_scr[i] */
static const Img *hi_screen(int i, const ScreenSource *s, int screen_w) {
    VkImageView v; uint32_t lw, lh;
    if (!s->valid || (s->format & 7) != 1 || !pica_vk_scanout(s->addr, &v, &lw, &lh) || lw * (uint32_t)screen_w != lh * SCR_H) return NULL;
    Img *t = &hi_scr[i];
    if (t->w != lh || t->h != lw) {
        img_free(t);
        if (img_make(t, lh, lw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return NULL;
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    gpu_quad_view(pipe_rgba, v, lw, lh, VK_IMAGE_LAYOUT_GENERAL, 1, (Rect){ 0, 0, (float)lh, (float)lw, 1 }, (float)lh, (float)lw, 0, 0, NULL);
    vkCmdEndRendering(cmdbuf);
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return t;
}

static void upload(const Img *t, VkDeviceSize off);
/* a UI panel image of exactly w x h (remade when the UI scale changes; the previous frame has finished with it) */
static void ui_img(Img *t, int w, int h) {
    if (t->img && t->w == (uint32_t)w && t->h == (uint32_t)h) return;
    img_free(t);
    if (img_make(t, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) memset(t, 0, sizeof *t);
}
static void upload(const Img *t, VkDeviceSize off) {   /* whole image from up_buf + off */
    barrier_img(t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy cp = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    vkCmdCopyBufferToImage(cmdbuf, up_buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier_img(t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

int video_frame(void) {
    SDL_Event e; int bottom_click = 0;
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    Rect rt, rb; layout_rects(ww, wh, &rt, &rb);   /* window coordinates for touch; the drawing below uses drawable pixels */
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: return 0;
        case SDL_CONTROLLERDEVICEADDED: case SDL_JOYDEVICEADDED: if (!g_input_none) pad_open(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: if (gc && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))) { SDL_GameControllerClose(gc); gc = NULL; input_pad_stick(0, 0); for (int b = 0; b < 12; b++) input_pad_button(b, 0); } break;
        case SDL_JOYDEVICEREMOVED: if (joy && e.jdevice.which == SDL_JoystickInstanceID(joy)) { SDL_JoystickClose(joy); joy = NULL; input_pad_stick(0, 0); for (int b = 0; b < 12; b++) input_pad_button(b, 0); } break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: if (g_input_none) break;
            if (ov_open) { if (e.type == SDL_CONTROLLERBUTTONDOWN) { int c = e.cbutton.button;
                ov_input(c == SDL_CONTROLLER_BUTTON_DPAD_UP, c == SDL_CONTROLLER_BUTTON_DPAD_DOWN, c == SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                         c == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || c == SDL_CONTROLLER_BUTTON_A, c == SDL_CONTROLLER_BUTTON_B); } break; }
            if (opt_select_layout && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) { if (e.type == SDL_CONTROLLERBUTTONDOWN) next_layout(); break; }
            input_pad_button(pad_pos(e.cbutton.button), e.type == SDL_CONTROLLERBUTTONDOWN); break;
        case SDL_CONTROLLERAXISMOTION: if (!g_input_none) {
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) input_pad_button(4, e.caxis.value > 16000);
            else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) input_pad_button(5, e.caxis.value > 16000);
            else pad_axes(); } break;
        case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: if (!joy || g_input_none || e.jbutton.button >= 8) break;
            if (opt_select_layout && e.jbutton.button == 6) { if (e.type == SDL_JOYBUTTONDOWN && !ov_open) next_layout(); break; }   /* raw pads: button 6 = Back/Select */
            if (!ov_open) input_pad_button(e.jbutton.button, e.type == SDL_JOYBUTTONDOWN); break;
        case SDL_JOYAXISMOTION: if (joy && !g_input_none) pad_axes(); break;
        case SDL_JOYHATMOTION: if (joy && !g_input_none) { input_pad_button(8, e.jhat.value & SDL_HAT_UP); input_pad_button(9, e.jhat.value & SDL_HAT_DOWN);
                                                           input_pad_button(10, e.jhat.value & SDL_HAT_LEFT); input_pad_button(11, e.jhat.value & SDL_HAT_RIGHT); } break;
        case SDL_KEYDOWN: case SDL_KEYUP: if (!hotkey(&e.key) && !e.key.repeat) input_key(e.key.keysym.sym, e.type == SDL_KEYDOWN); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
            int down = e.type == SDL_MOUSEMOTION ? (e.motion.state & SDL_BUTTON_LMASK) != 0 : e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
            int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            if (e.type == SDL_MOUSEMOTION && !down) break;
            if (ov_open) break;
            if (!rb.on || rb.w < 1 || rb.h < 1) { input_touch(0, -1, -1); break; }
            int inside = mx >= rb.x && mx < rb.x + rb.w && my >= rb.y && my < rb.y + rb.h;
            if (e.type == SDL_MOUSEBUTTONDOWN && inside) bottom_click = 1;
            input_touch(down, (int)((mx - rb.x) * BOT_W / rb.w), (int)((my - rb.y) * SCR_H / rb.h));
            break; }
        }
    }
    if (opt_layout == L_AUTO) auto_update(now_s(), bottom_click);

    ScreenSource top, bottom;
    gsp_screens(&top, &bottom); decode_screen(&top, TOP_W, top_px); decode_screen(&bottom, BOT_W, bot_px);
    static int test = -1; if (test < 0) test = getenv("PWR_TESTPATTERN") != NULL;
    if (test) for (int y = 0; y < SCR_H; y++) for (int x = 0; x < 50; x++) {   /* colour bars down the left edge of both screens */
        int band = y * 6 / SCR_H; uint8_t *d = top_px + ((size_t)y * TOP_W + (size_t)x) * 4, *d2 = bot_px + ((size_t)y * BOT_W + (size_t)x) * 4;
        d[0] = d2[0] = (band & 1) ? 255 : 0; d[1] = d2[1] = (band & 2) ? 255 : 0; d[2] = d2[2] = (band & 4) ? 255 : 0;
    }

    /* window resized or swapchain lost: rebuild before drawing */
    static int last_dw = -1, last_dh = -1; int dw, dh; SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
    if (last_dw >= 0 && (dw != last_dw || dh != last_dh)) { if (recreate_swapchain()) return 0; }
    last_dw = dw; last_dh = dh;
    if (!swap || !sc_extent.width || !sc_extent.height) return 1;

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g_vid_acquire_fail++; g_vid_last_err = r; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain() ? 0 : 1;
    vkResetFences(dev, 1, &fence);
    layout_rects((int)sc_extent.width, (int)sc_extent.height, &rt, &rb);   /* drawable pixels (differs from window size on HiDPI) */

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmdbuf, 0); vkBeginCommandBuffer(cmdbuf, &bi);
    if (gpu_ok && vk13) pica_vk_scan_lock(1);   /* until submitted: a 3D resolution change must not free a scan-out image in use */
    if (gpu_ok) {
        float W = (float)sc_extent.width, H = (float)sc_extent.height;
        memcpy(up_ptr, top_px, sizeof top_px); memcpy(up_ptr + sizeof top_px, bot_px, sizeof bot_px);
        upload(&tex_top, 0); upload(&tex_bot, sizeof top_px);
        /* UI scales: the panel at the largest whole number that fits 90% of the window, the small labels by window height */
        int kov = (int)fminf(W * 0.9f / OV_W, H * 0.9f / OV_H), ks = (int)(H / 480.0f);
        kov = kov < 1 ? 1 : kov > UI_KMAX ? UI_KMAX : kov; ks = ks < 1 ? 1 : ks > UI_KMAX ? UI_KMAX : ks;
        if (ov_open && (ov_dirty || tex_ov.w != (uint32_t)(OV_W * kov))) {
            ov_render(kov); ui_img(&tex_ov, OV_W * kov, OV_H * kov);
            memcpy(up_ptr + UP_OV, ov_px, (size_t)OV_W * OV_H * 4 * kov * kov); upload(&tex_ov, UP_OV); ov_dirty = 0;
        }
        static double fps_t; static unsigned fps_swaps; static char fps_txt[16];
        if (opt_fps) {   /* game frames per second (top-screen swaps), refreshed twice a second */
            extern unsigned g_gsp_top_swaps; double t = now_s();
            if (t - fps_t >= 0.5 || tex_fps.w != (uint32_t)(FPS_W * ks)) {
                if (t - fps_t >= 0.5) { if (fps_t > 0) snprintf(fps_txt, sizeof fps_txt, "%.0f fps", (g_gsp_top_swaps - fps_swaps) / (t - fps_t));
                                        fps_t = t; fps_swaps = g_gsp_top_swaps; }
                int w = FPS_W * ks, h = FPS_H * ks; ui_img(&tex_fps, w, h);
                fill(fps_px, w, 0, 0, w, h, 0x000000, 150); text(fps_px, w, h, 4 * ks, ks, fps_txt, 0x7cfc00, ks);
                memcpy(up_ptr + UP_FPS, fps_px, (size_t)w * h * 4); upload(&tex_fps, UP_FPS); }
        }
        /* screens at the 3D internal resolution when pica_vk kept them (resolution > 1x); the scan-out copies were submitted earlier */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        const Img *st = hi_screen(0, &top, TOP_W), *sb = hi_screen(1, &bottom, BOT_W);
        if (!st) st = &tex_top; if (!sb) sb = &tex_bot;
        if (toast_text && (toast_dirty || tex_toast.w != (uint32_t)(TOAST_W * ks))) {
            int w = TOAST_W * ks, h = TOAST_H * ks; ui_img(&tex_toast, w, h);
            fill(toast_px, w, 0, 0, w, h, 0x101820, 210);
            text(toast_px, w, h, (w - 8 * ks * (int)strlen(toast_text)) / 2, 3 * ks, toast_text, 0xffffff, ks);
            memcpy(up_ptr + UP_TOAST, toast_px, (size_t)w * h * 4); upload(&tex_toast, UP_TOAST); toast_dirty = 0;
        }
        int fsr = opt_filter == F_FSR;
        if (fsr) { fsr_upscale(0, st, &rt); fsr_upscale(1, sb, &rb); }
        begin_pass(&target, VK_ATTACHMENT_LOAD_OP_CLEAR);
        if (fsr) {
            float et[4] = { opt_sharp, 0, rt.x, rt.y }, eb[4] = { opt_sharp, 0, rb.x, rb.y };
            if (rt.on) gpu_quad(pipe_rcas, &fsr_tmp[0], rt, W, H, 0, 0, et);
            if (rb.on) gpu_quad(pipe_rcas, &fsr_tmp[1], rb, W, H, 0, 0, eb);
        } else {
            int f = opt_filter == F_NEAREST ? 0 : opt_filter == F_SHARP ? 2 : opt_filter == F_FXAA ? 4 : 1;
            gpu_quad(pipe, st, rt, W, H, f, 0, NULL); gpu_quad(pipe, sb, rb, W, H, f, 0, NULL);
        }
        if (ov_open && tex_ov.img) {   /* panels pixel for pixel (nearest) */
            Rect ro = { floorf((W - (float)tex_ov.w) / 2), floorf((H - (float)tex_ov.h) / 2), (float)tex_ov.w, (float)tex_ov.h, 1 };
            gpu_quad(pipe, &tex_ov, ro, W, H, 0, 1, NULL);
        }
        if (!ov_open && now_s() < toast_until && tex_toast.img)   /* layout label, top centre */
            gpu_quad(pipe, &tex_toast, (Rect){ floorf((W - (float)tex_toast.w) / 2), 12, (float)tex_toast.w, (float)tex_toast.h, 1 }, W, H, 0, 1, NULL);
        if (!ov_open && opt_fps && fps_txt[0] && tex_fps.img)
            gpu_quad(pipe, &tex_fps, (Rect){ 8, 8, (float)tex_fps.w, (float)tex_fps.h, 1 }, W, H, 0, 1, NULL);
        vkCmdEndRendering(cmdbuf);
        barrier_img(target.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .extent = { sc_extent.width, sc_extent.height, 1 } };
        vkCmdCopyImage(cmdbuf, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    } else {
        memset(stage_ptr, 0, (size_t)stage_size);
        cpu_blit(top_px, TOP_W, rt); cpu_blit(bot_px, BOT_W, rb);
        barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sc_extent.width, sc_extent.height, 1 } };
        vkCmdCopyBufferToImage(cmdbuf, stage, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmdbuf);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire, .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cmdbuf, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_done[idx] };
    pthread_mutex_lock(&queue_lock);
    vkQueueSubmit(queue, 1, &sub, fence);
    if (gpu_ok && vk13) pica_vk_scan_lock(0);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_done[idx], .swapchainCount = 1, .pSwapchains = &swap, .pImageIndices = &idx };
    r = vkQueuePresentKHR(queue, &pi);
    pthread_mutex_unlock(&queue_lock);
    if (r != VK_SUCCESS) { g_vid_present_fail++; g_vid_last_err = r; } else g_vid_presented++;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    return 1;
}

int video_vk(VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *family) {
    if (!dev || !vk13) return -1;
    *p = phys; *d = dev; *q = queue; *family = qfam; return 0;
}
void video_queue_lock(void) { pthread_mutex_lock(&queue_lock); }
void video_queue_unlock(void) { pthread_mutex_unlock(&queue_lock); }

void video_shutdown(void) {
    if (dev) { pthread_mutex_lock(&queue_lock); vkDeviceWaitIdle(dev); pthread_mutex_unlock(&queue_lock); }
    SDL_Quit();
}
