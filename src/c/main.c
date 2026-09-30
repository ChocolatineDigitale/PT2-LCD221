// LCD 221: a watch face for the Pebble Time 2 (emery, 200x228) that imitates
// the Casio W-221H: a white "LCD" with slanted 7-segment digits, a dot-matrix
// weekday and a 2x2 indicator box, between a top and a bottom bezel.
//
// Everything on the LCD panel is rasterized straight into the framebuffer, which
// allows things the drawing API can't do: polygon segments with a slant and
// gray-shade anti-aliasing, and faint "ghost" segments for the unlit parts (a
// sparse ordered dither). Only the bezel text and indicator labels go through
// the regular text API.
//
// File map (search for the "----" section banners):
//   raster primitives  span/fill and the ghost dither
//   7-segment digits   polygon fill, anti-aliasing, digits (outlines: segments.h)
//   dot-matrix glyphs  weekday letters, PM marker, small symbols
//   data helpers       weather, steps, heart rate, theme, backlight
//   drawing            layout constants, text helpers, one function per screen area
//   services           ticks, battery, Bluetooth vibration, phone messages
//   lifecycle          init / deinit
//
// Settings arrive from the phone (src/pkjs/config.js) as app messages and are
// persisted; bump SETTINGS_KEY whenever the Settings struct changes.

#include <pebble.h>
#include "segments.h"

// The 7-segment digits (time, date, temperature) lean like the W-221H's: LCD_SLANT
// is the lean in thousandths of the height (75 = 7.5%, as measured on the
// original). LCD_AA smooths their edges with the display's gray shades.
#define LCD_SLANT 75
#define LCD_AA true
#define SETTINGS_KEY 14  // bumped whenever Settings changes layout
#define WEATHER_KEY 4  // bumped whenever Weather changes layout
#define WEATHER_MAX_AGE (3 * 60 * 60)
#define WEATHER_REFRESH_MIN 30

#define BACKLIGHT_SYSTEM 0xFFFFFFFFu  // the user's normal backlight colour
#define BACKLIGHT_CUSTOM 0xFFFFFFFEu  // use backlight_custom

// Vibration patterns selectable for phone connect/disconnect.
typedef enum {
  VIBE_NONE, VIBE_SHORT, VIBE_LONG, VIBE_DOUBLE, VIBE_TRIPLE, VIBE_HEARTBEAT, VIBE_SOS, VIBE_COUNT
} VibeChoice;

// Everything the settings page controls, grouped and ordered like the page itself
// (config.js). Saved with persist_write_data: bump SETTINGS_KEY when it changes.
typedef struct {
  // Time & date
  bool twelve_hour;
  bool day_first;
  // Right box
  bool show_seconds;           // seconds instead of the temperature (redraws every second)
  // Top bezel
  bool show_battery;           // top-left: battery level, else top_left
  char top_left[20];           // top-left text when the battery level is off, upper-cased
  bool show_steps;             // top-right: step count, else top_right
  char top_right[20];          // top-right text when the step count is off, upper-cased
  // Bottom bezel
  bool heart_rate;             // HR badge + latest BPM instead of the WR badge
  char bezel_label[16];        // printed on the bottom bezel, upper-cased
  // Appearance
  bool silver;                 // silver case instead of black
  bool inverted;               // light digits on a dark LCD
  bool ghosts;                 // show faint unlit segments
  uint32_t backlight;          // 0xRRGGBB tint, BACKLIGHT_SYSTEM, or BACKLIGHT_CUSTOM
  uint32_t backlight_custom;   // 0xRRGGBB, from the settings page's colour picker
  // Alerts
  uint8_t vibe_disconnect;     // VibeChoice played when the phone disconnects
  uint8_t vibe_connect;        // VibeChoice played when it reconnects
} Settings;

typedef struct {
  int16_t temp;
  time_t updated;
} Weather;

static Window *s_window;
static Layer *s_canvas;
static GBitmap *s_fb;

static Settings s_settings;
static Weather s_weather;
static struct tm s_now;
static BatteryChargeState s_battery;
static bool s_connected;
static int s_steps = -1;
static int s_hr = -1;  // latest heart rate in BPM, -1 if unavailable

// Theme (see apply_theme). Normal: black ink on a white LCD. Inverted: white ink
// on a black LCD, with every shade swapped accordingly.
static GColor s_ink, s_lcd;          // digits, lines and lit text; the LCD's background
static GColor s_ghost;               // unlit segments and dots (drawn dithered)
static GColor s_frame, s_frame_text; // case (bezels) and the text printed on it
static uint8_t s_aa[3];              // anti-aliasing shades: faint, medium, full ink
static int s_ghost_density;          // dot density of s_ghost, out of DENSITY_FULL
static int s_label_off_density;      // dot density of inactive indicator labels (drawn in s_ghost)

// ---------------------------------------------------------------------------
// Framebuffer raster primitives

