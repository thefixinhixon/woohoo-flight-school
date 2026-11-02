/* pwr: boots the recompiled game.
 * usage: pwr [options] [<extracted dir>]
 *   <extracted dir> defaults to ./extracted (code.bin, exheader.bin, romfs.bin: tools/pipeline.py build, or tools/cia.py)
 *   --input=none  never open real input devices (headless runs)    --mute  no audio output
 *   --scale=N     initial window scale (default 2)
 *   --layout=stacked|side|large|top|bottom|auto  --auto-gameplay=large|top  --aspect=keep|integer|stretch
 *   --fullscreen=off|borderless|exclusive         --filter=nearest|linear|sharp|fsr|fxaa      (video.c)
 * hotkeys: F11 / Alt+Enter fullscreen, F9 next layout
 * Save data goes to $PWR_SAVE_DIR (default ./save).
 * env:   PWR_TRACE=1 logs kernel calls and IPC.
 * The guest main thread runs on its own host thread; this thread owns the window and drives VBlank at 60 Hz. */
#include "gpu.h"
#include "runtime.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

void f_00100000(CPU *c);   /* ExeFS entry point */

int g_input_none, g_mute;
extern const char *g_data_dir;

/* $PWR_SETTINGS (the profile's settings.json, which the F1 overlay writes) as starting options; the command line, which
   comes after, wins. Flat JSON: "key": value pairs; the display options plus scale and mute. */
