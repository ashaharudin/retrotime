// crt_clock.c — CRT Retro Clock for Flipper Zero
// Displays HH:MM:SS with scanline overlay and phosphor-style thick digits
// Build: place in applications_user/crt_clock/ with application.fam

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <furi_hal_rtc.h>
#include <notification/notification_messages.h>

#define APP_NAME "CRT Clock"

// ─── Canvas dimensions ────────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H 64

// ─── State ────────────────────────────────────────────────────────────────────
typedef struct {
    FuriMessageQueue* event_queue;
    bool running;
} CrtClockApp;

// ─── 5×7 bitmap font (digits 0-9 and colon) ──────────────────────────────────
// Each glyph is 5 columns × 7 rows, stored as column bitmasks (bit0 = top row).
// We render them 2× scaled for a chunky phosphor look.

static const uint8_t FONT_5x7[11][5] = {
    // 0
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    // 1
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    // 2
    {0x42, 0x61, 0x51, 0x49, 0x46},
    // 3
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    // 4
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    // 5
    {0x27, 0x45, 0x45, 0x45, 0x39},
    // 6
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    // 7
    {0x01, 0x71, 0x09, 0x05, 0x03},
    // 8
    {0x36, 0x49, 0x49, 0x49, 0x36},
    // 9
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    // colon (index 10)
    {0x00, 0x00, 0x36, 0x36, 0x00},
};

// Draw a single glyph scaled 2× at (x, y)
static void draw_glyph(Canvas* canvas, int x, int y, uint8_t glyph_idx) {
    const uint8_t* g = FONT_5x7[glyph_idx];
    for(int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for(int row = 0; row < 7; row++) {
            if(bits & (1 << row)) {
                // 2× scale: draw a 2×2 block per pixel
                canvas_draw_dot(canvas, x + col * 2,     y + row * 2);
                canvas_draw_dot(canvas, x + col * 2 + 1, y + row * 2);
                canvas_draw_dot(canvas, x + col * 2,     y + row * 2 + 1);
                canvas_draw_dot(canvas, x + col * 2 + 1, y + row * 2 + 1);
            }
        }
    }
}

// Glyph width at 2× = 5 cols × 2 px + 1 gap = 11px; colon = 5×2 = 10px
#define GLYPH_W 11  // per digit (includes 1px gap)
#define GLYPH_H 14  // 7 rows × 2

// Draw HH:MM:SS centred on screen
static void draw_time(Canvas* canvas, DateTime* dt) {
    // Layout: 8 glyphs (6 digits + 2 colons)
    // Total width: 6*11 + 2*10 = 66 + 20 = 86 px (approximate)
    int total_w = 6 * GLYPH_W + 2 * 10; // 86
    int start_x = (SCREEN_W - total_w) / 2; // ~21
    int y = (SCREEN_H - GLYPH_H) / 2;       // ~25 — vertically centred

    uint8_t digits[8] = {
        dt->hour   / 10, dt->hour   % 10, 10,  // HH:
        dt->minute / 10, dt->minute % 10, 10,  // MM:
        dt->second / 10, dt->second % 10,       // SS
    };

    int cursor = start_x;
    for(int i = 0; i < 8; i++) {
        draw_glyph(canvas, cursor, y, digits[i]);
        cursor += (digits[i] == 10) ? 10 : GLYPH_W; // colon narrower
    }
}

// ─── Scanline overlay ─────────────────────────────────────────────────────────
// Every other horizontal line is left "dark" (we clear those pixel rows).
// On a real display this simulates the CRT raster gap between phosphor rows.
// We draw a horizontal line in XOR/clear mode every even row in the digit area.
static void draw_scanlines(Canvas* canvas) {
    for(int y = 0; y < SCREEN_H; y += 2) {
        // Draw a faint horizontal rule by clearing a 1-pixel row
        // canvas_set_color sets the draw colour; ColorWhite erases on this display
        canvas_set_color(canvas, ColorWhite);
        for(int x = 0; x < SCREEN_W; x++) {
            canvas_draw_dot(canvas, x, y);
        }
        canvas_set_color(canvas, ColorBlack);
    }
}

// ─── Top status bar ───────────────────────────────────────────────────────────
static void draw_frame(Canvas* canvas) {
    // Thin border
    canvas_draw_frame(canvas, 0, 0, SCREEN_W, SCREEN_H);

    // "CRT" label top-left
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 3, 9, "[ CRT ]");

    // Blinking cursor blink indicator top-right (static underscore)
    canvas_draw_str(canvas, 100, 9, "_");
}

// ─── Render callback ──────────────────────────────────────────────────────────
static void render_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    // Get current time from RTC
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    // Draw UI layers
    draw_frame(canvas);
    draw_time(canvas, &dt);
    draw_scanlines(canvas); // applied last so it overlays the digits
}

// ─── Input callback ───────────────────────────────────────────────────────────
static void input_callback(InputEvent* event, void* ctx) {
    CrtClockApp* app = ctx;
    furi_message_queue_put(app->event_queue, event, FuriWaitForever);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int32_t crt_clock_app(void* p) {
    UNUSED(p);

    CrtClockApp* app = malloc(sizeof(CrtClockApp));
    app->running = true;
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Register GUI view port
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render_callback, app);
    view_port_input_callback_set(vp, input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    // Main loop — redraw ~once per second
    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) {
            // Back / OK exits
            if(event.type == InputTypeShort &&
               (event.key == InputKeyBack || event.key == InputKeyOk)) {
                app->running = false;
            }
        }
        view_port_update(vp);
    }

    // Cleanup
    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}