// Dithering: a shape is drawn as a dot pattern whose density is a number out of
// DENSITY_FULL (a pixel is drawn when its 4x4 Bayer threshold is below it), so
// 4 = 25%, 8 = 50% (checkerboard) and 16 = solid.
#define DENSITY_FULL 16
// Unlit segments and dots ("ghosts"). The inverted theme uses fewer dots, because
// light dots on black look brighter than dark dots on white.
#define GHOST_DENSITY 7
#define GHOST_DENSITY_INVERTED 4
// Inactive indicator labels are solid, except when inverted: dark gray is the
// darkest shade the display has, so they are drawn as dots instead.
#define LABEL_OFF_DENSITY_INVERTED 8
static const uint8_t BAYER4[4][4] = {
  { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static void span(int y, int x0, int x1, GColor c, bool dither) {
  if (y < 0 || y >= PBL_DISPLAY_HEIGHT) return;
  GBitmapDataRowInfo row = gbitmap_get_data_row_info(s_fb, y);
  if (x0 < row.min_x) x0 = row.min_x;
  if (x1 > row.max_x) x1 = row.max_x;
  for (int x = x0; x <= x1; x++) {
    if (!dither || BAYER4[y & 3][x & 3] < s_ghost_density) row.data[x] = c.argb;
  }
}

static void fill(int x, int y, int w, int h, GColor c, bool dither) {
  for (int r = 0; r < h; r++) span(y + r, x, x + w - 1, c, dither);
}

// ---------------------------------------------------------------------------
// 7-segment digits

enum { SEG_A = 1, SEG_B = 2, SEG_C = 4, SEG_D = 8, SEG_E = 16, SEG_F = 32, SEG_G = 64 };
#define DIGIT_BLANK -1
#define DIGIT_MINUS 10

static const uint8_t DIGIT_SEGS[] = {
  SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,          // 0
  SEG_B | SEG_C,                                          // 1
  SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,                  // 2
  SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,                  // 3
  SEG_F | SEG_G | SEG_B | SEG_C,                          // 4
  SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,                  // 5
  SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,          // 6
  SEG_A | SEG_B | SEG_C,                                  // 7
  0x7F,                                                   // 8
  SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,          // 9
  SEG_G,                                                  // minus
};

static int floor_div(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Fills a polygon given in 0..1000 coordinates, scaled into the box (x, y, w, h).
// A pixel is filled when its centre is inside (even-odd rule).
// `slant` leans the shape right like italics, in thousandths of the height:
// each row is shifted by its height above the bottom times slant/1000, with
// sub-pixel precision so edges stay clean diagonals.
static void fill_poly(int x, int y, int w, int h, const int16_t *pts, int n, int slant, GColor c,
                      bool dither) {
  for (int r = 0; r < h; r++) {
    int32_t shift = (int32_t)(2 * (h - r) - 1) * slant;  // row shift, in 1/2000 px
    int32_t yn = (int32_t)(2 * r + 1) * 1000 / (2 * h);
    int32_t xs[8];
    int count = 0;
    for (int i = 0; i < n && count < 8; i++) {
      int32_t x0 = pts[2 * i], y0 = pts[2 * i + 1];
      int32_t x1 = pts[2 * ((i + 1) % n)], y1 = pts[2 * ((i + 1) % n) + 1];
      if ((y0 <= yn) != (y1 <= yn)) xs[count++] = x0 + (x1 - x0) * (yn - y0) / (y1 - y0);
    }
    for (int i = 1; i < count; i++) {
      for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
        int32_t tmp = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = tmp;
      }
    }
    for (int i = 0; i + 1 < count; i += 2) {
      int c0 = -floor_div(1000 - 2 * xs[i] * w - shift, 2000);   // ceil(px - 0.5)
      int c1 = floor_div(2 * xs[i + 1] * w + shift - 1000, 2000);
      if (c0 <= c1) span(y + r, x + c0, x + c1, c, dither);
    }
  }
}

// Anti-aliased version of fill_poly for lit (ink) shapes: each pixel's
// coverage is measured exactly across its width at 4 heights, then drawn in
// one of the display's shades between ink and white (black, dark gray, light
// gray). A pixel is only ever darkened, never lightened, so shapes drawn one
// after another (neighbouring segments) don't eat into each other.
#define AA_SUB 4
#define AA_MAX_W 96
static void fill_poly_aa(int x, int y, int w, int h, const int16_t *pts, int n, int slant) {
  int32_t cov[AA_MAX_W];
  int cols = w + h * slant / 1000 + 2;
  if (cols > AA_MAX_W) cols = AA_MAX_W;
  for (int r = 0; r < h; r++) {
    memset(cov, 0, sizeof(cov));
    for (int k = 0; k < AA_SUB; k++) {
      // Sub-row centre in 0..1000 shape units, and its slant shift in 1/1000 px.
      int32_t yn = (int32_t)(2 * AA_SUB * r + 2 * k + 1) * 1000 / (2 * AA_SUB * h);
      int32_t shift = (int32_t)(2 * AA_SUB * (h - r) - 2 * k - 1) * slant / (2 * AA_SUB);
      int32_t xs[8];
      int count = 0;
      for (int i = 0; i < n && count < 8; i++) {
        int32_t x0 = pts[2 * i], y0 = pts[2 * i + 1];
        int32_t x1 = pts[2 * ((i + 1) % n)], y1 = pts[2 * ((i + 1) % n) + 1];
        if ((y0 <= yn) != (y1 <= yn)) xs[count++] = x0 + (x1 - x0) * (yn - y0) / (y1 - y0);
      }
      for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
          int32_t tmp = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = tmp;
        }
      }
      for (int i = 0; i + 1 < count; i += 2) {
        int32_t a = xs[i] * w + shift, b = xs[i + 1] * w + shift;  // span in 1/1000 px
        if (a < 0) a = 0;
        if (b > (int32_t)cols * 1000) b = (int32_t)cols * 1000;
        for (int c = a / 1000; c < cols && (int32_t)c * 1000 < b; c++) {
          int32_t lo = a > c * 1000 ? a : c * 1000, hi = b < (c + 1) * 1000 ? b : (c + 1) * 1000;
          if (hi > lo) cov[c] += hi - lo;
        }
      }
    }
    int yy = y + r;
    if (yy < 0 || yy >= PBL_DISPLAY_HEIGHT) continue;
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(s_fb, yy);
    for (int c = 0; c < cols; c++) {
      // Full coverage = AA_SUB * 1000.
      int level = cov[c] >= 3000 ? 2 : cov[c] >= 1800 ? 1 : cov[c] >= 600 ? 0 : -1;
      int xx = x + c;
      if (level < 0 || xx < row.min_x || xx > row.max_x) continue;
      uint8_t cur = row.data[xx];
      int cur_level = cur == s_aa[2] ? 2 : cur == s_aa[1] ? 1 : -1;
      if (level > cur_level) row.data[xx] = s_aa[level];
    }
  }
}

