#ifdef TARGET_PS2

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#include <kernel.h>
#include <gsKit.h>
#include <dmaKit.h>

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "gfx_ps2.h"

struct VidMode {
    const char *name;
    s16 mode;
    s16 interlace;
    s16 field;
    int max_width;  // framebuffer size that fills the whole screen
    int max_height;
    int width;      // framebuffer size actually used
    int height;
    int vck;
    int x_off;
    int y_off;
};

static const struct VidMode vid_modes[] = {
    // NTSC
    { "480i", GS_MODE_NTSC,      GS_INTERLACED,    GS_FIELD,  640,  480,  640,  452, 4, 0, 0 },
    { "480p", GS_MODE_DTV_480P,  GS_NONINTERLACED, GS_FRAME,  640,  480,  640,  452, 2, 0, 0 },
    // PAL
    { "576i", GS_MODE_PAL,       GS_INTERLACED,    GS_FIELD,  640,  576,  640,  536, 4, 0, 0 },
    { "576p", GS_MODE_DTV_576P,  GS_NONINTERLACED, GS_FRAME,  640,  576,  640,  536, 2, 0, 0 },
    // HDTV
    { "720p", GS_MODE_DTV_720P,  GS_NONINTERLACED, GS_FRAME, 1280,  720, 1280,  698, 1, 0, 0 },
    {"1080i", GS_MODE_DTV_1080I, GS_INTERLACED,    GS_FIELD, 1920, 1080, 1920, 1080, 1, 0, 0 },
};

GSGLOBAL *gs_global;
float gfx_ps2_aspect_ratio = 4.f / 3.f;

/* SM64 runs at 30 FPS, so each frame lasts 2 vsyncs */
#define VSYNCS_PER_FRAME 2

static int vsync_sema_id = -1;
static volatile u32 vsync_count;
static u32 next_vsync;

static const struct VidMode *vid_mode;

/* Waits until the vsync this frame is due at. If we're already late (e.g. the
   game loop was blocked by a memcard write), don't try to catch up by running
   frames back to back; just present now and count the next frame from here. */
static void wait_vsync(void)
{
    // the semaphore may have piled up signals while we weren't waiting, drop them
    while (PollSema(vsync_sema_id) >= 0) {}

    if ((s32)(vsync_count - next_vsync) > 0)
        next_vsync = vsync_count;

    while ((s32)(next_vsync - vsync_count) > 0)
        WaitSema(vsync_sema_id);

    next_vsync += VSYNCS_PER_FRAME;
}

/* Copy of gsKit_sync_flip, but without the 'sync' */
static void gsKit_flip(GSGLOBAL *gsGlobal)
{
   if (!gsGlobal->FirstFrame)
   {
      if (gsGlobal->DoubleBuffering == GS_SETTING_ON)
      {
         GS_SET_DISPFB2( gsGlobal->ScreenBuffer[
               gsGlobal->ActiveBuffer & 1] / 8192,
               gsGlobal->Width / 64, gsGlobal->PSM, 0, 0 );

         gsGlobal->ActiveBuffer ^= 1;
      }

   }

   gsKit_setactive(gsGlobal);
}

/* PRIVATE METHODS */
static int vsync_handler()
{
   vsync_count++;
   iSignalSema(vsync_sema_id);

   ExitHandler();
   return 0;
}

static void prepare_sema() {
    ee_sema_t sema;
    sema.init_count = 0;
    sema.max_count = 1;
    sema.option = 0;
    vsync_sema_id = CreateSema(&sema);
}

static void gfx_ps2_init(const char *game_name, bool start_in_fullscreen) {
    gs_global = gsKit_init_global();

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);

    dmaKit_chan_init(DMA_CHANNEL_GIF);

#if defined(VERSION_EU)
    vid_mode = &vid_modes[2]; // PAL
#else
    vid_mode = &vid_modes[0]; // NTCS
#endif

    // framebuffer pixels aren't square: derive the aspect ratio from the size that fills the screen
    const float screen_aspect = (vid_mode->max_height >= 720) ? 16.f / 9.f : 4.f / 3.f;
    gfx_ps2_aspect_ratio = screen_aspect * ((float)vid_mode->width / vid_mode->max_width)
                                         * ((float)vid_mode->max_height / vid_mode->height);

    gs_global->Mode = vid_mode->mode;
    gs_global->Width = vid_mode->width;
    gs_global->Height = vid_mode->height;
    gs_global->Interlace = vid_mode->interlace;
    gs_global->Field = vid_mode->field;
    gs_global->ZBuffering = GS_SETTING_ON;
    gs_global->DoubleBuffering = GS_SETTING_ON;
    gs_global->PrimAAEnable = GS_SETTING_OFF;
    gs_global->PSM = GS_PSM_CT24;
    gs_global->PSMZ = GS_PSMZ_16; // 16-bit unsigned zbuffer

    gsKit_init_screen(gs_global);
    gsKit_TexManager_init(gs_global);
}

static void gfx_ps2_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {

}

static void gfx_ps2_set_fullscreen(bool enable) {

}

static void gfx_ps2_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode), void (*on_all_keys_up)(void)) {

}

static void gfx_ps2_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void gfx_ps2_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = gs_global->Width;
    *height = gs_global->Height;
}

static void gfx_ps2_handle_events(void) {

}

static bool gfx_ps2_start_frame(void) {
    return 1;
}

static void gfx_ps2_swap_buffers_begin(void) {
    if (vsync_sema_id != -1) return;

    prepare_sema();
    gsKit_add_vsync_handler(vsync_handler);
}

static void gfx_ps2_swap_buffers_end(void) {
    wait_vsync();

    gsKit_flip(gs_global);
    gsKit_queue_exec(gs_global);
    gsKit_TexManager_nextFrame(gs_global);
}

static double gfx_ps2_get_time(void) {
    return 0.0;
}

struct GfxWindowManagerAPI gfx_ps2_wapi = {
    gfx_ps2_init,
    gfx_ps2_set_keyboard_callbacks,
    gfx_ps2_set_fullscreen_changed_callback,
    gfx_ps2_set_fullscreen,
    gfx_ps2_main_loop,
    gfx_ps2_get_dimensions,
    gfx_ps2_handle_events,
    gfx_ps2_start_frame,
    gfx_ps2_swap_buffers_begin,
    gfx_ps2_swap_buffers_end,
    gfx_ps2_get_time
};

#endif // TARGET_PS2