static void settings_file(int *scale) {
    const char *path = getenv("PWR_SETTINGS"); FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char buf[8192]; size_t len = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[len] = 0;
    for (char *c = buf; *c; ) {
        char *k = strchr(c, '"'); if (!k) break; char *ke = strchr(k + 1, '"'); if (!ke) break;
        char *v = strchr(ke, ':'); if (!v) break; v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        char *ve = v; if (*v == '"') { v++; ve = strchr(v, '"'); if (!ve) break; } else while (*ve && !strchr(",}\r\n", *ve)) ve++;
        char name[32], val[64];
        snprintf(name, sizeof name, "%.*s", (int)(ke - k - 1), k + 1); snprintf(val, sizeof val, "%.*s", (int)(ve - v), v);
        if (!strcmp(name, "scale")) { if (atoi(val) > 0) *scale = atoi(val); }
        else if (!strcmp(name, "mute")) { if (!strcmp(val, "true")) g_mute = 1; }
        else video_option(name, val);   /* unknown keys (args, ...) are ignored */
        c = *ve == '"' ? ve + 1 : ve;
    }
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    const char *dir = "extracted"; int scale = 2;
    settings_file(&scale);
#ifdef _WIN32
    timeBeginPeriod(1);   /* sleeps (the 60 Hz loop, the DSP clock's ~5 ms ticks) otherwise round up to the 15.6 ms tick */
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input=none")) g_input_none = 1;
        else if (!strcmp(argv[i], "--mute")) g_mute = 1;
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8) > 0 ? atoi(argv[i] + 8) : 2;
        else if (!strncmp(argv[i], "--", 2) && strchr(argv[i], '=')) {   /* display options: --name=value -> video_option */
            char name[32]; const char *eq = strchr(argv[i], '='); size_t n = (size_t)(eq - argv[i] - 2);
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, argv[i] + 2, n); name[n] = 0; for (char *c = name; *c; c++) if (*c == '-') *c = '_';
            if (video_option(name, eq + 1)) { fprintf(stderr, "pwr: unknown option or value %s\n", argv[i]); return 2; }
        }
        else if (!strncmp(argv[i], "--", 2)) { fprintf(stderr, "pwr: unknown option %s\n", argv[i]); return 2; }
        else dir = argv[i];
    }
    g_data_dir = dir;
    char path[4096];
    snprintf(path, sizeof path, "%s/code.bin", dir);
    if (rt_mem_init()) { fprintf(stderr, "pwr: cannot reserve guest address space\n"); return 1; }
    if (rt_load_image(path, 0x100000)) { fprintf(stderr, "pwr: cannot load %s\n", path); return 1; }
    { extern void drawdist_apply(void); drawdist_apply(); }
    k_init();
    snprintf(path, sizeof path, "%s/exheader.bin", dir);
    { FILE *f = fopen(path, "rb"); uint8_t pr; if (f && !fseek(f, 0x20F, SEEK_SET) && fread(&pr, 1, 1, f) == 1) g_main_prio = pr; if (f) fclose(f); }
    if (video_init(scale)) { fprintf(stderr, "pwr: video init failed\n"); return 1; }
    fprintf(stderr, "pwr: boot (input=%s, audio=%s)\n", g_input_none ? "none" : "default", g_mute ? "muted" : "on");

    volatile int *done = k_start_main(f_00100000);
    double next = now_s(), last_stat = next; unsigned frames = 0;
    while (!*done) {
        input_update();
        gsp_vblank();
        if (!video_frame()) break;
        frames++;
        next += 1.0 / 60.0;
        double t = now_s();
        if (next > t) { struct timespec ts = { 0, (long)((next - t) * 1e9) }; nanosleep(&ts, NULL); }
        else if (t - next > 0.25) next = t;          /* fell behind: don't try to catch up */
        if (t - last_stat >= 2.0) {
            uint32_t lists, draws; pica_stats(&lists, &draws);
            extern unsigned g_vid_acquire_fail, g_vid_present_fail, g_vid_presented; extern int g_vid_last_err;
            extern unsigned g_gsp_top_swaps; static unsigned last_swaps;
            fprintf(stderr, "pwr: frames=%u gpu-lists=%u draws=%u presented=%u acquire-fail=%u present-fail=%u last-err=%d game-fps=%.1f\n",
                    frames, lists, draws, g_vid_presented, g_vid_acquire_fail, g_vid_present_fail, g_vid_last_err, (g_gsp_top_swaps - last_swaps) / (t - last_stat));
            {   /* no new top-screen frame for 6 s after the game had started drawing: say where everything waits, once */
                static unsigned still; static int reported;
                if (g_gsp_top_swaps == last_swaps && g_gsp_top_swaps) still++; else still = 0;
                if (still == 3 && !reported) { reported = 1; fprintf(stderr, "hang: no game frame for 6 s\n"); k_hang_report(); gsp_hang_report(); }
                static double t0; if (!t0) t0 = t;   /* PWR_HANG_AT=<s>: the same report at that run time, frames or not */
                if (getenv("PWR_HANG_AT") && !reported && t - t0 >= atof(getenv("PWR_HANG_AT"))) {
                    reported = 1; fprintf(stderr, "hang: report at %.0f s (PWR_HANG_AT)\n", t - t0); k_hang_report(); gsp_hang_report();
                }
            }
            last_swaps = g_gsp_top_swaps;
            { extern void dsp_stats(void); dsp_stats(); }
            if (getenv("PWR_PEEK")) {   /* PWR_PEEK=<hex guest addr>[,<words>]: print guest memory every stats interval */
                unsigned pa = 0, pn = 8; sscanf(getenv("PWR_PEEK"), "%x,%u", &pa, &pn); fprintf(stderr, "peek %08x:", pa);
                for (unsigned i = 0; i < pn && i < 512; i++) fprintf(stderr, " %08x", rd32(pa + 4 * i));
                fprintf(stderr, "\n");
            }
            extern unsigned long long g_sw_tri_in, g_sw_tri_drawn, g_sw_px_cov, g_sw_px_zfail, g_sw_px_afail, g_sw_px_written;
            ScreenSource st, sb; gsp_screens(&st, &sb);
            extern unsigned long long g_sw_clip_plane[7];
            fprintf(stderr, "pwr: clipped by plane x<w %llu x>-w %llu y<w %llu y>-w %llu z<0 %llu z>-w %llu w>0 %llu\n", g_sw_clip_plane[0], g_sw_clip_plane[1], g_sw_clip_plane[2], g_sw_clip_plane[3], g_sw_clip_plane[4], g_sw_clip_plane[5], g_sw_clip_plane[6]);
            fprintf(stderr, "pwr: sw tris=%llu drawn=%llu px covered=%llu zfail=%llu afail=%llu written=%llu | top %08x fmt%u bottom %08x fmt%u\n",
                    g_sw_tri_in, g_sw_tri_drawn, g_sw_px_cov, g_sw_px_zfail, g_sw_px_afail, g_sw_px_written, st.addr, st.format, sb.addr, sb.format);
            extern unsigned long long g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vk_passes, g_vk_gpu_us, g_vk_wait_us, g_vk_flush_us, g_vk_verts, g_vk_idx;
            extern unsigned g_vsgen_compiled, g_vsgen_failed;
            extern int pica_vk_npipes(void);
            fprintf(stderr, "pwr: vk draws=%llu batches=%llu target-uploads=%llu tex-uploads=%llu vs-compiled=%u vs-failed=%u passes=%llu gpu-ms=%llu fence-wait-ms=%llu verts=%llu idx=%llu pipelines=%d\n", g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vsgen_compiled, g_vsgen_failed, g_vk_passes, g_vk_gpu_us / 1000, g_vk_wait_us / 1000, g_vk_verts, g_vk_idx, pica_vk_npipes());
            { extern unsigned long long g_gs_draws; if (g_gs_draws) fprintf(stderr, "pwr: geometry-shader draws=%llu (not implemented)\n", g_gs_draws); }
            last_stat = t;
        }
    }
    fprintf(stderr, "pwr: %s\n", *done ? "guest main returned" : "window closed");
    video_shutdown();
    return 0;
}