// A 7-segment digit built from the segment outlines of the "7-Segment" font by
// Jan Bobrowski (see segments.h), scaled to the box and slanted by LCD_SLANT. Only the
// segments in `present` are drawn; unlit ones appear as ghosts (if enabled),
// drawn first so lit segments always win.
static void draw_segments(int x, int y, int w, int h, uint8_t on, uint8_t present) {
  for (int pass = s_settings.ghosts ? 0 : 1; pass <= 1; pass++) {
    uint8_t mask = present & (pass ? on : (uint8_t)~on);
    for (int i = 0; i < 7; i++) {
      if (!(mask & (1 << i))) continue;
      if (pass && LCD_AA) fill_poly_aa(x, y, w, h, SEG_POLYS[i], SEG_POLY_LEN[i], LCD_SLANT);
      else fill_poly(x, y, w, h, SEG_POLYS[i], SEG_POLY_LEN[i], LCD_SLANT,
                     pass ? s_ink : s_ghost, !pass);
    }
  }
}

static void draw_digit(int x, int y, int w, int h, int value) {
  uint8_t on = (value >= 0 && value <= DIGIT_MINUS) ? DIGIT_SEGS[value] : 0;
  draw_segments(x, y, w, h, on, 0x7F);
}

// ---------------------------------------------------------------------------
// Dot-matrix glyphs ('#' = lit)

// Day-of-week letters: 5x5, matching the W-221H's dot-matrix day display.
static const char *day_glyph(char ch) {
  switch (ch) {
    case 'A': return ".###.#...#######...##...#";  // .###. #...# ##### #...# #...#
    case 'D': return "####.#...##...##...#####.";  // ####. #...# #...# #...# ####.
    case 'E': return "######....####.#....#####";  // ##### #.... ####. #.... #####
    case 'F': return "######....####.#....#....";  // ##### #.... ####. #.... #....
    case 'H': return "#...##...#######...##...#";  // #...# #...# ##### #...# #...#
    case 'I': return ".###...#....#....#...###.";  // .###. ..#.. ..#.. ..#.. .###.
    case 'M': return "#...###.###.#.##...##...#";  // #...# ##.## #.#.# #...# #...#
    case 'N': return "#...###..##.#.##..###...#";  // #...# ##..# #.#.# #..## #...#
    case 'O': return ".###.#...##...##...#.###.";  // .###. #...# #...# #...# .###.
    case 'R': return "####.#...#####.#..#.#...#";  // ####. #...# ####. #..#. #...#
    case 'S': return ".#####.....###.....#####.";  // .#### #.... .###. ....# ####.
    case 'T': return "#####..#....#....#....#..";  // ##### ..#.. ..#.. ..#.. ..#..
    case 'U': return "#...##...##...##...#.###.";  // #...# #...# #...# #...# .###.
    case 'W': return "#...##...##.#.###.###...#";  // #...# #...# #.#.# ##.## #...#
    default:  return NULL;
  }
}

// The "P" (PM) marker: a solid 11x14 LCD symbol like the W-221H's.
static const char PM_BITS[] =
  "#########.."
  "##########."
  "###....####"
  "###.....###"
  "###.....###"
  "###....####"
  "##########."
  "#########.."
  "###........"
  "###........"
  "###........"
  "###........"
  "###........"
  "###........";

// Draws a grid of `cols` x `rows` dots. With `on` false, the '#' dots are drawn
// as ghosts instead (an unlit fixed LCD symbol).
static void draw_dots(int x, int y, const char *bits, int cols, int rows, int pitch, int dot, bool on) {
  if (!bits || (!on && !s_settings.ghosts)) return;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if (bits[r * cols + c] != '#') continue;
      fill(x + c * pitch, y + r * pitch, dot, dot, on ? s_ink : s_ghost, !on);
    }
  }
}

// Dot-matrix cell: lit dots solid, the rest as ghosts (if enabled). Dots are
// dot_w x dot_h rectangles on a px x py grid.
static void draw_matrix(int x, int y, const char *bits, int cols, int rows, int px, int py, int dot_w,
                        int dot_h) {
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      bool lit = bits && bits[r * cols + c] == '#';
      if (!lit && !s_settings.ghosts) continue;
      fill(x + c * px, y + r * py, dot_w, dot_h, lit ? s_ink : s_ghost, !lit);
    }
  }
}

// Day-of-week text: 5x5 letters with 4x5 dots on a 5x6 grid, one column apart.
// Upright, like the W-221H's.
static void draw_day(int x, int y, const char *text) {
  for (int i = 0; text[i]; i++) {
    draw_matrix(x + i * 30, y, day_glyph(text[i]), 5, 5, 5, 6, 4, 5);
  }
}

// ---------------------------------------------------------------------------
// Data helpers

static bool weather_valid(void) {
  return s_weather.updated != 0 && time(NULL) - s_weather.updated <= WEATHER_MAX_AGE;
}

static void update_heart_rate(void) {
#if defined(PBL_HEALTH)
  time_t now = time(NULL);
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
  HealthValue bpm = (mask & HealthServiceAccessibilityMaskAvailable)
      ? health_service_peek_current_value(HealthMetricHeartRateBPM) : 0;
  s_hr = bpm > 0 ? (int)bpm : -1;
#endif
}

static void update_steps(void) {
#if defined(PBL_HEALTH)
  time_t start = time_start_of_today();
  time_t end = time(NULL);
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricStepCount, start, end);
  s_steps = (mask & HealthServiceAccessibilityMaskAvailable)
      ? (int)health_service_sum_today(HealthMetricStepCount) : -1;
#endif
}

static void apply_theme(void) {
  bool inv = s_settings.inverted;

  // The LCD.
  s_ink = inv ? GColorWhite : GColorBlack;
  s_lcd = inv ? GColorBlack : GColorWhite;
  s_ghost = inv ? GColorDarkGray : GColorLightGray;
  s_ghost_density = inv ? GHOST_DENSITY_INVERTED : GHOST_DENSITY;
  // Anti-aliasing shades from faint to full ink.
  s_aa[0] = inv ? GColorDarkGrayARGB8 : GColorLightGrayARGB8;
  s_aa[1] = inv ? GColorLightGrayARGB8 : GColorDarkGrayARGB8;
  s_aa[2] = inv ? GColorWhiteARGB8 : GColorBlackARGB8;

  // Inactive indicator labels use the ghost colour: solid on the white LCD,
  // dotted when inverted (dark gray is the darkest shade there is, so dots make it dimmer).
  s_label_off_density = inv ? LABEL_OFF_DENSITY_INVERTED : DENSITY_FULL;

  // The case does not depend on the theme.
  s_frame = s_settings.silver ? GColorLightGray : GColorBlack;
  s_frame_text = s_settings.silver ? GColorBlack : GColorWhite;
}

