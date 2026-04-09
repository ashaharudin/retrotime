// crt_clock.c -- CRT Retro Clock for Flipper Zero
// Displays HH:MM:SS with animated beam sweep that inverts glyph pixels
// Build: place in applications_user/crt_clock/ with application.fam

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <furi_hal_rtc.h>
#include <notification/notification_messages.h>

#define APP_NAME "CRT Clock"

// Canvas dimensions
#define SCREEN_W 128
#define SCREEN_H 64

// Beam sweep config
#define BEAM_WIDTH   4   // px -- inversion stripe width
#define TRAIL_WIDTH  48  // px -- erased wake behind beam
#define BEAM_STEP    1   // px per frame the beam advances
#define ERASE_LEAD   20  // px -- how far the erase beam leads the reveal beam

// Screen modes
typedef enum {
    ScreenClock,
    ScreenSettings,
} ScreenMode;

// App state
typedef struct {
    FuriMessageQueue* event_queue;
    bool running;
    bool beamstart;
    int beam_x;   // reveal beam position
    ScreenMode mode;
    bool backlight_on; // true = always on, false = auto
    NotificationApp* notifications;
} CrtClockApp;

// 5x7 bitmap font: digits 0-9, then colon (index 10)
static const uint8_t FONT_5x7[11][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x00, 0x36, 0x36, 0x00}, // : (colon)
};

#define GLYPH_W 11
#define GLYPH_H 14

// Draw a glyph at 2x scale in the given color.
// x_clip_lo / x_clip_hi constrain which pixel columns are drawn --
// pass 0 / SCREEN_W to draw everything, or beam bounds to draw only
// the beam slice.
static void draw_glyph_colored(
    Canvas* canvas,
    int x, int y,
    uint8_t idx,
    Color color,
    int x_clip_lo, int x_clip_hi)
{
    canvas_set_color(canvas, color);
    const uint8_t* g = FONT_5x7[idx];
    for(int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for(int row = 0; row < 7; row++) {
            if(bits & (1 << row)) {
                int px = x + col * 2;
                int py = y + row * 2;
                // Draw both rows of the 2x block
                for(int dx = 0; dx < 2; dx++) {
                    for(int dy = 0; dy < 2; dy++) {
                        int fx = px + dx;
                        int fy = py + dy;
                        if(fx >= x_clip_lo && fx < x_clip_hi) {
                            canvas_draw_dot(canvas, fx, fy);
                        }
                    }
                }
            }
        }
    }
}

// Vertical layout constants
// Time row sits in upper half, date row below it
#define TIME_Y   14  // top of time glyphs
#define DATE_Y   40  // baseline of date text row (~4px gap below time glyphs)

static const char* const DAY_NAMES[8] = {
    "", "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"
};

static const char* const MONTH_NAMES[13] = {
    "", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

// Draw HH:MM:SS with clipping -- used for both normal and inverted pass
static void draw_time_clipped(
    Canvas* canvas,
    DateTime* dt,
    Color color,
    int x_clip_lo, int x_clip_hi)
{
    int total_w = 6 * GLYPH_W + 2 * 10;
    int start_x = (SCREEN_W - total_w) / 2;

    bool colon_on = true;

    uint8_t digits[8] = {
        dt->hour   / 10, dt->hour   % 10, 10,
        dt->minute / 10, dt->minute % 10, 10,
        dt->second / 10, dt->second % 10,
    };

    int cursor = start_x;
    for(int i = 0; i < 8; i++) {
        bool is_colon = (digits[i] == 10);
        if(!is_colon || colon_on) {
            draw_glyph_colored(canvas, cursor, TIME_Y, digits[i],
                               color, x_clip_lo, x_clip_hi);
        }
        cursor += is_colon ? 10 : GLYPH_W;
    }
}

// Draw "MON  12 JAN 2025" below the time, with x clipping for beam inversion
static void draw_date_clipped(
    Canvas* canvas,
    DateTime* dt,
    Color color,
    int x_clip_lo, int x_clip_hi)
{
    // Build date string: "WED  08 JAN 2025"
    char date_str[24];
    uint8_t wd = (dt->weekday >= 1 && dt->weekday <= 7) ? dt->weekday : 1;
    uint8_t mo = (dt->month  >= 1 && dt->month  <= 12) ? dt->month  : 1;
    snprintf(date_str, sizeof(date_str), "%s %02d %s%04d",
             DAY_NAMES[wd], dt->day, MONTH_NAMES[mo], dt->year);

    // Measure string width using FontSecondary (~6px per char)
    // canvas_string_width isn't available in all SDK versions, so we estimate:
    // FontSecondary glyphs are 6px wide including spacing
    int char_w = 6;
    int str_len = 0;
    for(int i = 0; date_str[i]; i++) str_len++;
    int str_w = str_len * char_w;
    int x = (SCREEN_W - str_w) / 2;

    canvas_set_color(canvas, color);
    canvas_set_font(canvas, FontSecondary);

    // Draw character by character so we can clip to beam region
    for(int i = 0; i < str_len; i++) {
        int cx = x + i * char_w;
        // Only draw chars that overlap the clip region
        if(cx + char_w > x_clip_lo && cx < x_clip_hi) {
            char ch[2] = {date_str[i], 0};
            canvas_draw_str(canvas, cx, DATE_Y, ch);
        }
    }
}

// Draw settings screen
static void draw_settings(Canvas* canvas, CrtClockApp* app) {
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_frame(canvas, 0, 0, SCREEN_W, SCREEN_H);
    canvas_draw_str(canvas, 3, 9, "[ SETTINGS ]");
    canvas_draw_str(canvas, 90, 9, "OK=toggle");

    // Backlight row
    canvas_draw_str(canvas, 4, 28, "Backlight:");
    const char* bl_val = app->backlight_on ? "Always On" : "Auto";
    // Highlight the value
    canvas_draw_box(canvas, 67, 19, 56, 12);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str(canvas, 69, 28, bl_val);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, 3, 58, "Back=return");
}

