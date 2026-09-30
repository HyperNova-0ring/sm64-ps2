#ifdef TARGET_PS2

#include <ultra64.h>

#include "sm64.h"
#include "gfx_dimensions.h"
#include "game/game_init.h"
#include "game/geo_misc.h"
#include "game/memory.h"
#include "game/print.h"
#include "game/save_file.h"
#include "game/segment2.h"
#include "game/sound_init.h"

#include "ps2_memcard.h"
#include "ps2_memcard_ui.h"

#ifdef VERSION_EU
#define UI_FPS 25
#else
#define UI_FPS 30
#endif

#define BOOT_WARNING_FRAMES (5 * UI_FPS)
#define ICON_MIN_FRAMES     (2 * UI_FPS)
#define BOOT_FADE_FRAMES    (UI_FPS / 2)
#define BOOT_GAP_FRAMES     (1 * UI_FPS)
#define ICON_FADE_FRAMES    (UI_FPS / 5)
#define ICON_FADE_STEP      ((255 + ICON_FADE_FRAMES - 1) / ICON_FADE_FRAMES)
#define RESCAN_FADE_FRAMES  (UI_FPS / 5)

#define ICON_SIZE 32
// bottom left corner, lined up with the camera HUD on the right
#define ICON_X GFX_DIMENSIONS_RECT_FROM_LEFT_EDGE(22)
#define ICON_Y (221 - ICON_SIZE)

// colorful HUD font: 16x16 glyphs; the US ROM lacks J, Q, V, X, Z and punctuation
#define TEXT_LINE_HEIGHT 18
#define TEXT_TOP_Y       200

enum HudIcon {
    HUD_HIDDEN,
    HUD_SAVING,
    HUD_ERROR,
};

enum BootState {
    BOOT_WARNING,
    BOOT_GAP, // black screen between the warning and the card detection
    BOOT_CHECK,
    BOOT_DONE,
};

enum CheckState {
    CHECK_SEARCH,
    CHECK_RESULT,
    CHECK_DONE,
};

enum RescanState {
    RESCAN_IDLE,
    RESCAN_FADE_OUT,
    RESCAN_CHECK,
    RESCAN_RESTART, // waiting for the menu to reload
};

static const u8 icon_intro[] __attribute__((aligned(8))) = {
#include "ps2/memcard/mc_intro.rgba16.inc.c"
};
static const u8 icon_search[] __attribute__((aligned(8))) = {
#include "ps2/memcard/mc_search.rgba16.inc.c"
};
static const u8 icon_loading[] __attribute__((aligned(8))) = {
#include "ps2/memcard/mc_upload.rgba16.inc.c"
};
static const u8 icon_saving[] __attribute__((aligned(8))) = {
#include "ps2/memcard/mc_download.rgba16.inc.c"
};
static const u8 icon_error[] __attribute__((aligned(8))) = {
#include "ps2/memcard/mc_error.rgba16.inc.c"
};

static const char *warning_text[] = {
    "WHILE THIS ICON IS SHOWN",
    "DO NOT TAKE OUT THE",
    "MEMORY CARD OR TURN",
    "OFF THE CONSOLE",
};

static Vp ui_viewport = { {
    { SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2, G_MAXZ / 2, 0 },
    { SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2, G_MAXZ / 2, 0 },
} };

static enum BootState boot_state = BOOT_WARNING;
static u32 boot_timer;
static bool boot_started;
// gGlobalTimer when the boot screen started; restored at the end since the game
// uses it to time the intro (e.g. the Mario head greeting)
static u32 boot_global_timer;

// card detection shown as icons: search, then the result; used at boot and by the menu rescan
static struct {
    enum CheckState state;
    u32 timer;
    bool leaving; // the current icon is fading out
    u32 leave_timer;
    const u8 *result_icon;
} check;

static enum RescanState rescan_state = RESCAN_IDLE;
static u32 rescan_timer;

static enum HudIcon hud_state = HUD_HIDDEN;
static const u8 *hud_tex;
static u32 hud_frames; // frames since the current icon was set, for the minimum display time
static s32 hud_alpha;
static bool hud_leaving;
static u32 hud_seq; // last memcard result already handled