static void apply_backlight(void) {
#if defined(PBL_RGB_BACKLIGHT)
  if (s_settings.backlight == BACKLIGHT_SYSTEM) light_set_system_color();
  else if (s_settings.backlight == BACKLIGHT_CUSTOM) light_set_color_rgb888(s_settings.backlight_custom);
  else light_set_color_rgb888(s_settings.backlight);
#endif
}

// ---------------------------------------------------------------------------
// Drawing

// Screen layout (Pebble Time 2, 200x228): a black case with a white "LCD" panel
// running edge to edge, a top bezel above it and a bottom bezel below.
#define LCD_X 0
#define LCD_Y 24
#define LCD_W 200
#define LCD_H 176

// Row 1: weekday (left) and the 2x2 indicator box (right).
#define WEEKDAY_X 12
#define WEEKDAY_Y 34
#define BOX_LEFT 109    // indicator box, left outer line (left cells as wide as the right ones)
#define BOX_RIGHT 191   // indicator box, right outer line
#define BOX_DIV 150     // indicator box, vertical divider (off-centre: MUTE is widest)
#define BOX_TOP 34      // indicator box, top outer line (= weekday top)
#define BOX_BOTTOM 62   // indicator box, bottom outer line (= weekday bottom)
#define BOX_MID 48      // indicator box, middle divider
#define BOX_RADIUS 6    // indicator box, radius of the rounded corners
#define LABEL_H 10      // indicator label height in rows

// Row 2: the time.
#define TIME_Y 79
#define TIME_W 32
#define TIME_H 62

// Row 3: date (left) and seconds / temperature (right), under a 2px rule.
#define ROW3_LINE_Y 154
#define ROW3_DIV_X 110  // vertical divider between the date and the right box
#define ROW3_Y 160      // top of the right box's digits
#define ROW3_H 36       // height of the right box's digits
#define TEMP_X 3        // horizontal offset of the whole temperature group (sign, digits, degree)

// Bezels.
#define TOP_RIGHT_MAX 104  // widest the top-right bezel text may be
#define BOTTOM_CAP 208     // top of the bottom bezel's 14px capitals (centred in the bezel)

static const char *const DAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

static void draw_text(GContext *ctx, const char *text, GRect box, GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), box,
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

