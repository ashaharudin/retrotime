// crt_clock.c -- CRT Retro Clock for Flipper Zero
// Displays HH:MM:SS with animated beam sweep and phosphor-style thick digits
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
#define BEAM_WIDTH   16   // px -- bright leading stripe
#define TRAIL_WIDTH  4  // px -- erased wake behind beam
#define BEAM_STEP    3   // px per frame the beam advances
 
// App state
typedef struct {
    FuriMessageQueue* event_queue;
    bool running;
    int beam_x; // current X of sweeping beam leading edge
} CrtClockApp;
 
// 5x7 bitmap font: digits 0-9, then colon (index 10)
// Each entry is 5 column bitmasks, bit0 = top row
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
 
// Draw a single glyph at 2x scale
static void draw_glyph(Canvas* canvas, int x, int y, uint8_t idx) {
    const uint8_t* g = FONT_5x7[idx];
    for(int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for(int row = 0; row < 7; row++) {
            if(bits & (1 << row)) {
                canvas_draw_dot(canvas, x + col * 2,     y + row * 2);
                canvas_draw_dot(canvas, x + col * 2 + 1, y + row * 2);
                canvas_draw_dot(canvas, x + col * 2,     y + row * 2 + 1);
                canvas_draw_dot(canvas, x + col * 2 + 1, y + row * 2 + 1);
            }
        }
    }
}
 
#define GLYPH_W 11  // digit width px (includes 1px gap)
#define GLYPH_H 14  // digit height px (7 rows * 2)
 
// Draw HH:MM:SS centred, with blinking colons
static void draw_time(Canvas* canvas, DateTime* dt) {
    int total_w = 6 * GLYPH_W + 2 * 10; // 86px
    int start_x = (SCREEN_W - total_w) / 2;
    int y = (SCREEN_H - GLYPH_H) / 2;
 
    bool colon_on = (dt->second % 2 == 0);
 
    uint8_t digits[8] = {
        dt->hour   / 10, dt->hour   % 10, 10,
        dt->minute / 10, dt->minute % 10, 10,
        dt->second / 10, dt->second % 10,
    };
 
    int cursor = start_x;
    for(int i = 0; i < 8; i++) {
        bool is_colon = (digits[i] == 10);
        if(!is_colon || colon_on) {
            draw_glyph(canvas, cursor, y, digits[i]);
        }
        cursor += is_colon ? 10 : GLYPH_W;
    }
}
 
// Draw the CRT border and status label
static void draw_frame(Canvas* canvas) {
    canvas_draw_frame(canvas, 0, 0, SCREEN_W, SCREEN_H);
    canvas_set_font(canvas, FontSecondary);
    //canvas_draw_str(canvas, 3, 9, "[ CRT ]"); maybe consider reformatting
    //canvas_draw_str(canvas, 100, 9, "_");
}
 
// Draw the animated beam sweep.
// The beam moves left to right. Behind it is an erased (white) trail so the
// digits appear to be "written" by the beam and fade in the trail.
// Horizontal scanline gaps are applied everywhere for the raster look.
static void draw_beam(Canvas* canvas, int beam_x) {
    // 1. Erase the trail region just behind the beam
    canvas_set_color(canvas, ColorWhite);
    int trail_start = beam_x - TRAIL_WIDTH;
    for(int x = trail_start; x < beam_x; x++) {
        if(x < 0 || x >= SCREEN_W) continue;
        for(int y = 0; y < SCREEN_H; y++) {
            canvas_draw_dot(canvas, x, y);
        }
    }
 
    // 2. Re-apply horizontal scanline gaps across the whole screen
    // for(int y = 1; y < SCREEN_H; y += 2) {
    //     for(int x = 0; x < SCREEN_W; x++) {
    //         canvas_draw_dot(canvas, x, y);
    //     }
    // }
 
    // 3. Draw the bright leading edge stripe in black
    canvas_set_color(canvas, ColorBlack);
    for(int x = beam_x; x < beam_x + BEAM_WIDTH && x < SCREEN_W; x++) {
        for(int y = 0; y < SCREEN_H; y += 2) {
            canvas_draw_dot(canvas, x, y);
        }
    }
}
 
// Render callback -- called by the GUI each frame
static void render_callback(Canvas* canvas, void* ctx) {
    CrtClockApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
 
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
 
    draw_frame(canvas);
    draw_time(canvas, &dt);
    draw_beam(canvas, app->beam_x); // beam applied last, over digits
}
 
// Input callback
static void input_callback(InputEvent* event, void* ctx) {
    CrtClockApp* app = ctx;
    furi_message_queue_put(app->event_queue, event, FuriWaitForever);
}
 
// Main
int32_t crt_clock_app(void* p) {
    UNUSED(p);
 
    CrtClockApp* app = malloc(sizeof(CrtClockApp));
    app->running = true;
    app->beam_x  = 0;
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
 
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render_callback, app);
    view_port_input_callback_set(vp, input_callback, app);
 
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
 
    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 50) == FuriStatusOk) {
            if(event.type == InputTypeShort &&
               (event.key == InputKeyBack || event.key == InputKeyOk)) {
                app->running = false;
            }
        }
 
        // Advance beam; wrap around when trail has cleared the right edge
        app->beam_x += BEAM_STEP;
        if(app->beam_x >= SCREEN_W + TRAIL_WIDTH) {
            app->beam_x = 0;
        }
 
        view_port_update(vp);
    }
 
    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(app->event_queue);
    free(app);
 
    return 0;
}