/* text */

// y is measured from the bottom of the screen, like print_text
static void render_centered_lines(const char **lines, const s32 count, s32 y) {
    for (s32 i = 0; i < count; ++i, y -= TEXT_LINE_HEIGHT)
        print_text_centered(SCREEN_WIDTH / 2, y, lines[i]);
    render_text_labels();
}

/* icons */

static void load_icon_texture(const u8 *tex) {
    gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, tex);
    gDPSetTile(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0,
               G_TX_WRAP | G_TX_NOMIRROR, 5, G_TX_NOLOD, G_TX_WRAP | G_TX_NOMIRROR, 5, G_TX_NOLOD);
    gDPLoadSync(gDisplayListHead++);
    gDPLoadBlock(gDisplayListHead++, G_TX_LOADTILE, 0, 0, ICON_SIZE * ICON_SIZE - 1,
                 CALC_DXT(ICON_SIZE, G_IM_SIZ_16b_BYTES));
    gDPSetTile(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, (ICON_SIZE * G_IM_SIZ_16b_BYTES) / 8, 0,
               G_TX_RENDERTILE, 0, G_TX_CLAMP, 5, G_TX_NOLOD, G_TX_CLAMP, 5, G_TX_NOLOD);
    gDPSetTileSize(gDisplayListHead++, G_TX_RENDERTILE, 0, 0,
                   (ICON_SIZE - 1) << G_TEXTURE_IMAGE_FRAC, (ICON_SIZE - 1) << G_TEXTURE_IMAGE_FRAC);
}

// opaque icon, copy mode like the HUD glyphs
static void render_icon(const s32 x, const s32 y, const u8 *tex) {
    gSPDisplayList(gDisplayListHead++, dl_hud_img_begin);
    load_icon_texture(tex);
    gSPTextureRectangle(gDisplayListHead++, x << 2, y << 2, (x + ICON_SIZE - 1) << 2, (y + ICON_SIZE - 1) << 2,
                        G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
    gSPDisplayList(gDisplayListHead++, dl_hud_img_end);
}

// translucent icon: copy mode ignores alpha, so draw in 1-cycle mode with the
// texture modulated by a white primitive color carrying the alpha
static void render_icon_alpha(const s32 x, const s32 y, const u8 *tex, const u8 alpha) {
    if (alpha == 0) return;

    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_NONE);
    gDPSetTextureFilter(gDisplayListHead++, G_TF_POINT);
    gDPSetRenderMode(gDisplayListHead++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATERGBA_PRIM, G_CC_MODULATERGBA_PRIM);
    gDPSetPrimColor(gDisplayListHead++, 0, 0, 255, 255, 255, alpha);
    gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);

    load_icon_texture(tex);
    gSPTextureRectangle(gDisplayListHead++, x << 2, y << 2, (x + ICON_SIZE) << 2, (y + ICON_SIZE) << 2,
                        G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);

    gDPPipeSync(gDisplayListHead++);
    gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_PERSP);
    gDPSetTextureFilter(gDisplayListHead++, G_TF_BILERP);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
}

// full screen black quad, drawn like the game's own fade transitions
static void render_black_overlay(const u8 alpha) {
    if (alpha == 0) return;

    Vtx *verts = alloc_display_list(4 * sizeof(*verts));
    if (verts == NULL) return;

    make_vertex(verts, 0, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 0, -1, 0, 0, 0, 0, 0, alpha);
    make_vertex(verts, 1, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), 0, -1, 0, 0, 0, 0, 0, alpha);
    make_vertex(verts, 2, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, 0, 0, 0, alpha);
    make_vertex(verts, 3, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, 0, 0, 0, alpha);

    gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
    gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
    gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
}

/* card detection */

static const u8 *result_icon(const enum Ps2McResult res) {
    switch (res) {
        case PS2_MC_RES_LOADED:  return icon_loading;
        case PS2_MC_RES_CREATED: return icon_saving;
        default:                 return icon_error;
    }
}

