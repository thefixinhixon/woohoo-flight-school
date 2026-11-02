/* GPU side of the runtime: gsp::Gpu service state, GX commands, PICA command lists, presentation. */
#ifndef PWR_GPU_H
#define PWR_GPU_H
#include "kernel.h"

/* physical <-> guest virtual (old-3DS application layout) */
static inline uint32_t pa_to_va(uint32_t pa) {
    if (pa >= 0x18000000u && pa < 0x18600000u) return pa - 0x18000000u + VA_VRAM;     /* VRAM */
    if (pa >= 0x20000000u && pa < 0x28000000u) return pa - 0x20000000u + VA_LINEAR;   /* FCRAM, linear heap */
    return 0;
}

enum { GSP_IRQ_PSC0, GSP_IRQ_PSC1, GSP_IRQ_VBLANK_TOP, GSP_IRQ_VBLANK_BOTTOM, GSP_IRQ_PPF, GSP_IRQ_P3D, GSP_IRQ_DMA };

/* one screen as the LCD scans it out; filled by gsp_vblank() */
typedef struct { uint32_t addr, stride, format; int valid; } ScreenSource;

void gsp_hang_report(void);                                     /* GX/interrupt queue state (stderr), with k_hang_report */
void gsp_vblank(void);                                         /* host 60 Hz tick: framebuffer swaps + VBlank irqs */
int  gsp_screens(ScreenSource *top, ScreenSource *bottom);     /* current scan-out sources; 0 before any setup */
void gsp_handle(KThread *t, uint32_t *cmd, KObject *s);

void pica_run(uint32_t va, uint32_t size);                     /* execute one PICA200 command list */
void pica_run_buf(const uint8_t *buf, uint32_t va, uint32_t size);   /* same, from a copy of the list */
void pica_stats(uint32_t *lists, uint32_t *draws);

/* video.c */
/* display options (main.c command line; also changed at run time by hotkeys / the settings overlay):
   layout: stacked, side, large, top, bottom, auto    fullscreen: off, borderless, exclusive    filter: nearest, linear, sharp, fsr, fxaa
   aspect: keep, integer, stretch                      auto_gameplay: the layout auto mode uses while flying (large or top)
   fsr_sharpness: 0 (most) .. 2 stops    resolution: 3D internal resolution 1..4    aniso: 1, 2, 4, 8, 16    show_fps: 0/1
   select_layout: 0/1 (pad Back/Select cycles layouts)    free_look: 0/1 (right stick orbits the chase camera, freelook.c) */
int  video_option(const char *name, const char *value);   /* 0 ok, -1 unknown name or value */
void pica_vk_quality(int resolution, int aniso);          /* pica_vk.c: internal resolution, anisotropic filtering */
void pica_vk_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);   /* gsp.c: keep the
                                                           internal-resolution image of a display target for video.c */
void pica_vk_scanout_invalidate(uint32_t start, uint32_t end);   /* gsp.c: a GX operation wrote these guest bytes */
int  video_init(int scale);
int  video_frame(void);                                        /* present current screens; 0 when the window closed */
void video_shutdown(void);
#endif