// Draw the CRT border and status label
static void draw_frame(Canvas* canvas) {
    canvas_set_color(canvas, ColorBlack);
    //canvas_draw_frame(canvas, 0, 0, SCREEN_W, SCREEN_H);
    //canvas_set_font(canvas, FontSecondary);
    //canvas_draw_str(canvas, 3, 9, "[ CRT ]");
    //canvas_draw_str(canvas, 100, 9, "_");
}

// Draw the beam:
//   1. Erase lead  -- solid white stripe ahead of the beam, hides text before reveal
//   2. Noisy trail -- LFSR noise wake behind the beam
//   3. Beam stripe -- solid black, inversion background
static void draw_beam(Canvas* canvas, int beam_x) {
    // 1. Erase lead: white zone from beam_x to beam_x + ERASE_LEAD
    canvas_set_color(canvas, ColorWhite);
    for(int x = beam_x; x < beam_x + ERASE_LEAD && x < SCREEN_W; x++) {
        if(x < 0) continue;
        for(int y = 0; y < SCREEN_H; y++) {
            canvas_draw_dot(canvas, x, y);
        }
    }

    // 2. Noisy trail behind the beam
    static uint16_t lfsr = 0xACE1u;
    for(int x = beam_x - TRAIL_WIDTH; x < beam_x; x++) {
        if(x < 0 || x >= SCREEN_W) continue;
        for(int y = 0; y < SCREEN_H; y++) {
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u); // Galois LFSR
            canvas_set_color(canvas, (lfsr & 1) ? ColorBlack : ColorWhite);
            canvas_draw_dot(canvas, x, y);
        }
    }

    // 3. Beam stripe black (inversion background)
    canvas_set_color(canvas, ColorBlack);
    for(int x = beam_x; x < beam_x + BEAM_WIDTH && x < SCREEN_W; x++) {
        for(int y = 0; y < SCREEN_H; y++) {
            canvas_draw_dot(canvas, x, y);
        }
    }
}

// Render callback
static void render_callback(Canvas* canvas, void* ctx) {
    CrtClockApp* app = ctx;

    if(app->mode == ScreenSettings) {
        draw_settings(canvas, app);
        return;
    }

    canvas_clear(canvas);

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    // Layer 1: frame, time and date in black
    draw_frame(canvas);
    draw_time_clipped(canvas, &dt, ColorBlack, 0, SCREEN_W);
    draw_date_clipped(canvas, &dt, ColorBlack, 0, SCREEN_W);

    // Layers 2 & 3 only active while beam is sweeping
    if(app->beamstart) {
        // Layer 2: beam (erase lead + noisy trail + black stripe)
        draw_beam(canvas, app->beam_x);

        // Layer 3: redraw text in WHITE only in the revealed zone
        //          (trail behind beam, before erase lead wipes ahead)
        draw_time_clipped(canvas, &dt, ColorWhite,
                          app->beam_x - TRAIL_WIDTH, app->beam_x + BEAM_WIDTH);
        draw_date_clipped(canvas, &dt, ColorWhite,
                          app->beam_x - TRAIL_WIDTH, app->beam_x + BEAM_WIDTH);
    }
}

// Input callback
static void input_callback(InputEvent* event, void* ctx) {
    CrtClockApp* app = ctx;
    furi_message_queue_put(app->event_queue, event, FuriWaitForever);
}

// Apply backlight setting via notification service
static void apply_backlight(CrtClockApp* app) {
    if(app->backlight_on) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    } else {
        notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    }
}

// Main
int32_t crt_clock_app(void* p) {
    UNUSED(p);

    CrtClockApp* app = malloc(sizeof(CrtClockApp));
    app->running      = true;
    app->beamstart    = false;
    app->beam_x       = -(BEAM_WIDTH); // fully off-screen left
    app->mode         = ScreenClock;
    app->backlight_on = false; // default to auto
    app->event_queue  = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Advance beam once before registering the viewport so the very
    // first render callback never sees the raw init value
    app->beam_x += BEAM_STEP;

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render_callback, app);
    view_port_input_callback_set(vp, input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    apply_backlight(app); // apply default (auto)

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 50) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
                if(app->mode == ScreenSettings) {
                    if(event.key == InputKeyBack) {
                        app->mode = ScreenClock;
                    } else if(event.key == InputKeyOk) {
                        app->backlight_on = !app->backlight_on;
                        apply_backlight(app);
                    }
                } else {
                    // Clock screen
                    if(event.key == InputKeyOk) {
                        app->mode = ScreenSettings;
                    } else if(event.key == InputKeyBack) {
                        app->running = false;
                    }
                }
            }
        }

        // Trigger beam on every 10-second mark (main loop owns all beam state)
        DateTime trigger_dt;
        furi_hal_rtc_get_datetime(&trigger_dt);
        if(trigger_dt.second % 10 == 0 && !app->beamstart) {
            app->beamstart = true;
        }

        if(app->beamstart) {
            app->beam_x += BEAM_STEP;
            if(app->beam_x >= SCREEN_W + TRAIL_WIDTH) {
                app->beamstart = false;
                app->beam_x = -(BEAM_WIDTH);
            }
        }

        view_port_update(vp);
    }

    // Release backlight enforce on exit
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}