static void check_set_state(const enum CheckState state) {
    check.state = state;
    check.timer = 0;
    check.leaving = false;
    check.leave_timer = 0;
}

static void check_start(void) {
    ps2_memcard_boot_start();
    check_set_state(CHECK_SEARCH);
}

// returns true once the result icon has faded out
static bool check_update(void) {
    check.timer++;

    switch (check.state) {
        case CHECK_SEARCH:
            if (check.leaving) {
                if (++check.leave_timer >= ICON_FADE_FRAMES)
                    check_set_state(CHECK_RESULT);
            } else if (check.timer >= ICON_MIN_FRAMES && ps2_memcard_boot_done()) {
                check.result_icon = result_icon(ps2_memcard_boot_wait());
                // the save data is only valid now; this may queue writes for invalid slots
                save_file_load_all();
                set_sound_mode(save_file_get_sound_mode());
                check.leaving = true;
            }
            break;

        case CHECK_RESULT:
            if (check.leaving) {
                if (++check.leave_timer >= ICON_FADE_FRAMES) {
                    check_set_state(CHECK_DONE);
                    // these results were already shown, keep them off the gameplay icon
                    hud_seq = ps2_memcard_last_result(NULL);
                    hud_state = HUD_HIDDEN;
                    hud_alpha = 0;
                }
            } else if (check.timer >= ICON_MIN_FRAMES && ps2_memcard_activity() == PS2_MC_IDLE) {
                // also waited for any write queued by save_file_load_all
                check.leaving = true;
            }
            break;

        default:
            break;
    }

    return check.state == CHECK_DONE;
}

// fades in when the current state starts and out while leaving
static void check_render(void) {
    if (check.state == CHECK_DONE) return;

    u32 alpha = (check.timer >= ICON_FADE_FRAMES) ? 255 : check.timer * 255 / ICON_FADE_FRAMES;
    if (check.leaving) {
        const u32 out = (check.leave_timer >= ICON_FADE_FRAMES) ? 0 : 255 - check.leave_timer * 255 / ICON_FADE_FRAMES;
        if (out < alpha) alpha = out;
    }

    render_icon_alpha(ICON_X, ICON_Y, (check.state == CHECK_SEARCH) ? icon_search : check.result_icon, alpha);
}

/* boot screen */

static u8 warning_fade_alpha(void) {
    const u32 fade_out_start = BOOT_WARNING_FRAMES - BOOT_FADE_FRAMES;
    u32 alpha = 0;
    if (boot_timer < BOOT_FADE_FRAMES)
        alpha = 255 - boot_timer * 255 / BOOT_FADE_FRAMES;
    else if (boot_timer > fade_out_start)
        alpha = (boot_timer - fade_out_start) * 255 / BOOT_FADE_FRAMES;
    return (alpha > 255) ? 255 : alpha;
}

static void boot_render(void) {
    config_gfx_pool();
    init_render_image();
    clear_frame_buffer(0);

    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&ui_viewport));

    switch (boot_state) {
        case BOOT_WARNING:
            render_centered_lines(warning_text, sizeof(warning_text) / sizeof(warning_text[0]), TEXT_TOP_Y);
            render_icon(SCREEN_WIDTH / 2 - ICON_SIZE / 2, ICON_Y, icon_intro);
            render_black_overlay(warning_fade_alpha());
            break;
        case BOOT_CHECK:
            check_render();
            break;
        default:
            break;
    }

    end_master_display_list();
    display_and_vsync();
}

static void boot_set_state(const enum BootState state) {
    boot_state = state;
    boot_timer = 0;
}

bool ps2_memcard_ui_boot_frame(void) {
    if (boot_state == BOOT_DONE) return false;

    if (!boot_started) {
        boot_started = true;
        boot_global_timer = gGlobalTimer;
    }

    boot_timer++;

    switch (boot_state) {
        case BOOT_WARNING:
            if (boot_timer >= BOOT_WARNING_FRAMES)
                boot_set_state(BOOT_GAP);
            break;

        case BOOT_GAP:
            if (boot_timer >= BOOT_GAP_FRAMES) {
                check_start();
                boot_set_state(BOOT_CHECK);
            }
            break;

        case BOOT_CHECK:
            if (check_update()) {
                boot_set_state(BOOT_DONE);
                gGlobalTimer = boot_global_timer;
                return false;
            }
            break;

        default:
            break;
    }

    boot_render();
    return true;
}