// Printed bezel text: larger (Gothic 24 bold) for readability. `y` is the top
// of the capitals; the font draws them BEZEL_CAP_OFFSET below the box top.
#define BEZEL_CAP_OFFSET 10
static void draw_bezel_text(GContext *ctx, const char *text, int x, int y, int w,
                            GTextAlignment align) {
  graphics_context_set_text_color(ctx, s_frame_text);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(x, y - BEZEL_CAP_OFFSET, w, 30),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

// Draws a label so it fits its indicator-box cell. The text is drawn in the
// bold font with its ink starting on the cell's top row, then re-sampled in
// the framebuffer (nearest-neighbour, so edges stay crisp), one letter at a time:
//  - vertically to `height` rows by dropping rows from inside each letter (see
//    row_to_drop), so horizontal bars keep their thickness;
//  - optionally horizontally, each letter to its own width `letter_w[i]`
//    (NULL = keep the width), with 2px letter gaps and stems kept at 2px;
//  - drawn with the ordered dither at `density` (out of DENSITY_FULL).
// The result is centred in `cell`; only the cell's interior is touched.
#define FIT_MAX_W 64
#define FIT_MAX_H 16
#define FIT_MAX_LETTERS 8
#define BAR_RUN 4  // a horizontal run of this many ink pixels makes a row part of a bar

typedef uint8_t FitRows[FIT_MAX_H][FIT_MAX_W];

// The longest horizontal run of ink in one row of a letter (columns x0..x1).
static int longest_run(const FitRows buf, int y, int x0, int x1) {
  int best = 0, run = 0;
  for (int x = x0; x <= x1; x++) {
    run = buf[y][x] ? run + 1 : 0;
    if (run > best) best = run;
  }
  return best;
}

// Which of a letter's `n` rows (rows[] holds their indices into buf) to drop when it
// is one row too tall. Never the first or last row, and rows that belong to a
// horizontal bar are avoided (dropping one would thin the bar to 1px while the
// letter's other bars stay 2px). Among the rest, the row most like its neighbour
// wins, then the one nearest the middle of the letter, where a letter is usually
// just vertical strokes.
static int row_to_drop(const FitRows buf, const int *rows, int n, int x0, int x1) {
  int best = n / 2, best_cost = 1 << 30;
  for (int i = 1; i < n - 1; i++) {
    int up = 0, down = 0;
    for (int x = x0; x <= x1; x++) {
      up += buf[rows[i]][x] != buf[rows[i - 1]][x];
      down += buf[rows[i]][x] != buf[rows[i + 1]][x];
    }
    int like = up < down ? up : down;  // differences from the most similar neighbour
    int bar = longest_run(buf, rows[i], x0, x1) >= BAR_RUN ? 1000 : 0;
    int middle = abs(2 * i - (n - 1));
    int cost = bar + like * 10 + middle;
    if (cost < best_cost) { best_cost = cost; best = i; }
  }
  return best;
}

static void draw_fitted_text(GContext *ctx, const char *text, GRect cell, const int8_t *letter_w,
                             int height, GColor color, int density) {
  // Gothic 18 bold capitals start 7px below the text box's top.
  GRect box = GRect(cell.origin.x - 1, cell.origin.y - 7, cell.size.w + 2, 22);
  draw_text(ctx, text, box, GTextAlignmentCenter, color);
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  int x0 = cell.origin.x, x1 = x0 + cell.size.w - 1;
  int y0 = cell.origin.y, y1 = y0 + cell.size.h - 1;

  // Ink bounding box inside the cell.
  int ix0 = x1, ix1 = x0, iy0 = y1, iy1 = y0;
  for (int y = y0; y <= y1; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    for (int x = x0; x <= x1; x++) {
      if (row.data[x] != color.argb) continue;
      if (x < ix0) ix0 = x;
      if (x > ix1) ix1 = x;
      if (y < iy0) iy0 = y;
      if (y > iy1) iy1 = y;
    }
  }
  int w = ix1 - ix0 + 1, h = iy1 - iy0 + 1;
  if (w <= 0 || h <= 0 || w > FIT_MAX_W || h > FIT_MAX_H) {
    graphics_release_frame_buffer(ctx, fb);
    return;
  }
  static FitRows buf, out;
  for (int y = 0; y < h; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, iy0 + y);
    for (int x = 0; x < w; x++) {
      buf[y][x] = row.data[ix0 + x] == color.argb;  // 1 = ink
      row.data[ix0 + x] = s_lcd.argb;
    }
  }

  // Re-sample one letter (a run of columns with ink) at a time into `out`.
  int rows_out = h > height ? height : h;
  int kept = 0, letter = 0, prev_end = 0;
  for (int x = 0; x < w && letter < FIT_MAX_LETTERS;) {
    bool ink = false;
    for (int y = 0; y < h; y++) ink |= buf[y][x];
    if (!ink) { x++; continue; }
    int start = x;
    for (; x < w; x++) {
      bool col = false;
      for (int y = 0; y < h; y++) col |= buf[y][x];
      if (!col) break;
    }
    int len = x - start, last = x - 1;

    // Vertical: drop rows from this letter until it fits.
    int rows[FIT_MAX_H], n = h;
    for (int i = 0; i < h; i++) rows[i] = i;
    while (n > height) {
      int drop = row_to_drop(buf, rows, n, start, last);
      for (int i = drop; i < n - 1; i++) rows[i] = rows[i + 1];
      n--;
    }

    // Horizontal: keep the letter's width (and the text's own spacing), or
    // stretch it, with stems trimmed back to the bold font's 2px (bars by 1px).
    int tw = len, gap = 0;
    if (letter_w) {
      tw = letter_w[letter] > len ? letter_w[letter] : len;
      gap = kept > 0 ? 2 : 0;
    } else if (kept > 0) {
      gap = start - prev_end - 1;
    }
    if (kept + gap + tw > FIT_MAX_W) break;
    for (int y = 0; y < rows_out; y++) {
      for (int i = 0; i < gap; i++) out[y][kept + i] = 0;
      for (int i = 0; i < tw; i++) out[y][kept + gap + i] = buf[rows[y]][start + i * len / tw];
      if (tw == len) continue;
      int run_len = 0;
      for (int i = 0; i <= tw; i++) {
        if (i < tw && out[y][kept + gap + i]) { run_len++; continue; }
        int trim = (run_len >= 3 && run_len <= 5) ? run_len - 2 : (run_len > 5 ? 1 : 0);
        for (int k = 1; k <= trim; k++) out[y][kept + gap + i - k] = 0;
        run_len = 0;
      }
    }
    kept += gap + tw;
    prev_end = last;
    letter++;
  }

  int ox = (x0 + x1 + 1) / 2 - kept / 2, oy = y0 + (cell.size.h - rows_out) / 2;
  for (int y = 0; y < rows_out; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, oy + y);
    for (int i = 0; i < kept; i++) {
      bool lit = out[y][i] && BAYER4[(oy + y) & 3][(ox + i) & 3] < density;
      row.data[ox + i] = lit ? color.argb : s_lcd.argb;
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

static bool is_24h(void) { return !s_settings.twelve_hour; }

// The indicator box's outline. Like the W-221H, the top-right and bottom-left
// corners are rounded (radius BOX_RADIUS); the outer frame is 2px, the inner
// dividers are 1px.
static void draw_indicator_frame(void) {
  // RING: one quadrant of a 2px ring of radius 6 around the corner's centre,
  // as (dx, dy) offsets (pixel centres 4.5 <= distance < 6.5).
  _Static_assert(BOX_RADIUS == 6, "RING is drawn for radius 6");
  static const int8_t RING[][2] = {
    {0, 5}, {0, 6}, {1, 5}, {1, 6}, {2, 5}, {2, 6}, {3, 4}, {3, 5}, {4, 3}, {4, 4},
    {4, 5}, {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {6, 0}, {6, 1}, {6, 2},
  };
  const int r = BOX_RADIUS;
  const int ur_x = BOX_RIGHT - r, ur_y = BOX_TOP + r;    // top-right corner centre
  const int ll_x = BOX_LEFT + r, ll_y = BOX_BOTTOM - r;  // bottom-left corner centre
  fill(BOX_LEFT, BOX_TOP, ur_x - BOX_LEFT, 2, s_ink, false);              // top
  fill(BOX_RIGHT - 1, ur_y, 2, BOX_BOTTOM - ur_y + 1, s_ink, false);      // right
  fill(ll_x + 1, BOX_BOTTOM - 1, BOX_RIGHT - ll_x, 2, s_ink, false);      // bottom
  fill(BOX_LEFT, BOX_TOP, 2, ll_y - BOX_TOP, s_ink, false);               // left
  for (unsigned i = 0; i < ARRAY_LENGTH(RING); i++) {
    fill(ur_x + RING[i][0], ur_y - RING[i][1], 1, 1, s_ink, false);
    fill(ll_x - RING[i][0], ll_y + RING[i][1], 1, 1, s_ink, false);
  }
  // Inner dividers: solid 1px lines, meeting the inside of the frame.
  fill(BOX_LEFT + 2, BOX_MID, BOX_RIGHT - BOX_LEFT - 3, 1, s_ink, false);
  fill(BOX_DIV, BOX_TOP + 2, 1, BOX_BOTTOM - BOX_TOP - 3, s_ink, false);
}

// The big time, with the PM marker.
static void draw_time(void) {
  bool is24 = is_24h();
  int hour = s_now.tm_hour;
  bool pm = hour >= 12;
  if (!is24) {
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  const int ty = TIME_Y, tw = TIME_W, th = TIME_H;
  draw_dots(11, ty, PM_BITS, 11, 14, 1, 1, !is24 && pm);
  draw_digit(24, ty, tw, th, (is24 || hour >= 10) ? hour / 10 : DIGIT_BLANK);
  draw_digit(62, ty, tw, th, hour % 10);
  // The colon's dots follow the digits' slant; their centres sit 22px from the
  // top and 22px from the bottom of the digit height.
  fill(101 + (th - 22) * LCD_SLANT / 1000, ty + 19, 6, 6, s_ink, false);
  fill(101 + 22 * LCD_SLANT / 1000, ty + th - 25, 6, 6, s_ink, false);
  draw_digit(114, ty, tw, th, s_now.tm_min / 10);
  draw_digit(152, ty, tw, th, s_now.tm_min % 10);
}

// The date as "DD-MM" or "MM-DD". Like the W-221H, it is ~84% the height of the
// right-hand digits, centred vertically between the rule and the LCD's edge.
static void draw_date(void) {
  int first = s_settings.day_first ? s_now.tm_mday : s_now.tm_mon + 1;
  int second = s_settings.day_first ? s_now.tm_mon + 1 : s_now.tm_mday;
  const int w = 16, h = 30, top = 156, box_h = LCD_Y + LCD_H - top;
  const int y = top + (box_h - h) / 2;
  draw_digit(17, y, w, h, first >= 10 ? first / 10 : DIGIT_BLANK);
  draw_digit(36, y, w, h, first % 10);
  draw_segments(54, y, 10, h, SEG_G, SEG_G);  // dash: the font's middle bar
  draw_digit(66, y, w, h, second / 10);
  draw_digit(85, y, w, h, second % 10);
}

// Right box, option 1: the seconds, two digits centred in the box.
static void draw_seconds(void) {
  draw_digit(131, ROW3_Y, 22, ROW3_H, s_now.tm_sec / 10);
  draw_digit(156, ROW3_Y, 22, ROW3_H, s_now.tm_sec % 10);
}

// Right box, option 2: the temperature. A half-width sign slot (minus, or the
// "1" of 100+ in Fahrenheit) followed by two digits, so -99..199 all fit; "--"
// while there is no recent weather.
static void draw_temperature(void) {
  const int dy = ROW3_Y, dh = ROW3_H;
  int temp = s_weather.temp, v = abs(temp);
  bool valid = weather_valid();
  bool neg = valid && temp < 0, hundred = valid && v >= 100;
  // The minus is the font's middle bar in a narrow box; the "1" is the right
  // verticals of a full-width digit placed so they land in the sign slot.
  // Unlit parts go first so the lit one is never covered by a ghost.
  if (!neg) draw_segments(TEMP_X + 111, dy, 16, dh, 0, SEG_G);
  if (!hundred) draw_segments(TEMP_X + 102, dy, 22, dh, 0, SEG_B | SEG_C);
  if (neg) draw_segments(TEMP_X + 111, dy, 16, dh, SEG_G, SEG_G);
  if (hundred) draw_segments(TEMP_X + 102, dy, 22, dh, SEG_B | SEG_C, SEG_B | SEG_C);
  if (!valid) {
    draw_digit(TEMP_X + 129, dy, 22, dh, DIGIT_MINUS);
    draw_digit(TEMP_X + 154, dy, 22, dh, DIGIT_MINUS);
  } else {
    int tens = (v >= 10) ? v / 10 % 10 : DIGIT_BLANK;
    draw_digit(TEMP_X + 129, dy, 22, dh, tens);
    draw_digit(TEMP_X + 154, dy, 22, dh, v % 10);
  }
  // Degree mark: a round 7x7 ring, 2px thick, shifted with the digits' slanted
  // top edge.
  static const char DEGREE_BITS[] =
    "..###.."
    ".#####."
    "##...##"
    "##...##"
    "##...##"
    ".#####."
    "..###..";
  draw_dots(TEMP_X + 180 + (dh - 4) * LCD_SLANT / 1000, dy, DEGREE_BITS, 7, 7, 1, 1, true);
}

// Everything on the white LCD panel, drawn straight into the framebuffer.
static void draw_lcd(void) {
  draw_day(WEEKDAY_X, WEEKDAY_Y, DAYS[s_now.tm_wday]);
  draw_indicator_frame();
  draw_time();
  fill(LCD_X, ROW3_LINE_Y, LCD_W, 2, s_ink, false);
  fill(ROW3_DIV_X, ROW3_LINE_Y, 2, LCD_Y + LCD_H - ROW3_LINE_Y, s_ink, false);
  draw_date();
  if (s_settings.show_seconds) draw_seconds();
  else draw_temperature();
}

// Top bezel (smaller Gothic 18 bold font): the battery level and step count, or
// the custom texts when those are switched off. The right text takes the width
// it needs (up to TOP_RIGHT_MAX) and the left text gets the rest.
static void draw_top_bezel(GContext *ctx) {
  char left[24], right[24];
  if (s_settings.show_battery) {
    snprintf(left, sizeof(left), "BATT %d%%", s_battery.charge_percent);
  } else {
    snprintf(left, sizeof(left), "%s", s_settings.top_left);
  }
  if (!s_settings.show_steps) {
    snprintf(right, sizeof(right), "%s", s_settings.top_right);
  } else if (s_steps >= 1000) {
    snprintf(right, sizeof(right), "%d,%03d STEPS", s_steps / 1000, s_steps % 1000);
  } else if (s_steps >= 0) {
    snprintf(right, sizeof(right), "%d STEPS", s_steps);
  } else {
    snprintf(right, sizeof(right), "-- STEPS");
  }
  int right_w = graphics_text_layout_get_content_size(
      right, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), GRect(0, 0, 190, 22),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentRight).w + 4;
  if (right_w > TOP_RIGHT_MAX) right_w = TOP_RIGHT_MAX;
  draw_text(ctx, right, GRect(190 - right_w, -2, right_w, 22), GTextAlignmentRight, s_frame_text);
  draw_text(ctx, left, GRect(10, -2, 190 - right_w - 14, 22), GTextAlignmentLeft, s_frame_text);
}

// Bottom bezel (larger text): the WR badge (or HR badge and heart rate) and the
// custom label.
static void draw_bottom_bezel(GContext *ctx) {
  graphics_context_set_stroke_color(ctx, s_frame_text);
  graphics_draw_round_rect(ctx, GRect(10, BOTTOM_CAP - 3, 30, 20), 3);
  draw_bezel_text(ctx, s_settings.heart_rate ? "HR" : "WR", 10, BOTTOM_CAP, 30, GTextAlignmentCenter);
  if (s_settings.heart_rate) {
    char hr_text[12];
    if (s_hr > 0) snprintf(hr_text, sizeof(hr_text), "%d", s_hr);
    else snprintf(hr_text, sizeof(hr_text), "--");
    draw_bezel_text(ctx, hr_text, 44, BOTTOM_CAP, 36, GTextAlignmentLeft);
  }
  draw_bezel_text(ctx, s_settings.bezel_label, 82, BOTTOM_CAP, 108, GTextAlignmentRight);
}

// The indicator box's labels: lit (ink) when active, faint otherwise. Each is
// fitted to LABEL_H rows and centred vertically in its cell (the cells are the
// rows between the 2px frame and the middle divider); "BT" is also widened. The
// CHG and 24H cells stop 1px short of the rounded corners that intrude.
static void draw_indicator_labels(GContext *ctx) {
  const int top_h = BOX_MID - BOX_TOP - 2, bottom_h = BOX_BOTTOM - BOX_MID - 2;
  const int left_w = BOX_DIV - BOX_LEFT, right_w = BOX_RIGHT - BOX_DIV;
  static const int8_t BT_WIDTHS[] = { 8, 9 };  // widened B and T
  struct { const char *label; bool on; GRect cell; const int8_t *letter_w; } ind[] = {
    { "BT",   s_connected,
      GRect(BOX_LEFT + 4, BOX_TOP + 2, left_w - 4, top_h), BT_WIDTHS },
    { "CHG",  s_battery.is_charging,
      GRect(BOX_DIV + 1, BOX_TOP + 2, right_w - 4, top_h), NULL },
    { "24H",  is_24h(),
      GRect(BOX_LEFT + 4, BOX_MID + 1, left_w - 4, bottom_h), NULL },
    { "MUTE", quiet_time_is_active(),
      GRect(BOX_DIV + 1, BOX_MID + 1, right_w - 2, bottom_h), NULL },
  };
  for (unsigned i = 0; i < ARRAY_LENGTH(ind); i++) {
    if (!ind[i].on && !s_settings.ghosts) continue;
    draw_fitted_text(ctx, ind[i].label, ind[i].cell, ind[i].letter_w, LABEL_H,
                     ind[i].on ? s_ink : s_ghost, ind[i].on ? DENSITY_FULL : s_label_off_density);
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  // Case frame and LCD window.
  graphics_context_set_fill_color(ctx, s_frame);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(LCD_X, LCD_Y - 2, LCD_W, LCD_H + 4), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, s_lcd);
  graphics_fill_rect(ctx, GRect(LCD_X, LCD_Y, LCD_W, LCD_H), 0, GCornerNone);
  // A black LCD (inverted) would melt into a black case: mark the panel's edges.
  if (s_settings.inverted && s_frame.argb == GColorBlackARGB8) {
    graphics_context_set_fill_color(ctx, GColorDarkGray);
    graphics_fill_rect(ctx, GRect(LCD_X, LCD_Y - 1, LCD_W, 1), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(LCD_X, LCD_Y + LCD_H, LCD_W, 1), 0, GCornerNone);
  }

  s_fb = graphics_capture_frame_buffer(ctx);
  if (!s_fb) return;
  draw_lcd();
  graphics_release_frame_buffer(ctx, s_fb);
  s_fb = NULL;

  // Text goes through the regular text API, so it comes after the framebuffer
  // is released.
  draw_top_bezel(ctx);
  draw_bottom_bezel(ctx);
  draw_indicator_labels(ctx);
}

// ---------------------------------------------------------------------------
// Services

static void request_weather(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, 1);
  app_message_outbox_send();
}

static void tick_handler(struct tm *now, TimeUnits changed);

// Once a minute normally; every second when the seconds are shown.
static void subscribe_ticks(void) {
  tick_timer_service_unsubscribe();
  tick_timer_service_subscribe(s_settings.show_seconds ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
}

static void tick_handler(struct tm *now, TimeUnits changed) {
  s_now = *now;
  if (changed & MINUTE_UNIT) {
    update_steps();
    if (now->tm_min % WEATHER_REFRESH_MIN == 0) request_weather();
  }
  layer_mark_dirty(s_canvas);
}

static void battery_handler(BatteryChargeState state) {
  s_battery = state;
  layer_mark_dirty(s_canvas);
}

static void play_vibe(uint8_t pattern) {
  static const uint32_t TRIPLE[] = { 150, 100, 150, 100, 150 };
  static const uint32_t HEARTBEAT[] = { 80, 120, 200 };
  static const uint32_t SOS[] = { 100, 100, 100, 100, 100, 300, 300, 100, 300, 100, 300, 300,
                                  100, 100, 100, 100, 100 };
  switch (pattern) {
    case VIBE_SHORT:  vibes_short_pulse(); break;
    case VIBE_LONG:   vibes_long_pulse(); break;
    case VIBE_DOUBLE: vibes_double_pulse(); break;
    case VIBE_TRIPLE:
      vibes_enqueue_custom_pattern(
          (VibePattern){ .durations = TRIPLE, .num_segments = ARRAY_LENGTH(TRIPLE) });
      break;
    case VIBE_HEARTBEAT:
      vibes_enqueue_custom_pattern(
          (VibePattern){ .durations = HEARTBEAT, .num_segments = ARRAY_LENGTH(HEARTBEAT) });
      break;
    case VIBE_SOS:
      vibes_enqueue_custom_pattern((VibePattern){ .durations = SOS, .num_segments = ARRAY_LENGTH(SOS) });
      break;
    default: break;
  }
}

static void connection_handler(bool connected) {
  if (connected != s_connected && !quiet_time_is_active()) {
    play_vibe(connected ? s_settings.vibe_connect : s_settings.vibe_disconnect);
  }
  s_connected = connected;
  layer_mark_dirty(s_canvas);
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate || event == HealthEventSignificantUpdate) {
    update_steps();
    layer_mark_dirty(s_canvas);
  } else if (event == HealthEventHeartRateUpdate) {
    update_heart_rate();
    if (s_settings.heart_rate) layer_mark_dirty(s_canvas);
  }
}
#endif

// Reads a preset colour such as "FFA020" into *rgb. Returns false, leaving *rgb
// alone, unless it is exactly six hex digits.
static bool parse_hex(const char *hex, uint32_t *rgb) {
  uint32_t value = 0;
  int digits = 0;
  for (; *hex; hex++, digits++) {
    char ch = *hex;
    int d = (ch >= '0' && ch <= '9') ? ch - '0'
          : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
          : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
    if (d < 0) return false;
    value = (value << 4) | d;
  }
  if (digits != 6) return false;
  *rgb = value;
  return true;
}

// Copies a settings-page text into a fixed buffer, upper-cased to match the
// other printed text.
static void copy_upper(char *dst, size_t size, const char *src) {
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
  for (char *c = dst; *c; c++) {
    if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
  }
}

static int tuple_int(Tuple *t) {
  if (t->type == TUPLE_CSTRING) return atoi(t->value->cstring);
  return (int)t->value->int32;
}

static void inbox_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool settings_changed = false;

  if ((t = dict_find(iter, MESSAGE_KEY_Temp))) {
    s_weather.temp = tuple_int(t);
    s_weather.updated = time(NULL);
    persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
  }
  // Time & date
  if ((t = dict_find(iter, MESSAGE_KEY_TimeFormat))) {
    s_settings.twelve_hour = strcmp(t->value->cstring, "12") == 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DateFormat))) {
    s_settings.day_first = strcmp(t->value->cstring, "DM") == 0;
    settings_changed = true;
  }
  // Right box
  if ((t = dict_find(iter, MESSAGE_KEY_RightBox))) {
    s_settings.show_seconds = strcmp(t->value->cstring, "seconds") == 0;
    settings_changed = true;
  }
  // Top bezel
  if ((t = dict_find(iter, MESSAGE_KEY_ShowBattery))) {
    s_settings.show_battery = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TopLeftText))) {
    copy_upper(s_settings.top_left, sizeof(s_settings.top_left), t->value->cstring);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ShowSteps))) {
    s_settings.show_steps = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TopRightText))) {
    copy_upper(s_settings.top_right, sizeof(s_settings.top_right), t->value->cstring);
    settings_changed = true;
  }
  // Bottom bezel
  if ((t = dict_find(iter, MESSAGE_KEY_HeartRate))) {
    s_settings.heart_rate = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_BezelLabel))) {
    copy_upper(s_settings.bezel_label, sizeof(s_settings.bezel_label), t->value->cstring);
    settings_changed = true;
  }
  // Appearance
  if ((t = dict_find(iter, MESSAGE_KEY_CaseColor))) {
    s_settings.silver = strcmp(t->value->cstring, "silver") == 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Inverted))) {
    s_settings.inverted = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Ghosts))) {
    s_settings.ghosts = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_BacklightColor))) {
    // "system", "custom" (colour in BacklightCustom), or a hex colour such as "FFA020".
    const char *choice = t->value->cstring;
    uint32_t rgb;
    if (strcmp(choice, "system") == 0) s_settings.backlight = BACKLIGHT_SYSTEM;
    else if (strcmp(choice, "custom") == 0) s_settings.backlight = BACKLIGHT_CUSTOM;
    else if (parse_hex(choice, &rgb)) s_settings.backlight = rgb;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_BacklightCustom))) {
    s_settings.backlight_custom = (uint32_t)tuple_int(t) & 0xFFFFFF;  // 0xRRGGBB from the picker
    settings_changed = true;
  }
  // Alerts
  if ((t = dict_find(iter, MESSAGE_KEY_VibeDisconnect))) {
    int v = tuple_int(t);
    s_settings.vibe_disconnect = (v >= 0 && v < VIBE_COUNT) ? v : VIBE_NONE;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_VibeConnect))) {
    int v = tuple_int(t);
    s_settings.vibe_connect = (v >= 0 && v < VIBE_COUNT) ? v : VIBE_NONE;
    settings_changed = true;
  }

  if (settings_changed) {
    persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
    apply_theme();
    apply_backlight();
    subscribe_ticks();
  }
  layer_mark_dirty(s_canvas);
}

// ---------------------------------------------------------------------------
// Lifecycle

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  s_settings = (Settings){
    .day_first = true,
    .show_battery = true,
    .top_left = "3 DAY BATTERY",
    .show_steps = true,
    .top_right = "WR 3ATM",
    .bezel_label = "PEBBLE",
    .ghosts = true,
    .backlight = BACKLIGHT_SYSTEM,
    .vibe_disconnect = VIBE_DOUBLE,
    .vibe_connect = VIBE_SHORT,
  };
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  persist_read_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
  apply_theme();
  apply_backlight();

  time_t now = time(NULL);
  s_now = *localtime(&now);
  s_battery = battery_state_service_peek();
  s_connected = connection_service_peek_pebble_app_connection();
  update_steps();
  update_heart_rate();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);

  subscribe_ticks();
  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe(
      (ConnectionHandlers){ .pebble_app_connection_handler = connection_handler });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif

  app_message_register_inbox_received(inbox_handler);
  // The inbox must hold a full settings Save: about 230 bytes with plain text and
  // over 400 with emoji in the three custom texts.
  app_message_open(512, 64);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