/* file select rescan */

// Z held, then X (mapped to A) pressed; the press is consumed so the menu doesn't click
static bool rescan_combo_pressed(void) {
    if (!(gPlayer1Controller->buttonDown & Z_TRIG) || !(gPlayer1Controller->buttonPressed & A_BUTTON))
        return false;

    gPlayer1Controller->buttonPressed &= ~(A_BUTTON | Z_TRIG);
    gPlayer3Controller->buttonPressed &= ~(A_BUTTON | Z_TRIG);
    return true;
}

enum Ps2MenuState ps2_memcard_ui_menu_update(const bool can_rescan) {
    switch (rescan_state) {
        case RESCAN_IDLE:
            if (can_rescan && rescan_combo_pressed()) {
                rescan_state = RESCAN_FADE_OUT;
                rescan_timer = 0;
                return PS2_MENU_BUSY;
            }
            return PS2_MENU_RUN;

        case RESCAN_FADE_OUT:
            if (++rescan_timer >= RESCAN_FADE_FRAMES) {
                check_start();
                rescan_state = RESCAN_CHECK;
            }
            return PS2_MENU_BUSY;

        case RESCAN_CHECK:
            if (check_update()) {
                rescan_state = RESCAN_RESTART;
                return PS2_MENU_RESTART;
            }
            return PS2_MENU_BUSY;

        case RESCAN_RESTART:
        default:
            // called again from the reloaded menu, which fades in from black on its own
            rescan_state = RESCAN_IDLE;
            return PS2_MENU_RUN;
    }
}

static void rescan_render(void) {
    u32 alpha = 255;
    if (rescan_state == RESCAN_FADE_OUT)
        alpha = rescan_timer * 255 / RESCAN_FADE_FRAMES;

    render_black_overlay(alpha > 255 ? 255 : alpha);
    if (rescan_state == RESCAN_CHECK)
        check_render();
}

/* gameplay icon */

static void hud_show(const enum HudIcon state, const u8 *tex) {
    hud_state = state;
    hud_tex = tex;
    hud_frames = 0;
    hud_leaving = false;
}

void ps2_memcard_ui_render(void) {
    if (boot_state != BOOT_DONE) return;

    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    if (rescan_state != RESCAN_IDLE) {
        rescan_render();
        return;
    }

    enum Ps2McResult res;
    const u32 seq = ps2_memcard_last_result(&res);
    const bool busy = ps2_memcard_activity() != PS2_MC_IDLE;

    if (seq != hud_seq) {
        hud_seq = seq;
        if (res == PS2_MC_RES_ERROR)
            hud_show(HUD_ERROR, icon_error);
    }

    if (busy) {
        // an error stays up for its minimum time before a new save replaces it
        if (hud_state == HUD_SAVING)
            hud_leaving = false;
        else if (hud_state != HUD_ERROR || hud_frames >= ICON_MIN_FRAMES)
            hud_show(HUD_SAVING, icon_saving);
    }

    if (hud_state == HUD_HIDDEN) return;

    if (!hud_leaving && hud_frames >= ICON_MIN_FRAMES && !(hud_state == HUD_SAVING && busy))
        hud_leaving = true;

    hud_frames++;
    hud_alpha += hud_leaving ? -ICON_FADE_STEP : ICON_FADE_STEP;
    if (hud_alpha > 255) hud_alpha = 255;
    if (hud_alpha <= 0) {
        hud_alpha = 0;
        if (hud_leaving) {
            hud_state = HUD_HIDDEN;
            return;
        }
    }

    render_icon_alpha(ICON_X, ICON_Y, hud_tex, hud_alpha);
}

#endif // TARGET_PS2
