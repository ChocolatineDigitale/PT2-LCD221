// LCD 221: a watch face for the Pebble Time 2 (emery, 200x228) that imitates
// the Casio W-221H: a white "LCD" with slanted 7-segment digits, a dot-matrix
// weekday and a 2x2 indicator box, between a top and a bottom bezel.
//
// Everything on the LCD panel is rasterized straight into the framebuffer, which
// allows things the drawing API can't do: polygon segments with a slant and
// gray-shade anti-aliasing, and faint "ghost" segments for the unlit parts (a
// sparse ordered dither). Only the bezel text and indicator labels go through
// the regular text API (the bezel text is then re-sampled and cached, see "Bezel text").
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
// persisted; bump SETTINGS_KEY whenever the layout of existing Settings fields changes.

#include <pebble.h>
#include "segments.h"

// The 7-segment digits (time, date, temperature) lean like the W-221H's: LCD_SLANT
// is the lean in thousandths of the height (75 = 7.5%, as measured on the
// original). LCD_AA smooths the edges of the slanted digits with the display's gray
// shades. Upright digits (the "Slanted digits" setting off) are drawn without
// smoothing: their edges are vertical and horizontal, so plain pixels are sharper.
#define LCD_SLANT 75
#define LCD_AA true
#define SETTINGS_KEY 18  // bumped whenever existing Settings fields change layout (new fields may use free padding)
#define WEATHER_KEY 8  // bumped whenever Weather changes layout
#define WEATHER_MIN_MAX_AGE (3 * 60 * 60)  // shown as valid for this long, or twice the refresh interval if longer
#define WEATHER_REFRESH_DEFAULT 30  // minutes (setting: 30 to 720)
#define SECONDS_BURST_DEFAULT_S 30  // how long the seconds tick after a shake (setting: 5..120)
#define SECONDS_BURST_MIN_S 5
#define SECONDS_BURST_MAX_S 120

#define CHIME_VOLUME_DEFAULT 70  // 0-100

#define BACKLIGHT_SYSTEM 0xFFFFFFFFu  // the user's normal backlight colour
#define BACKLIGHT_CUSTOM 0xFFFFFFFEu  // use backlight_custom

// Vibration patterns selectable for phone connect/disconnect.
typedef enum {
  VIBE_NONE, VIBE_SHORT, VIBE_LONG, VIBE_DOUBLE, VIBE_TRIPLE, VIBE_HEARTBEAT, VIBE_SOS, VIBE_COUNT
} VibeChoice;

// Hourly chime choices; the values are what the settings page sends.
typedef enum {
  CHIME_OFF = 0, CHIME_LCD_CLASSIC = 1, CHIME_DOORBELL = 2,
  // 3 was retired; the values stay as they were so saved settings keep their meaning
  CHIME_VIBE = 4, CHIME_BIG_BEN = 5, CHIME_SUPER = 6,
  CHIME_COUNT
} ChimeChoice;

// Time format and temperature unit: follow the watch, or force one.
enum { FORMAT_AUTO = 0, FORMAT_24H = 1, FORMAT_12H = 2 };
enum { UNIT_AUTO = 0, UNIT_C = 1, UNIT_F = 2 };
// Date padding: zeros, a blank for the first number only, or blanks for both numbers.
enum { PAD_ZERO = 0, PAD_FIRST_BLANK = 1, PAD_BOTH_BLANK = 2 };
// What a corner of the top or bottom bezel shows. The names are what the settings page sends.
// From CORNER_WX on, a corner shows a weather reading: see WX_CORNERS.
enum { CORNER_NOTHING = 0, CORNER_BATTERY, CORNER_STEPS, CORNER_DISTANCE, CORNER_HEART_RATE,
       CORNER_TEXT, CORNER_WR, CORNER_WX };
static const char CORNER_NAMES[CORNER_WX][9] = {
  "none", "battery", "steps", "distance", "hr", "text", "wr",
};
// The readings the phone sends (in this order: see src/pkjs/index.js), each a 16-bit number.
enum { WX_TEMP, WX_FEELS, WX_HIGH, WX_LOW, WX_DEW, WX_HUMIDITY, WX_RAIN, WX_CHANCE, WX_SNOWFALL,
       WX_SNOWDEPTH, WX_WIND, WX_GUST, WX_DIR, WX_PRESSURE, WX_CLOUD, WX_VIS, WX_UV, WX_UVMAX,
       WX_SUNRISE, WX_SUNSET, WX_COND, WX_COUNT };
enum { AQ_US, AQ_EU, AQ_PM25, AQ_PM10, AQ_O3, AQ_NO2, AQ_SO2, AQ_CO, AQ_DUST, AQ_COUNT };
#define WX_NONE INT16_MIN  // a reading the phone did not have
enum { SLOT_TOP_LEFT = 0, SLOT_TOP_RIGHT, SLOT_BOTTOM_LEFT, SLOT_BOTTOM_RIGHT, SLOTS };
enum { DIST_AUTO = 0, DIST_KM = 1, DIST_MI = 2 };
enum { WUNITS_AUTO = 0, WUNITS_METRIC = 1, WUNITS_IMPERIAL = 2 };  // wind and rain

// Everything the settings page controls, grouped and ordered like the page itself
// (config.js). Saved with persist_write_data: bump SETTINGS_KEY when it changes.
typedef struct {
  // Time & date
  uint8_t time_format;         // FORMAT_AUTO follows the watch's 12/24-hour setting
  bool day_first;
  // Right box
  bool show_seconds;           // seconds instead of the temperature (always, or after a shake: see below)
  uint8_t temp_unit;           // UNIT_AUTO follows the watch's measurement system
  // Top and bottom bezel: what each of the four corners shows
  uint8_t corner[SLOTS];       // CORNER_*
  char corner_text[SLOTS][20]; // the text of a CORNER_TEXT corner, upper-cased
  uint8_t dist_unit;           // DIST_AUTO follows the watch's measurement system
  // Appearance
  bool silver;                 // silver case instead of black
  bool inverted;               // light digits on a dark LCD
  bool slanted;                // digits lean like the original's (off: upright and sharper)
  bool ghosts;                 // show faint unlit segments
  uint32_t backlight;          // 0xRRGGBB tint, BACKLIGHT_SYSTEM, or BACKLIGHT_CUSTOM
  uint32_t backlight_custom;   // 0xRRGGBB, from the settings page's colour picker
  // Alerts
  uint8_t vibe_disconnect;     // VibeChoice played when the phone disconnects
  uint8_t vibe_connect;        // VibeChoice played when it reconnects
  uint8_t chime;               // ChimeChoice played on the hour
  bool chime_quiet;            // stay silent during Quiet Time
  uint8_t chime_volume;        // 0-100
  // Added after the groups above, at the end, so settings saved by earlier versions still load.
  // (They sit in what used to be padding, so the struct keeps its size; init() sanitises them.)
  uint8_t seconds_on_shake;    // 1: seconds tick only for a while after a wrist shake
  uint8_t seconds_burst_s;     // how many seconds they tick for
  uint8_t hour_no_zero;        // 1: 24-hour time has no leading zero (7:05, like the original)
  uint8_t date_pad;            // single-digit date numbers: PAD_ZERO (06-05), PAD_FIRST_BLANK ( 6-05), PAD_BOTH_BLANK ( 6- 5)
  uint8_t button_markers;       // 1: red marks along the LCD edges, where the watch's buttons are
  uint8_t weather_refresh;     // minutes between weather updates, divided by 10 (3 = 30 minutes)
  uint8_t weather_units;       // WUNITS_*: units of the wind speed and the rain
  uint8_t aqi_eu;              // 1: the European air quality index, 0: the US one
} Settings;

typedef struct {
  int16_t wx[WX_COUNT];        // the weather readings, WX_NONE if unknown (units: see index.js)
  int16_t aq[AQ_COUNT];        // the air quality readings, in a message of their own
  time_t updated;
  time_t aqi_updated;
} Weather;

static Window *s_window;
static Layer *s_canvas;
static GBitmap *s_fb;

static Settings s_settings;
static Weather s_weather;
static time_t s_weather_saved;       // when s_weather was last written to persistent storage
static bool s_focus = true;          // the face is the app in front (not covered by a system window)
static bool s_quiet, s_muted;        // Quiet Time / speaker mute, read once per redraw
static time_t s_burst_until;         // seconds_on_shake: the seconds show until this time (0 = idle)
static struct tm s_now;
static BatteryChargeState s_battery;
static bool s_connected;
static int s_steps = -1;
static int s_distance_m = -1;  // metres walked today, -1 if unavailable
static int s_hr = -1;  // latest heart rate in BPM, -1 if unavailable

// Theme (see apply_theme). Normal: black ink on a white LCD. Inverted: white ink
// on a black LCD, with every shade swapped accordingly.
static GColor s_ink, s_lcd;          // digits, lines and lit text; the LCD's background
static GColor s_ghost;               // unlit segments and dots (drawn dithered)
static GColor s_frame, s_frame_text; // case (bezels) and the text printed on it
static uint8_t s_aa[3];              // anti-aliasing shades: faint, medium, full ink
static int s_ghost_density;          // dot density of s_ghost, out of DENSITY_FULL
static int s_slant;                  // digit lean in thousandths of the height (0 = upright)
static bool s_smooth;                // anti-alias the digits' edges (only while slanted)
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

// Upright digits: maps a 0..1000 coordinate to whole pixels so that every bar of a digit
// has the same thickness (plain scaling gives bars of 8 and 9 pixels side by side).
// `knots` are the bar edges in font units and `px` where they land, in pixels.
static int snap_axis(int v, int size, const int16_t *knots, const int *px, int count) {
  int k = 0;
  while (k + 2 < count && v > knots[k + 1]) k++;
  int a = k == 0 ? 0 : px[k], b = px[k + 1];
  int ka = k == 0 ? 0 : knots[k], kb = knots[k + 1];
  int32_t at = a * 1000 + (kb == ka ? 0 : (int32_t)(v - ka) * (b - a) * 1000 / (kb - ka));  // fits 32 bits: at most 1000 * 96 * 1000
  return (int)((at + size / 2) / size);  // font units of the snapped pixel position
}

static void snap_poly(int w, int h, const int16_t *pts, int n, int16_t *out) {
  static const int16_t KX[] = { 0, 243, 756, 1000 };
  static const int16_t KY[] = { 0, 125, 438, 562, 875, 1000 };
  int tv = (244 * w + 500) / 1000, th = (125 * h + 500) / 1000;
  int tg = ((h - th) & 1) ? th - 1 : th;
  int px[4] = { 0, tv, w - tv, w };
  int py[6] = { 0, th, (h - tg) / 2, (h + tg) / 2, h - th, h };
  for (int i = 0; i < n; i++) {
    int xp = snap_axis(pts[2 * i], w, KX, px, 4);
    int yp = snap_axis(pts[2 * i + 1], h, KY, py, 6);
    out[2 * i] = (int16_t)xp;
    out[2 * i + 1] = (int16_t)yp;
  }
}

// Fills a polygon given in 0..1000 coordinates, scaled into the box (x, y, w, h).
// A pixel is filled when its centre is inside (even-odd rule).
// `slant` leans the shape right like italics, in thousandths of the height:
// each row is shifted by its height above the bottom times slant/1000, with
// sub-pixel precision so edges stay clean diagonals.
// The rows (0..h-1) a polygon given in 0..1000 coordinates can touch, with a row of margin:
// segments cover only 12-48% of a digit's height, so most rows need no scanning at all.
static void poly_rows(const int16_t *pts, int n, int h, int *first, int *last) {
  int lo = 1000, hi = 0;
  for (int i = 0; i < n; i++) {
    if (pts[2 * i + 1] < lo) lo = pts[2 * i + 1];
    if (pts[2 * i + 1] > hi) hi = pts[2 * i + 1];
  }
  *first = lo * h / 1000 - 1;
  if (*first < 0) *first = 0;
  *last = hi * h / 1000 + 1;
  if (*last > h - 1) *last = h - 1;
}

static void fill_poly(int x, int y, int w, int h, const int16_t *pts, int n, int slant, GColor c,
                      bool dither) {
  int16_t snapped[16];
  if (slant == 0 && n <= 8) {
    snap_poly(w, h, pts, n, snapped);
    pts = snapped;
  }
  int first, last;
  poly_rows(pts, n, h, &first, &last);
  for (int r = first; r <= last; r++) {
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
  int first, last;
  poly_rows(pts, n, h, &first, &last);
  for (int r = first; r <= last; r++) {
    memset(cov, 0, sizeof(cov[0]) * cols);
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
// Jan Bobrowski (see segments.h), scaled to the box and slanted by s_slant. Only the
// segments in `present` are drawn; unlit ones appear as ghosts (if enabled),
// drawn first so lit segments always win.
static void draw_segments(int x, int y, int w, int h, uint8_t on, uint8_t present) {
  for (int pass = s_settings.ghosts ? 0 : 1; pass <= 1; pass++) {
    uint8_t mask = present & (pass ? on : (uint8_t)~on);
    for (int i = 0; i < 7; i++) {
      if (!(mask & (1 << i))) continue;
      if (pass && s_smooth) fill_poly_aa(x, y, w, h, SEG_POLYS[i], SEG_POLY_LEN[i], s_slant);
      else fill_poly(x, y, w, h, SEG_POLYS[i], SEG_POLY_LEN[i], s_slant,
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
// Each glyph is a list of rows, one bit per dot (bit 0 = the leftmost column).
static const struct { char ch; uint16_t rows[5]; } DAY_GLYPHS[] = {
  { 'A', { 14, 17, 31, 17, 17 } },
  { 'D', { 15, 17, 17, 17, 15 } },
  { 'E', { 31, 1, 15, 1, 31 } },
  { 'F', { 31, 1, 15, 1, 1 } },
  { 'H', { 17, 17, 31, 17, 17 } },
  { 'I', { 14, 4, 4, 4, 14 } },
  { 'M', { 17, 27, 21, 17, 17 } },
  { 'N', { 17, 19, 21, 25, 17 } },
  { 'O', { 14, 17, 17, 17, 14 } },
  { 'R', { 15, 17, 15, 9, 17 } },
  { 'S', { 30, 1, 14, 16, 15 } },
  { 'T', { 31, 4, 4, 4, 4 } },
  { 'U', { 17, 17, 17, 17, 14 } },
  { 'W', { 17, 17, 21, 27, 17 } },
};

static const uint16_t *day_glyph(char ch) {
  for (unsigned i = 0; i < ARRAY_LENGTH(DAY_GLYPHS); i++) {
    if (DAY_GLYPHS[i].ch == ch) return DAY_GLYPHS[i].rows;
  }
  return NULL;
}

// The "P" (PM) marker: an 11x14 LCD symbol with a square-ish bowl (corners cut by a dot)
// and a straight stem.
static const uint16_t PM_BITS[14] = { 511, 2047, 1799, 1799, 1799, 1799, 2047, 511, 7, 7, 7, 7, 7, 7 };

// Draws a grid of `cols` x `rows` dots. With `on` false, the '#' dots are drawn
// as ghosts instead (an unlit fixed LCD symbol).
static void draw_dots(int x, int y, const uint16_t *bits, int cols, int rows, int pitch, int dot, bool on) {
  if (!bits || (!on && !s_settings.ghosts)) return;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if (!((bits[r] >> c) & 1)) continue;
      fill(x + c * pitch, y + r * pitch, dot, dot, on ? s_ink : s_ghost, !on);
    }
  }
}

// Dot-matrix cell: lit dots solid, the rest as ghosts (if enabled). Dots are
// dot_w x dot_h rectangles on a px x py grid.
static void draw_matrix(int x, int y, const uint16_t *bits, int cols, int rows, int px, int py, int dot_w,
                        int dot_h) {
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      bool lit = bits && ((bits[r] >> c) & 1);
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

static int weather_refresh_min(void) {
  return s_settings.weather_refresh * 10;
}

// Whether a reading made at `when` is recent enough to show.
static bool reading_valid(time_t when) {
  int max_age = 2 * weather_refresh_min() * 60;
  if (max_age < WEATHER_MIN_MAX_AGE) max_age = WEATHER_MIN_MAX_AGE;
  return when != 0 && time(NULL) - when <= max_age;
}

static bool weather_valid(void) { return reading_valid(s_weather.updated); }

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

static void update_distance(void) {
#if defined(PBL_HEALTH)
  time_t start = time_start_of_today();
  time_t end = time(NULL);
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(HealthMetricWalkedDistanceMeters, start, end);
  s_distance_m = (mask & HealthServiceAccessibilityMaskAvailable)
      ? (int)health_service_sum_today(HealthMetricWalkedDistanceMeters) : -1;
#endif
}

// Whether any corner of the bezels shows this kind of data.
static bool corner_used(int type) {
  for (int i = 0; i < SLOTS; i++) {
    if (s_settings.corner[i] == type) return true;
  }
  return false;
}

static void apply_theme(void) {
  bool inv = s_settings.inverted;

  // The digits.
  s_slant = s_settings.slanted ? LCD_SLANT : 0;
  s_smooth = LCD_AA && s_settings.slanted;

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
#define LCD_H 180

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
// Proportions measured on the W-221H: the time is ~41% of the LCD height and sits
// just above the rule, its digits are ~7% wider than tall-relative to the LCD, and
// they are spread across nearly the whole width: the first digit's box starts about
// 4px from the edge, with the PM marker in the empty left part of that box. (The
// original's marker starts level with the digits and 6px from the edge; here it is
// nudged up and in a little.)
#define TIME_Y 80
#define TIME_W 35
#define TIME_H 70
#define PM_X 7    // the PM marker's distance from the LCD's left edge
#define PM_DY -2  // and how many rows it sits above the top of the digits

// Row 3: date (left) and seconds / temperature (right), under a 2px rule.
#define DST_X 4         // DST label cell, top left of the date cell
#define DST_Y 161
#define DST_W 34
#define DST_H 12
#define ROW3_LINE_Y 158
#define ROW3_DIV_X 110  // vertical divider between the date and the right box
#define ROW3_Y 164      // top of the right box's digits
#define ROW3_H 36       // height of the right box's digits
#define TEMP_X 3        // horizontal offset of the whole temperature group (sign, digits, degree)

// Bezels.
#define TOP_RIGHT_MAX 104  // widest the top-right bezel text may be
#define BOTTOM_RIGHT_MAX 108  // widest the bottom-right bezel text may be
#define WR_BADGE_W 28      // a badge (WR / HR)
#define BADGE_GAP 4        // between a badge and the value beside it
#define BEZEL_ROWS 14      // height of the bezel capitals, in pixels
#define TOP_CAP_Y ((24 - BEZEL_ROWS) / 2)               // first row of the top bezel's capitals
#define BOTTOM_CAP_Y (LCD_Y + LCD_H + (24 - BEZEL_ROWS) / 2 + 1)  // ... and of the bottom bezel's

// The weekday names, three letters each, in one string (a table of pointers would take RAM).
static const char DAYS[] = "SUNMONTUEWEDTHUFRISAT";

static void draw_text(GContext *ctx, const char *text, GRect box, GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), box,
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
#define FIT_MAX_W 40  // the widest indicator cell is 39 px,
#define FIT_MAX_H 12  // and every cell is 12 rows tall
#define FIT_MAX_LETTERS 8
_Static_assert(BOX_RIGHT - BOX_DIV - 2 <= FIT_MAX_W && BOX_DIV - BOX_LEFT - 4 <= FIT_MAX_W &&
               DST_W <= FIT_MAX_W, "an indicator cell is wider than the label scratch buffer");
_Static_assert(BOX_MID - BOX_TOP - 2 <= FIT_MAX_H && BOX_BOTTOM - BOX_MID - 2 <= FIT_MAX_H &&
               DST_H <= FIT_MAX_H, "an indicator cell is taller than the label scratch buffer");
#define BAR_RUN 4  // a horizontal run of this many ink pixels makes a row part of a bar

typedef uint8_t FitRows[FIT_MAX_H][FIT_MAX_W];

// Scratch space for re-sampling text. The labels (draw_fitted_text) and the bezel text
// (bezel_text_render) are never made at the same time, so they share it.
#define BZ_H 20             // tallest bezel text result (capitals and a comma)
#define BZ_MAX_LETTER 40    // widest bezel letter (or letters that touch)
static union {
  struct { FitRows buf, out; } fit;
  uint8_t letter[BZ_H][BZ_MAX_LETTER];
} s_scratch;

// The longest horizontal run of ink in one row of a letter (columns x0..x1).
static int longest_run(const uint8_t *buf, int stride, int y, int x0, int x1) {
  int best = 0, run = 0;
  for (int x = x0; x <= x1; x++) {
    run = buf[y * stride + x] ? run + 1 : 0;
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
static int row_to_drop(const uint8_t *buf, int stride, const int *rows, int n, int x0, int x1) {
  int best = n / 2, best_cost = 1 << 30;
  for (int i = 1; i < n - 1; i++) {
    int up = 0, down = 0;
    for (int x = x0; x <= x1; x++) {
      up += buf[rows[i] * stride + x] != buf[rows[i - 1] * stride + x];
      down += buf[rows[i] * stride + x] != buf[rows[i + 1] * stride + x];
    }
    int like = up < down ? up : down;  // differences from the most similar neighbour
    int bar = longest_run(buf, stride, rows[i], x0, x1) >= BAR_RUN ? 1000 : 0;
    int middle = abs(2 * i - (n - 1));
    int cost = bar + like * 10 + middle;
    if (cost < best_cost) { best_cost = cost; best = i; }
  }
  return best;
}

static void draw_fitted_text(GContext *ctx, const char *text, GRect cell, const int8_t *letter_w,
                             int letter_w_n, int height, GColor color, int density) {
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
  uint8_t (*buf)[FIT_MAX_W] = s_scratch.fit.buf, (*out)[FIT_MAX_W] = s_scratch.fit.out;
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
      int drop = row_to_drop(&buf[0][0], FIT_MAX_W, rows, n, start, last);
      for (int i = drop; i < n - 1; i++) rows[i] = rows[i + 1];
      n--;
    }

    // Horizontal: keep the letter's width (and the text's own spacing), or
    // stretch it, with stems trimmed back to the bold font's 2px (bars by 1px).
    int tw = len, gap = 0;
    if (letter_w) {
      tw = (letter < letter_w_n && letter_w[letter] > len) ? letter_w[letter] : len;
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

static bool is_24h(void) {
  if (s_settings.time_format == FORMAT_AUTO) return clock_is_24h_style();
  return s_settings.time_format == FORMAT_24H;
}

// The measurement system chosen on the watch.
static bool watch_is_imperial(void) {
#if defined(PBL_HEALTH)
  return health_service_get_measurement_system_for_display(HealthMetricWalkedDistanceMeters) ==
         MeasurementSystemImperial;
#else
  return false;
#endif
}

// Imperial units on the watch mean Fahrenheit when following it.
static bool use_fahrenheit(void) {
  if (s_settings.temp_unit != UNIT_AUTO) return s_settings.temp_unit == UNIT_F;
  return watch_is_imperial();
}

// Miles rather than kilometres: forced by the setting, else when the watch uses imperial units.
static bool use_miles(void) {
  if (s_settings.dist_unit != DIST_AUTO) return s_settings.dist_unit == DIST_MI;
  return watch_is_imperial();
}

// Wind speed and rain in miles per hour / inches rather than km/h / mm.
static bool use_imperial_weather(void) {
  if (s_settings.weather_units != WUNITS_AUTO) return s_settings.weather_units == WUNITS_IMPERIAL;
  return watch_is_imperial();
}

// A temperature in tenths of a degree Celsius, in the unit shown, rounded.
static int temp_in_unit(int t10) {
  if (use_fahrenheit()) {
    int f50 = t10 * 9 + 1600;  // fiftieths of a degree Fahrenheit, rounded only once below
    return (f50 + (f50 >= 0 ? 25 : -25)) / 50;
  }
  return (t10 + (t10 >= 0 ? 5 : -5)) / 10;
}

// The temperature in the right box.
static int display_temp(void) { return temp_in_unit(s_weather.wx[WX_TEMP]); }

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
  if (!is24) draw_dots(PM_X, ty + PM_DY, PM_BITS, 11, 14, 1, 1, pm);  // AM/PM only in 12-hour mode
  // Digit boxes (left edges) and the colon, spread like the original's.
  static const int X[4] = { 6, 49, 108, 153 };
  const int colon_x = 93;
  // 12-hour: the first digit is blank or a 1 (the P sits where a 0 would be). 24-hour: the
  // leading zero is optional.
  int tens = (hour >= 10 || (is24 && !s_settings.hour_no_zero)) ? hour / 10 : DIGIT_BLANK;
  if (is24) {
    draw_digit(X[0], ty, tw, th, tens);
  } else {
    // In 12-hour mode the first digit can only be a 1 (or blank), so it only has
    // the two right-hand segments, as on the real watch. This also keeps the PM
    // marker clear of an unlit digit.
    draw_segments(X[0], ty, tw, th, tens == 1 ? SEG_B | SEG_C : 0, SEG_B | SEG_C);
  }
  draw_digit(X[1], ty, tw, th, hour % 10);
  // The colon's dots follow the digits' slant. As on the original, their centres
  // sit 36% and 70% of the way down the digits (not symmetrically).
  const int dot = 7, dot1 = th * 36 / 100, dot2 = th * 70 / 100;  // dot size; centres below the top
  fill(colon_x + (th - dot1) * s_slant / 1000, ty + dot1 - dot / 2, dot, dot, s_ink, false);
  fill(colon_x + (th - dot2) * s_slant / 1000, ty + dot2 - dot / 2, dot, dot, s_ink, false);
  draw_digit(X[2], ty, tw, th, s_now.tm_min / 10);
  draw_digit(X[3], ty, tw, th, s_now.tm_min % 10);
}

// The date as "DD-MM" or "MM-DD". As on the W-221H, its digits are about 72% as
// tall as the right box's and share their baseline (so the time stands out); the
// last digit ends where it did before, ~19px short of the divider.
static void draw_date(void) {
  int first = s_settings.day_first ? s_now.tm_mday : s_now.tm_mon + 1;
  int second = s_settings.day_first ? s_now.tm_mon + 1 : s_now.tm_mday;
  const int w = 15, h = 26;
  const int y = ROW3_Y + ROW3_H - h;  // bottom aligned with the right box's digits
  const bool blank_first = s_settings.date_pad != PAD_ZERO;
  const bool blank_second = s_settings.date_pad == PAD_BOTH_BLANK;
  draw_digit(19, y, w, h, (first >= 10 || !blank_first) ? first / 10 : DIGIT_BLANK);
  draw_digit(38, y, w, h, first % 10);
  draw_segments(56, y, 8, h, SEG_G, SEG_G);  // dash: the font's middle bar
  draw_digit(67, y, w, h, (second >= 10 || !blank_second) ? second / 10 : DIGIT_BLANK);
  draw_digit(86, y, w, h, second % 10);
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
  int temp = display_temp(), v = abs(temp);
  bool valid = weather_valid() && s_weather.wx[WX_TEMP] != WX_NONE;
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
  static const uint16_t DEGREE_BITS[7] = { 28, 62, 99, 99, 99, 62, 28 };
  draw_dots(TEMP_X + 180 + (dh - 4) * s_slant / 1000, dy, DEGREE_BITS, 7, 7, 1, 1, true);
}

// A shake's burst of seconds is running. It also ends if the clock was set back meanwhile, so
// it can never last longer than seconds_burst_s.
static bool burst_active(void) {
  if (s_burst_until == 0) return false;
  const time_t now = time(NULL);
  return now < s_burst_until && s_burst_until - now <= s_settings.seconds_burst_s;
}

// Seconds in the right box: always, or (seconds_on_shake) only during a burst after a wrist
// shake, with the temperature there the rest of the time.
static bool seconds_showing(void) {
  if (!s_settings.show_seconds) return false;
  return !s_settings.seconds_on_shake || burst_active();
}

// Everything on the white LCD panel, drawn straight into the framebuffer.
// Button markers (optional): red bars along the LCD's edges, beside the watch's buttons (one on the
// left, three on the right: opposite it, one lower in the middle, and one mirrored below that).
// Each is 5 px wide and a little taller than the weekday, with its inner corners rounded; the upper
// pair sits a little above the weekday's centre. The lowest one just reaches into the bottom bezel.
#define MARKER_W 5
#define MARKER_H 32         // a little over the weekday's 29 rows
#define MARKER_UPPER_CY 44  // centre of the upper markers (the weekday's centre is 48.5)
#define MARKER_MID_CY 119   // centre of the middle marker; the lowest one mirrors the upper one

// One marker with its left edge at x. `inner_right`: its inside edge is the right one.
static void draw_marker(int x, int cy, bool inner_right) {
  const int top = cy - MARKER_H / 2;
  for (int r = 0; r < MARKER_H; r++) {
    // Rounded inside corners: the first and last rows lose 2 px on the inside, the next ones 1.
    const int cut = (r == 0 || r == MARKER_H - 1) ? 2 : (r == 1 || r == MARKER_H - 2) ? 1 : 0;
    const int w = MARKER_W - cut;
    fill(inner_right ? x : x + MARKER_W - w, top + r, w, 1, GColorRed, false);
  }
}

static void draw_button_markers(void) {
  draw_marker(LCD_X, MARKER_UPPER_CY, true);
  const int right_x = LCD_X + LCD_W - MARKER_W;
  draw_marker(right_x, MARKER_UPPER_CY, false);
  draw_marker(right_x, MARKER_MID_CY, false);
  draw_marker(right_x, 2 * MARKER_MID_CY - MARKER_UPPER_CY, false);
}

static void draw_lcd(void) {
  if (s_settings.button_markers) draw_button_markers();
  char day[4];
  memcpy(day, DAYS + 3 * s_now.tm_wday, 3);
  day[3] = '\0';
  draw_day(WEEKDAY_X, WEEKDAY_Y, day);
  draw_indicator_frame();
  draw_time();
  fill(LCD_X, ROW3_LINE_Y, LCD_W, 2, s_ink, false);
  fill(ROW3_DIV_X, ROW3_LINE_Y, 2, LCD_Y + LCD_H - ROW3_LINE_Y, s_ink, false);
  draw_date();
  if (seconds_showing()) draw_seconds();
  else draw_temperature();
}

// Footprints: the steps' icon, shown instead of the word STEPS. 15 x 13 dots.
#define FOOT_BW 12  // the bitmap's width
#define FOOT_H 13
#define FOOT_W 16   // the room the icon takes: the bitmap, slanted by 20 degrees (rows shift up to 2 px each way)
static const uint16_t FOOT_BITS[FOOT_H] = { 14, 15, 15, 15, 1806, 3968, 3982, 3982, 1796, 0, 896, 896, 256 };

static bool corner_has_icon(int slot) {
  return s_settings.corner[slot] == CORNER_STEPS;
}

// The letters of the badge a bottom corner shows ("WR", or "HR" for the heart rate), or NULL.
static const char *corner_badge(int slot) {
  if (s_settings.corner[slot] == CORNER_WR) return "WR";
  if (s_settings.corner[slot] == CORNER_HEART_RATE && slot >= SLOT_BOTTOM_LEFT) return "HR";
  return NULL;
}

// ---- Weather corners ---------------------------------------------------------------------
// How a reading is written. Values are as the phone sends them (index.js): temperatures in tenths
// of a degree C, speeds in tenths of a km/h, rain and snowfall in tenths of a mm / cm, pressure in
// tenths of a hPa, visibility in tenths of a km, the UV index and the air pollutants in tenths.
enum { F_TEMP, F_HILO, F_PCT, F_TENTHS, F_ROUND, F_AQI, F_SPEED, F_RAIN, F_SNOWFALL, F_DEPTH, F_DIR,
       F_PRESSURE, F_VISIBILITY, F_COND, F_TIME };
enum { SRC_WX, SRC_AIR };
typedef struct {
  char name[11];      // what the settings page sends
  char label[7];      // printed before the value
  uint8_t src, idx, fmt;
} WxCorner;  // (no pointers: the table then stays in flash instead of being copied to RAM)
// The first five are the earliest weather corners: their numbers are saved in settings.
static const WxCorner WX_CORNERS[] = {
  { "uv",        "UV",     SRC_WX,  WX_UV,       F_TENTHS },
  { "wind",      "",       SRC_WX,  WX_WIND,     F_SPEED },
  { "rain",      "",       SRC_WX,  WX_RAIN,     F_RAIN },
  { "humidity",  "HUM",    SRC_WX,  WX_HUMIDITY, F_PCT },
  { "aqi",       "AQI",    SRC_AIR, AQ_US,       F_AQI },
  { "feels",     "FEELS",  SRC_WX,  WX_FEELS,    F_TEMP },
  { "hilo",      "",       SRC_WX,  WX_HIGH,     F_HILO },
  { "chance",    "RAIN",   SRC_WX,  WX_CHANCE,   F_PCT },
  { "sunrise",   "SR",     SRC_WX,  WX_SUNRISE,  F_TIME },
  { "sunset",    "SS",     SRC_WX,  WX_SUNSET,   F_TIME },
  { "winddir",   "",       SRC_WX,  WX_DIR,      F_DIR },
  { "gust",      "G",      SRC_WX,  WX_GUST,     F_SPEED },
  { "pressure",  "",       SRC_WX,  WX_PRESSURE, F_PRESSURE },
  { "cloud",     "CLOUD",  SRC_WX,  WX_CLOUD,    F_PCT },
  { "visibility", "VIS",   SRC_WX,  WX_VIS,      F_VISIBILITY },
  { "dew",       "DEW",    SRC_WX,  WX_DEW,      F_TEMP },
  { "cond",      "",       SRC_WX,  WX_COND,     F_COND },
  { "snowfall",  "SNOW",   SRC_WX,  WX_SNOWFALL, F_SNOWFALL },
  { "snowdepth", "DEPTH",  SRC_WX,  WX_SNOWDEPTH, F_DEPTH },
  { "uvmax",     "UV MAX", SRC_WX,  WX_UVMAX,    F_ROUND },
  { "pm25",      "PM2.5",  SRC_AIR, AQ_PM25,     F_ROUND },
  { "pm10",      "PM10",   SRC_AIR, AQ_PM10,     F_ROUND },
  { "ozone",     "O3",     SRC_AIR, AQ_O3,       F_ROUND },
  { "no2",       "NO2",    SRC_AIR, AQ_NO2,      F_ROUND },
  { "so2",       "SO2",    SRC_AIR, AQ_SO2,      F_ROUND },
  { "co",        "CO",     SRC_AIR, AQ_CO,       F_ROUND },
  { "dust",      "DUST",   SRC_AIR, AQ_DUST,     F_ROUND },
};
#define WX_CORNER_COUNT ((int)ARRAY_LENGTH(WX_CORNERS))
#define CORNER_TYPES (CORNER_WX + WX_CORNER_COUNT)

static const WxCorner *wx_corner(int slot) {
  const int type = s_settings.corner[slot];
  return type >= CORNER_WX && type < CORNER_TYPES ? &WX_CORNERS[type - CORNER_WX] : NULL;
}

// Whether any corner shows a reading of the weather (SRC_WX) or of the air quality (SRC_AIR).
static bool wx_source_used(int src) {
  for (int i = 0; i < SLOTS; i++) {
    const WxCorner *c = wx_corner(i);
    if (c && c->src == src) return true;
  }
  return false;
}

// The reading a corner shows, WX_NONE if it is missing or too old.
static int wx_value(const WxCorner *c) {
  const bool air = c->src == SRC_AIR;
  if (!(air ? reading_valid(s_weather.aqi_updated) : weather_valid())) return WX_NONE;
  const int idx = (c->fmt == F_AQI && s_settings.aqi_eu) ? AQ_EU : c->idx;
  return air ? s_weather.aq[idx] : s_weather.wx[idx];
}

static void weather_string(const WxCorner *c, char *out, size_t size) {
  static const char DIRS[16][4] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW",
                                    "WSW", "W", "WNW", "NW", "NNW" };
  static const char CONDS[][8] = { "CLEAR", "PARTLY", "CLOUDY", "FOG", "DRIZZLE", "RAIN", "SNOW",
                                   "SHOWERS", "STORM" };
  const int v = wx_value(c);
  const bool imperial = use_imperial_weather();
  char head[8];
  snprintf(head, sizeof(head), "%s%s", c->label, c->label[0] ? " " : "");
  if (v == WX_NONE) {
    snprintf(out, size, "%s--", head);
    return;
  }
  switch (c->fmt) {
    case F_TEMP:
      snprintf(out, size, "%s%d", head, temp_in_unit(v));
      break;
    case F_HILO: {
      const int low = s_weather.wx[c->idx + 1];
      if (low == WX_NONE) snprintf(out, size, "H %d L --", temp_in_unit(v));
      else snprintf(out, size, "H %d L %d", temp_in_unit(v), temp_in_unit(low));
      break;
    }
    case F_PCT:
      snprintf(out, size, "%s%d%%", head, v);
      break;
    case F_TENTHS:
      snprintf(out, size, "%s%d.%d", head, v / 10, v % 10);
      break;
    case F_ROUND:
      snprintf(out, size, "%s%d", head, (v + 5) / 10);
      break;
    case F_AQI:
      snprintf(out, size, "%s%d", head, v);
      break;
    case F_SPEED:
      if (imperial) snprintf(out, size, "%s%d MPH", head, (v * 621 + 5000) / 10000);
      else snprintf(out, size, "%s%d KM/H", head, (v + 5) / 10);
      break;
    case F_RAIN:
    case F_SNOWFALL:
      if (imperial) {  // hundredths of an inch (rain in mm, snow in cm: the same step of 25.4)
        const int h = c->fmt == F_RAIN ? (v * 100 + 127) / 254 : (v * 1000 + 127) / 254;
        snprintf(out, size, "%s%d.%02d IN", head, h / 100, h % 100);
      } else {
        snprintf(out, size, "%s%d.%d %s", head, v / 10, v % 10, c->fmt == F_RAIN ? "MM" : "CM");
      }
      break;
    case F_DEPTH:
      if (imperial) snprintf(out, size, "%s%d IN", head, (v * 100 + 127) / 254);
      else snprintf(out, size, "%s%d CM", head, v);
      break;
    case F_DIR:
      snprintf(out, size, "%s%s", head, DIRS[((2 * v + 22) / 45) & 15]);
      break;
    case F_PRESSURE:
      if (imperial) {
        const int h = (v * 2953 + 5000) / 10000;  // hundredths of an inch of mercury
        snprintf(out, size, "%s%d.%02d IN", head, h / 100, h % 100);
      } else {
        snprintf(out, size, "%s%d HPA", head, (v + 5) / 10);
      }
      break;
    case F_VISIBILITY:
      if (imperial) snprintf(out, size, "%s%d MI", head, (v * 621 + 5000) / 10000);
      else snprintf(out, size, "%s%d KM", head, (v + 5) / 10);
      break;
    case F_COND:
      snprintf(out, size, "%s", v >= 0 && v < (int)ARRAY_LENGTH(CONDS) ? CONDS[v] : "--");
      break;
    case F_TIME:
      if (is_24h()) snprintf(out, size, "%s%02d:%02d", head, v / 60, v % 60);
      else snprintf(out, size, "%s%d:%02d%c", head, (v / 60 + 11) % 12 + 1, v % 60, v >= 720 ? 'P' : 'A');
      break;
  }
}

// The text of a bezel corner: empty for nothing and for the WR badge (which is drawn); beside a
// badge only the value is given (the heart rate's "HR" is the badge).
static void corner_string(int slot, char *out, size_t size) {
  out[0] = '\0';
  switch (s_settings.corner[slot]) {
    case CORNER_BATTERY:
      snprintf(out, size, "BATT %d%%", s_battery.charge_percent);
      break;
    case CORNER_STEPS:
    {
      const char *unit = corner_has_icon(slot) ? "" : " STEPS";  // the icon says it in the bottom bezel
      if (s_steps >= 1000) snprintf(out, size, "%d,%03d%s", s_steps / 1000, s_steps % 1000, unit);
      else if (s_steps >= 0) snprintf(out, size, "%d%s", s_steps, unit);
      else snprintf(out, size, "--%s", unit);
      break;
    }
    case CORNER_DISTANCE: {
      const bool miles = use_miles();
      if (s_distance_m < 0) {
        snprintf(out, size, miles ? "-- MI" : "-- KM");
      } else {
        const int tenths = miles ? (s_distance_m * 10 + 804) / 1609 : (s_distance_m + 50) / 100;
        snprintf(out, size, "%d.%d %s", tenths / 10, tenths % 10, miles ? "MI" : "KM");
      }
      break;
    }
    case CORNER_HEART_RATE: {
      const char *label = corner_badge(slot) ? "" : "HR ";
      if (s_hr > 0) snprintf(out, size, "%s%d", label, s_hr);
      else snprintf(out, size, "%s--", label);
      break;
    }
    case CORNER_TEXT:
      snprintf(out, size, "%s", s_settings.corner_text[slot]);
      break;
    default:
      if (s_settings.corner[slot] >= CORNER_WX) weather_string(wx_corner(slot), out, size);
      break;
  }
}

// ---- Bezel text -------------------------------------------------------------------------
// The bezels use Gothic 24 bold (capitals 14 px tall), shrunk to BEZEL_ROWS rows if that is less
// (the system fonts have nothing between 18 and 24). Each text is drawn once in a scratch area (the
// top strip, which is still empty then), its ink is read back from the frame buffer and re-sampled
// one letter at a time, like draw_fitted_text does for the indicator labels: rows are dropped from
// inside each letter (not from its bars) and so are columns (not from its stems), so strokes keep
// their thickness. The result is kept as a bitmap and only drawn again when the text changes.
#define BZ_CAP 14           // capital height of Gothic 24 bold
#define BZ_SCRATCH_H 22     // rows of the top strip that are free (the last two are the case's edge)
#define BZ_BOX_Y (-8)       // the text box's top, so that the capitals start on row 2
#define BZ_W 176            // widest text
#define BZ_STRIDE ((BZ_W + 7) / 8)
#define BADGE_STRIDE 4      // a badge's two letters are at most 32 px wide
#define BZ_KEY 24           // longest text (with its end)
_Static_assert(BEZEL_ROWS <= BZ_CAP, "the bezel text can only be shrunk");
_Static_assert(BZ_BOX_Y + 10 + BZ_H <= BZ_SCRATCH_H, "the scratch text must fit the free rows");

typedef struct {
  uint8_t *bits;     // BZ_H rows of `stride` bytes, 1 = ink, the first pixel in the lowest bit
  uint8_t stride, w, rows;
  uint32_t key;      // a hash of the text it was made from
} BezelText;

static uint8_t s_bezel_bits[SLOTS][BZ_H * BZ_STRIDE], s_badge_bits[2][BZ_H * BADGE_STRIDE];
static BezelText s_bezel[SLOTS], s_badge[2];  // one per corner, and the two badge texts (WR, HR)

static void bezel_text_init(void) {
  for (int i = 0; i < SLOTS; i++) s_bezel[i] = (BezelText){ .bits = s_bezel_bits[i], .stride = BZ_STRIDE };
  for (int i = 0; i < 2; i++) s_badge[i] = (BezelText){ .bits = s_badge_bits[i], .stride = BADGE_STRIDE };
}

static uint32_t text_hash(const char *text) {  // FNV-1a
  uint32_t h = 2166136261u;
  for (; *text; text++) h = (h ^ (uint8_t)*text) * 16777619u;
  return h;
}

// Which column of a letter (cols[] hold the indices of its n columns, rows 0..BZ_CAP-1 count) to
// drop when it is too wide: never the first or last, never a stem that is only two columns wide;
// otherwise the column most like a neighbour, then the one nearest the middle.
static int col_to_drop(const int *cols, int n) {
  int best = n / 2, best_cost = 1 << 30;
  for (int i = 1; i < n - 1; i++) {
    int up = 0, down = 0, vrun = 0, vbest = 0;
    for (int y = 0; y < BZ_CAP; y++) {
      up += s_scratch.letter[y][cols[i]] != s_scratch.letter[y][cols[i - 1]];
      down += s_scratch.letter[y][cols[i]] != s_scratch.letter[y][cols[i + 1]];
      vrun = s_scratch.letter[y][cols[i]] ? vrun + 1 : 0;
      if (vrun > vbest) vbest = vrun;
    }
    int same = 1;  // how many columns in a row look like this one
    for (int dir = -1; dir <= 1; dir += 2) {
      for (int j = i + dir; j >= 0 && j < n; j += dir) {
        bool eq = true;
        for (int y = 0; y < BZ_CAP && eq; y++) eq = s_scratch.letter[y][cols[j]] == s_scratch.letter[y][cols[i]];
        if (!eq) break;
        same++;
      }
    }
    const int stem = (vbest >= 4 && same <= 2) ? 1000 : 0;
    const int like = up < down ? up : down;
    const int cost = stem + like * 10 + abs(2 * i - (n - 1));
    if (cost < best_cost) { best_cost = cost; best = i; }
  }
  return best;
}

// Draws `text` in the scratch area, shrinks it into `bt`, and clears the scratch area again.
static void bezel_text_render(GContext *ctx, const char *text, BezelText *bt) {
  bt->w = bt->rows = 0;
  bt->key = text_hash(text);
  memset(bt->bits, 0, BZ_H * bt->stride);
  if (!text[0]) return;
  graphics_context_set_text_color(ctx, s_frame_text);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, BZ_BOX_Y, BZ_W, 30), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  // The ink's rows, and which columns have any.
  uint8_t col[BZ_W] = { 0 };
  int top = BZ_SCRATCH_H, bottom = -1;
  for (int y = 0; y < BZ_SCRATCH_H; y++) {
    GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
    for (int x = 0; x < BZ_W; x++) {
      if (row.data[x] != s_frame_text.argb) continue;
      col[x] = 1;
      if (y < top) top = y;
      if (y > bottom) bottom = y;
    }
  }
  if (bottom >= 0) {
    const int desc = bottom - top + 1 > BZ_CAP ? bottom - top + 1 - BZ_CAP : 0;  // rows below the capitals
    const int out_rows = BEZEL_ROWS + (desc > BZ_H - BEZEL_ROWS ? BZ_H - BEZEL_ROWS : desc);
    int kept = 0, prev_end = -1;
    for (int x = 0; x < BZ_W;) {
      if (!col[x]) { x++; continue; }
      const int start = x;
      while (x < BZ_W && col[x]) x++;
      const int len = x - start;
      if (len > BZ_MAX_LETTER) break;
      for (int y = 0; y < out_rows - BEZEL_ROWS + BZ_CAP; y++) {  // the letter, row by row
        GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, top + y);
        for (int i = 0; i < len; i++) s_scratch.letter[y][i] = row.data[start + i] == s_frame_text.argb;
      }

      // Vertical: the capital rows are dropped from BZ_CAP down to BEZEL_ROWS, the rows below kept.
      int rows[BZ_H], n = BZ_CAP;
      for (int i = 0; i < BZ_H; i++) rows[i] = i;
      while (n > BEZEL_ROWS) {
        const int drop = row_to_drop(&s_scratch.letter[0][0], BZ_MAX_LETTER, rows, n, 0, len - 1);
        for (int i = drop; i < BZ_H - 1; i++) rows[i] = rows[i + 1];
        n--;
      }
      // Horizontal: the letter's width is scaled like its height.
      int cols[BZ_MAX_LETTER], m = len;
      for (int i = 0; i < len; i++) cols[i] = i;
      const int want = (len * BEZEL_ROWS + BZ_CAP / 2) / BZ_CAP;
      while (m > want && m > 2) {
        const int drop = col_to_drop(cols, m);
        for (int i = drop; i < m - 1; i++) cols[i] = cols[i + 1];
        m--;
      }
      int gap = 0;
      if (prev_end >= 0) {
        gap = ((start - prev_end - 1) * BEZEL_ROWS + BZ_CAP / 2) / BZ_CAP;
        if (gap < 1) gap = 1;
      }
      if (kept + gap + m > bt->stride * 8) break;
      for (int y = 0; y < out_rows; y++) {
        for (int i = 0; i < m; i++) {
          if (s_scratch.letter[rows[y]][cols[i]]) bt->bits[y * bt->stride + ((kept + gap + i) >> 3)] |= 1 << ((kept + gap + i) & 7);
        }
      }
      kept += gap + m;
      prev_end = start + len - 1;
    }
    bt->w = kept;
    bt->rows = out_rows;
  }
  graphics_release_frame_buffer(ctx, fb);
  graphics_context_set_fill_color(ctx, s_frame);
  graphics_fill_rect(ctx, GRect(0, 0, BZ_W, BZ_SCRATCH_H), 0, GCornerNone);
}

// The shrunk text, made again only when it is not the one kept in `bt`.
static const BezelText *bezel_text(GContext *ctx, BezelText *bt, const char *text) {
  if (bt->key != text_hash(text)) bezel_text_render(ctx, text, bt);
  return bt;
}

// What a bezel row has to put on the frame buffer once its texts are made: shrunk texts and
// footprints icons (the badges' outlines are drawn with the drawing API before that).
typedef struct {
  const BezelText *text;
  int16_t x, y, max_w;
} BezelBlit;
typedef struct {
  BezelBlit blit[8];
  int16_t foot[4][2];  // top-left corners of the footprints icons
  uint8_t blits, feet;
} BezelOps;

static void ops_text(BezelOps *ops, const BezelText *text, int x, int y, int max_w) {
  if (text->w > 0 && max_w > 0 && ops->blits < ARRAY_LENGTH(ops->blit)) {
    ops->blit[ops->blits++] = (BezelBlit){ text, x, y, max_w };
  }
}

static void ops_footprints(BezelOps *ops, int x, int y) {
  if (ops->feet < ARRAY_LENGTH(ops->foot)) {
    ops->foot[ops->feet][0] = x;
    ops->foot[ops->feet][1] = y;
    ops->feet++;
  }
}

// A badge: a small outlined box with two letters in it ("WR" water resistant, or "HR").
static void ops_badge(GContext *ctx, BezelOps *ops, int x, int cap_y, const BezelText *letters) {
  graphics_context_set_stroke_color(ctx, s_frame_text);
  graphics_draw_round_rect(ctx, GRect(x, cap_y - 2, WR_BADGE_W, BEZEL_ROWS + 4), 3);
  ops_text(ops, letters, x + (WR_BADGE_W - letters->w + 1) / 2, cap_y, WR_BADGE_W);
}

// Lays out one bezel row: the right corner gets the width it needs (up to right_max), the left
// one the rest. A corner is a text, a badge (with its value beside it), or a steps count that
// ends with the footprints icon. `ctx` is only used to draw the badges' outlines.
static void layout_bezel_row(GContext *ctx, BezelOps *ops, int left_slot, int right_slot, int cap_y,
                             int right_max, bool badges) {
  const int slot[2] = { left_slot, right_slot };
  char str[2][BZ_KEY];
  const BezelText *text[2], *letters[2];
  const char *badge[2];
  for (int i = 0; i < 2; i++) {  // (all text is made before any of it is drawn: see above)
    corner_string(slot[i], str[i], BZ_KEY);
    text[i] = bezel_text(ctx, &s_bezel[slot[i]], str[i]);
    badge[i] = badges ? corner_badge(slot[i]) : NULL;  // (the top strip is the scratch area: no outlines)
    letters[i] = badge[i] ? bezel_text(ctx, &s_badge[badge[i][0] == 'H'], badge[i]) : NULL;
  }
  const int foot_y = cap_y + (BEZEL_ROWS - FOOT_H) / 2;
  const int right_icon_w = corner_has_icon(right_slot) ? FOOT_W + 2 : 0;
  int right_w = badge[1] ? WR_BADGE_W + (str[1][0] ? BADGE_GAP + text[1]->w : 0)
                         : text[1]->w + right_icon_w;
  if (right_w > right_max) right_w = right_max;
  const int right_x = 190 - right_w;
  if (badge[1]) {
    ops_badge(ctx, ops, right_x, cap_y, letters[1]);
    ops_text(ops, text[1], right_x + WR_BADGE_W + BADGE_GAP, cap_y, right_w - WR_BADGE_W - BADGE_GAP);
  } else {
    ops_text(ops, text[1], right_x, cap_y, right_w - right_icon_w);
    if (right_icon_w) ops_footprints(ops, 190 - FOOT_W, foot_y);
  }
  const int left_room = right_x - 10 - 8;  // from x = 10 to the right corner
  if (badge[0]) {
    ops_badge(ctx, ops, 10, cap_y, letters[0]);
    ops_text(ops, text[0], 10 + WR_BADGE_W + BADGE_GAP, cap_y, left_room - WR_BADGE_W - BADGE_GAP);
  } else {
    const int w = text[0]->w < left_room ? text[0]->w : left_room;
    ops_text(ops, text[0], 10, cap_y, w);
    if (corner_has_icon(left_slot) && left_room - w >= FOOT_W + 2) ops_footprints(ops, 10 + w + 2, foot_y);
  }
}

static void draw_bezels(GContext *ctx) {
  BezelOps ops = { .blits = 0, .feet = 0 };
  layout_bezel_row(ctx, &ops, SLOT_TOP_LEFT, SLOT_TOP_RIGHT, TOP_CAP_Y, TOP_RIGHT_MAX, false);
  layout_bezel_row(ctx, &ops, SLOT_BOTTOM_LEFT, SLOT_BOTTOM_RIGHT, BOTTOM_CAP_Y, BOTTOM_RIGHT_MAX, true);

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  for (int i = 0; i < ops.blits; i++) {
    const BezelBlit *b = &ops.blit[i];
    const int w = b->text->w < b->max_w ? b->text->w : b->max_w;
    for (int r = 0; r < b->text->rows; r++) {
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, b->y + r);
      for (int c = 0; c < w; c++) {
        if ((b->text->bits[r * b->text->stride + (c >> 3)] >> (c & 7)) & 1) row.data[b->x + c] = s_frame_text.argb;
      }
    }
  }
  for (int i = 0; i < ops.feet; i++) {  // the footprints, slanted by 20 degrees (tan = 0.364): the
    for (int r = 0; r < FOOT_H; r++) {  // top row leans right, the bottom row left
      const int dx = ((FOOT_H / 2 - r) * 364 + (r < FOOT_H / 2 ? 500 : -500)) / 1000;
      GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, ops.foot[i][1] + r);
      for (int c = 0; c < FOOT_BW; c++) {
        if ((FOOT_BITS[r] >> c) & 1) row.data[ops.foot[i][0] + 2 + c + dx] = s_frame_text.argb;
      }
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}

// SIG lights up while a sound (not just a vibration) is set as the hourly chime and the
// speaker is not muted. Quiet Time mutes the speaker, so the chime is off for its duration;
// the setting itself is untouched, so SIG comes back when Quiet Time ends.
static bool chime_is_audible(void) {
  const uint8_t c = s_settings.chime;
  const bool sound =
      c == CHIME_LCD_CLASSIC || c == CHIME_DOORBELL || c == CHIME_BIG_BEN || c == CHIME_SUPER;
  // The speaker is also muted for good in the watch's sound settings, not only in Quiet Time.
  return sound && !s_quiet && !s_muted;
}

// The indicator box's labels: lit (ink) when active, faint otherwise. Each is
// fitted to LABEL_H rows and centred vertically in its cell (the cells are the
// rows between the 2px frame and the middle divider); "BT" is also widened. The
// CHG and SIG cells stop 1px short of the rounded corners that intrude.
static void draw_indicator_labels(GContext *ctx) {
  const int top_h = BOX_MID - BOX_TOP - 2, bottom_h = BOX_BOTTOM - BOX_MID - 2;
  const int left_w = BOX_DIV - BOX_LEFT, right_w = BOX_RIGHT - BOX_DIV;
  static const int8_t BT_WIDTHS[] = { 8, 9 };  // widened B and T
  // On the charger but no longer charging: the battery is full.
  const bool full = s_battery.is_plugged && !s_battery.is_charging;
  struct { const char *label; bool on; GRect cell; const int8_t *letter_w; int letter_w_n; } ind[] = {
    { "BT",   s_connected,
      GRect(BOX_LEFT + 4, BOX_TOP + 2, left_w - 4, top_h), BT_WIDTHS, ARRAY_LENGTH(BT_WIDTHS) },
    { full ? "FULL" : "CHG", s_battery.is_charging || full,
      GRect(BOX_DIV + 1, BOX_TOP + 2, right_w - 4, top_h), NULL, 0 },
    { "SIG",  chime_is_audible(),
      GRect(BOX_LEFT + 4, BOX_MID + 1, left_w - 4, bottom_h), NULL, 0 },
    { "MUTE", s_quiet,
      GRect(BOX_DIV + 1, BOX_MID + 1, right_w - 2, bottom_h), NULL, 0 },
  };
  for (unsigned i = 0; i < ARRAY_LENGTH(ind); i++) {
    if (!ind[i].on && !s_settings.ghosts) continue;
    draw_fitted_text(ctx, ind[i].label, ind[i].cell, ind[i].letter_w, ind[i].letter_w_n, LABEL_H,
                     ind[i].on ? s_ink : s_ghost, ind[i].on ? DENSITY_FULL : s_label_off_density);
  }
}

// DST: top left of the date cell, lit while daylight saving time is in effect in the watch's
// time zone (the phone provides the zone; the watch's own clock knows when DST applies).
static void draw_dst_label(GContext *ctx) {
  const bool on = s_now.tm_isdst > 0;
  if (!on && !s_settings.ghosts) return;
  draw_fitted_text(ctx, "DST", GRect(DST_X, DST_Y, DST_W, DST_H), NULL, 0, LABEL_H,
                   on ? s_ink : s_ghost, on ? DENSITY_FULL : s_label_off_density);
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

  s_quiet = quiet_time_is_active();
  s_muted = speaker_is_muted();
  s_fb = graphics_capture_frame_buffer(ctx);
  if (!s_fb) return;
  draw_lcd();
  graphics_release_frame_buffer(ctx, s_fb);
  s_fb = NULL;

  // Text goes through the regular text API, so it comes after the framebuffer
  // is released.
  draw_bezels(ctx);
  draw_indicator_labels(ctx);
  draw_dst_label(ctx);
}

// ---------------------------------------------------------------------------
// Services

static void request_weather(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_RequestWeather, s_settings.weather_refresh);  // the phone uses it to throttle
  dict_write_uint8(iter, MESSAGE_KEY_RequestAQI, wx_source_used(SRC_AIR) ? 1 : 0);
  app_message_outbox_send();
}

// Tells the phone which 12/24-hour style and measurement system the watch follows, so that the
// settings page's preview can show what "Follow watch" means. Bit 0: 24-hour, bit 1: imperial.
static void send_watch_prefs(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_WatchPrefs,
                   (clock_is_24h_style() ? 1 : 0) | (watch_is_imperial() ? 2 : 0));
  app_message_outbox_send();
}

// Asks the phone for weather only when it would be shown and used: the temperature is on
// screen (not the seconds), the phone is connected, and the last reading is not recent.
// Each request wakes the phone app for a location fix and a web request.
static void refresh_weather_if_needed(void) {
  // The temperature is on screen unless the seconds are always shown.
  const bool temp_shown = !(s_settings.show_seconds && !s_settings.seconds_on_shake);
  if (!(temp_shown || wx_source_used(SRC_WX) || wx_source_used(SRC_AIR)) || !s_connected) return;
  // A reading a corner needs that the last answer lacked (the corner was just switched on) is asked for at once.
  bool missing = false;
  for (int i = 0; i < SLOTS; i++) {
    const WxCorner *c = wx_corner(i);
    if (!c) continue;
    const bool air = c->src == SRC_AIR;
    if ((air ? s_weather.aqi_updated : s_weather.updated) == 0 ||
        (air ? s_weather.aq[c->fmt == F_AQI && s_settings.aqi_eu ? AQ_EU : c->idx] : s_weather.wx[c->idx]) == WX_NONE) missing = true;
  }
  if (!missing && s_weather.updated != 0 && time(NULL) - s_weather.updated < (weather_refresh_min() - 5) * 60) return;
  request_weather();
}

static void tick_handler(struct tm *now, TimeUnits changed);
static void play_chime(bool preview);

// Once a minute normally (plus the hour, so a whole-hour time zone change redraws at once);
// every second only while the seconds are shown AND the face is in front.
static void subscribe_ticks(void) {
  tick_timer_service_unsubscribe();
  const bool per_second = seconds_showing() && s_focus;
  tick_timer_service_subscribe(per_second ? SECOND_UNIT : (MINUTE_UNIT | HOUR_UNIT), tick_handler);
}

// A wrist shake: start (or extend) the burst of ticking seconds.
static void start_seconds_burst(void) {
  if (!s_focus || !s_settings.show_seconds || !s_settings.seconds_on_shake) return;
  s_burst_until = time(NULL) + s_settings.seconds_burst_s;
  subscribe_ticks();
  time_t now = time(NULL);
  s_now = *localtime(&now);
  layer_mark_dirty(s_canvas);
}

static void shake_handler(AccelAxisType axis, int32_t direction) { start_seconds_burst(); }

// The accelerometer's shake detection (the SDK calls it the "tap" service) is only listened to
// while the setting needs it.
static void update_shake_subscription(void) {
  accel_tap_service_unsubscribe();
  if (s_settings.show_seconds && s_settings.seconds_on_shake) {
    accel_tap_service_subscribe(shake_handler);
  } else {
    s_burst_until = 0;
  }
}

// A notification or menu covering the face makes per-second redraws pointless.
static void focus_handler(bool in_focus) {
  s_focus = in_focus;
  subscribe_ticks();
  if (in_focus) {
    time_t now = time(NULL);
    s_now = *localtime(&now);
    layer_mark_dirty(s_canvas);
  }
}

static void tick_handler(struct tm *now, TimeUnits changed) {
  s_now = *now;
  if (s_burst_until != 0 && !burst_active()) {
    s_burst_until = 0;      // the burst is over: back to a tick a minute and the temperature
    subscribe_ticks();
  }
  if (changed & MINUTE_UNIT) {
    if (corner_used(CORNER_STEPS)) update_steps();
    if (corner_used(CORNER_DISTANCE)) update_distance();
    if ((now->tm_hour * 60 + now->tm_min) % weather_refresh_min() == 0) refresh_weather_if_needed();
  }
  layer_mark_dirty(s_canvas);
  if (now->tm_min == 0 && now->tm_sec == 0) play_chime(false);
}

// The charge level is only on screen while a corner shows it; the charging indicator
// (CHG / FULL) needs a redraw only when the charger state changes.
static void battery_handler(BatteryChargeState state) {
  const bool redraw = state.is_charging != s_battery.is_charging || state.is_plugged != s_battery.is_plugged ||
                      (state.charge_percent != s_battery.charge_percent && corner_used(CORNER_BATTERY));
  s_battery = state;
  if (redraw) layer_mark_dirty(s_canvas);
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

#define LCD_BEEP_HZ 4096
#define LCD_BEEP_MS 120

static void lcd_second_beep(void *context) {
  speaker_play_tone(LCD_BEEP_HZ, LCD_BEEP_MS + 10, s_settings.chime_volume, SpeakerWaveformSquare);
}

// Tunes are built into one buffer that stays valid while the speaker plays it.
#define TUNE_MAX 16  // the longest tune needs 15 notes
static SpeakerNote s_tune[TUNE_MAX];
static uint32_t s_tune_len;

static void tune_add(uint8_t midi, uint16_t ms, SpeakerWaveform wave) {
  if (s_tune_len < TUNE_MAX) {
    s_tune[s_tune_len++] = (SpeakerNote){ .midi_note = midi, .waveform = wave, .duration_ms = ms };
  }
}

// Big Ben: the first bar of the Westminster Quarters' full-hour chime, three 550 ms notes and
// a 1100 ms one in the key of E major (E4 G#4 F#4 B3).
static void build_big_ben(void) {
  static const uint8_t BAR[4] = { 64, 68, 66, 59 };
  s_tune_len = 0;
  for (int n = 0; n < 4; n++) tune_add(BAR[n], n == 3 ? 1100 : 550, SpeakerWaveformSine);
}

// The first bar of a well-known platform-game theme, up to the high G (midi note, length in
// 85 ms steps; 0 = rest).
static void build_super(void) {
  static const uint8_t TUNE[][2] = {
    { 76, 1 }, { 76, 1 }, { 0, 1 }, { 76, 1 }, { 0, 1 }, { 72, 1 }, { 76, 1 }, { 0, 1 },
    { 79, 2 },
  };
  s_tune_len = 0;
  for (size_t i = 0; i < ARRAY_LENGTH(TUNE); i++) {
    uint16_t ms = TUNE[i][1] * 85;
    if (TUNE[i][0] == 0) {
      tune_add(0, ms, SpeakerWaveformSquare);
    } else {
      tune_add(TUNE[i][0], ms - 12, SpeakerWaveformSquare);  // a short gap so repeats stay separate
      tune_add(0, 12, SpeakerWaveformSquare);
    }
  }
}

// The hourly chime. The watch mutes its speaker during Quiet Time and apps can't override
// that, so when the chime is allowed then, it is replaced by a vibration. `preview` plays
// it once when the choice is saved, whatever the time or Quiet Time.
static void play_chime(bool preview) {
  static const SpeakerNote BELL[] = {
    { 76, SpeakerWaveformSine, 500, 0, 0 }, { 72, SpeakerWaveformSine, 900, 0, 0 },
  };
  const uint8_t choice = s_settings.chime;
  if (choice == CHIME_OFF || choice == 3 || choice >= CHIME_COUNT) return;
  const bool quiet = quiet_time_is_active();
  if (quiet && s_settings.chime_quiet && !preview) return;
  if (choice == CHIME_VIBE) {
    vibes_double_pulse();
    return;
  }
  if (speaker_is_muted()) {
    if (quiet && !s_settings.chime_quiet) vibes_double_pulse();  // the speaker is muted for Quiet Time
    return;
  }
  switch (choice) {
    case CHIME_LCD_CLASSIC:
      // Two flat 4096 Hz beeps, 120 ms long with a 120 ms gap, as measured from a recording
      // of a digital watch hourly signal. The notes API only has semitone steps, so the second beep
      // is scheduled with a timer to keep the exact frequency.
      if (speaker_play_tone(LCD_BEEP_HZ, LCD_BEEP_MS, s_settings.chime_volume, SpeakerWaveformSquare)) {
        app_timer_register(2 * LCD_BEEP_MS, lcd_second_beep, NULL);
      }
      break;
    case CHIME_DOORBELL:  speaker_play_notes(BELL, ARRAY_LENGTH(BELL), s_settings.chime_volume); break;
    case CHIME_BIG_BEN:
      build_big_ben();
      speaker_play_notes(s_tune, s_tune_len, s_settings.chime_volume);
      break;
    case CHIME_SUPER:
      build_super();
      speaker_play_notes(s_tune, s_tune_len, s_settings.chime_volume);
      break;
    default: break;
  }
}

static void connection_handler(bool connected) {
  if (connected != s_connected && !quiet_time_is_active()) {
    play_vibe(connected ? s_settings.vibe_connect : s_settings.vibe_disconnect);
  }
  const bool changed = connected != s_connected;
  s_connected = connected;
  if (!changed) return;  // nothing visible changes
  layer_mark_dirty(s_canvas);
  if (connected) refresh_weather_if_needed();
}

#if defined(PBL_HEALTH)
static void health_handler(HealthEventType event, void *context) {
  if (event != HealthEventHeartRateUpdate) return;
  const int before = s_hr;
  update_heart_rate();
  if (s_hr != before) layer_mark_dirty(s_canvas);
}
#endif

// Health events wake the app, so they are only listened to while the heart rate is shown. (The
// steps and distance are read on the minute tick, which redraws the face anyway.)
static void update_health_subscription(void) {
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
  if (corner_used(CORNER_HEART_RATE)) {
    update_heart_rate();
    health_service_events_subscribe(health_handler, NULL);
  }
#endif
}

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
  // The cut is in bytes but the settings page counts characters: if it landed inside a
  // multi-byte UTF-8 character, drop that whole character so the string stays valid.
  size_t len = strlen(dst);
  if (strlen(src) > len) {
    while (len > 0 && ((unsigned char)dst[len - 1] & 0xC0) == 0x80) len--;  // continuation bytes
    if (len > 0 && ((unsigned char)dst[len - 1] & 0xC0) == 0xC0) len--;  // their lead byte
    dst[len] = '\0';
  }
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
  bool play_preview = false;

  if (dict_find(iter, MESSAGE_KEY_RequestPrefs)) send_watch_prefs();
  if ((t = dict_find(iter, MESSAGE_KEY_WeatherData)) && t->length >= WX_COUNT * 2) {
    const time_t now = time(NULL);
    int16_t before[WX_COUNT];
    memcpy(before, s_weather.wx, sizeof(before));
    memcpy(s_weather.wx, t->value->data, sizeof(s_weather.wx));  // little-endian 16-bit numbers, like the watch
    s_weather.updated = now;
    layer_mark_dirty(s_canvas);
    // Flash writes cost energy: save when a reading changed, otherwise about hourly (the saved
    // time only has to be good enough for the "too old" limit).
    if (memcmp(before, s_weather.wx, sizeof(before)) != 0 || now - s_weather_saved >= 60 * 60) {
      persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
      s_weather_saved = now;
    }
  }
  // The air quality comes in a message of its own, a little after the weather.
  if ((t = dict_find(iter, MESSAGE_KEY_AirData)) && t->length >= AQ_COUNT * 2) {
    memcpy(s_weather.aq, t->value->data, sizeof(s_weather.aq));
    s_weather.aqi_updated = time(NULL);
    persist_write_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
    layer_mark_dirty(s_canvas);
  }
  // Time & date
  if ((t = dict_find(iter, MESSAGE_KEY_TimeFormat))) {
    const char *f = t->value->cstring;
    s_settings.time_format = strcmp(f, "12") == 0 ? FORMAT_12H : strcmp(f, "24") == 0 ? FORMAT_24H : FORMAT_AUTO;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DateFormat))) {
    s_settings.day_first = strcmp(t->value->cstring, "DM") == 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TimeZero))) {
    s_settings.hour_no_zero = tuple_int(t) ? 0 : 1;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DatePadding))) {
    const char *p = t->value->cstring;
    s_settings.date_pad = strcmp(p, "first") == 0 ? PAD_FIRST_BLANK : strcmp(p, "both") == 0 ? PAD_BOTH_BLANK : PAD_ZERO;
    settings_changed = true;
  }
  // Right box
  if ((t = dict_find(iter, MESSAGE_KEY_RightBox))) {
    s_settings.show_seconds = strcmp(t->value->cstring, "seconds") == 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SecondsMode))) {
    s_settings.seconds_on_shake = strcmp(t->value->cstring, "shake") == 0 ? 1 : 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_SecondsDuration))) {
    int v = tuple_int(t);
    s_settings.seconds_burst_s = v < SECONDS_BURST_MIN_S ? SECONDS_BURST_MIN_S
        : v > SECONDS_BURST_MAX_S ? SECONDS_BURST_MAX_S : v;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TempUnit))) {
    const char *u = t->value->cstring;
    s_settings.temp_unit = strcmp(u, "C") == 0 ? UNIT_C : strcmp(u, "F") == 0 ? UNIT_F : UNIT_AUTO;
    settings_changed = true;
  }
  // Top and bottom bezel corners
  // (The message keys are variables, so these are filled in at run time.)
  const uint32_t CORNER_KEYS[SLOTS] = {
    MESSAGE_KEY_TopLeft, MESSAGE_KEY_TopRight, MESSAGE_KEY_BottomLeft, MESSAGE_KEY_BottomRight,
  };
  const uint32_t CORNER_TEXT_KEYS[SLOTS] = {
    MESSAGE_KEY_TopLeftText, MESSAGE_KEY_TopRightText, MESSAGE_KEY_BottomLeftText,
    MESSAGE_KEY_BottomRightText,
  };
  for (int i = 0; i < SLOTS; i++) {
    if ((t = dict_find(iter, CORNER_KEYS[i]))) {
      s_settings.corner[i] = CORNER_NOTHING;
      for (int k = 0; k < CORNER_TYPES; k++) {
        const char *name = k < CORNER_WX ? CORNER_NAMES[k] : WX_CORNERS[k - CORNER_WX].name;
        if (strcmp(t->value->cstring, name) == 0) s_settings.corner[i] = k;
      }
      settings_changed = true;
    }
    if ((t = dict_find(iter, CORNER_TEXT_KEYS[i]))) {
      copy_upper(s_settings.corner_text[i], sizeof(s_settings.corner_text[i]), t->value->cstring);
      settings_changed = true;
    }
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WeatherUnits))) {
    const char *u = t->value->cstring;
    s_settings.weather_units = strcmp(u, "metric") == 0 ? WUNITS_METRIC : strcmp(u, "imperial") == 0 ? WUNITS_IMPERIAL : WUNITS_AUTO;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_AQIScale))) {
    s_settings.aqi_eu = strcmp(t->value->cstring, "eu") == 0;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DistanceUnit))) {
    const char *d = t->value->cstring;
    s_settings.dist_unit = strcmp(d, "km") == 0 ? DIST_KM : strcmp(d, "mi") == 0 ? DIST_MI : DIST_AUTO;
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
  if ((t = dict_find(iter, MESSAGE_KEY_Slanted))) {
    s_settings.slanted = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Ghosts))) {
    s_settings.ghosts = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_WeatherRefresh))) {
    const int minutes = atoi(t->value->cstring);
    s_settings.weather_refresh = (minutes >= 30 && minutes <= 720) ? minutes / 10 : WEATHER_REFRESH_DEFAULT / 10;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ButtonMarkers))) {
    s_settings.button_markers = tuple_int(t) ? 1 : 0;
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
  if ((t = dict_find(iter, MESSAGE_KEY_HourlyChime))) {
    int v = tuple_int(t);
    uint8_t chime = (v >= 0 && v < CHIME_COUNT && v != 3) ? v : CHIME_OFF;
    play_preview = chime != CHIME_OFF && chime != s_settings.chime;
    s_settings.chime = chime;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ChimeQuiet))) {
    s_settings.chime_quiet = tuple_int(t);
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ChimeVolume))) {
    int v = tuple_int(t);
    s_settings.chime_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    settings_changed = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ChimeTest)) && tuple_int(t)) {
    play_preview = true;  // asked for with the settings page's "Play chime" button
    settings_changed = true;
  }

  if (settings_changed) {
    persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
    apply_theme();
    apply_backlight();
    update_shake_subscription();
    update_health_subscription();
    subscribe_ticks();
    if (corner_used(CORNER_STEPS)) update_steps();
    if (corner_used(CORNER_DISTANCE)) update_distance();
    refresh_weather_if_needed();
    if (play_preview) play_chime(true);
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
    .corner = { CORNER_BATTERY, CORNER_STEPS, CORNER_WR, CORNER_TEXT },
    .corner_text = { "30 DAY BATT", "WR 3ATM", "", "PEBBLE" },
    .slanted = true,
    .ghosts = true,
    .backlight = BACKLIGHT_SYSTEM,
    .vibe_disconnect = VIBE_DOUBLE,
    .vibe_connect = VIBE_SHORT,
    .chime_quiet = true,
    .chime_volume = CHIME_VOLUME_DEFAULT,
    .seconds_burst_s = SECONDS_BURST_DEFAULT_S,
    .weather_refresh = WEATHER_REFRESH_DEFAULT / 10,
  };
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  for (int i = 0; i < SLOTS; i++) {
    if (s_settings.corner[i] >= CORNER_TYPES) s_settings.corner[i] = CORNER_NOTHING;
    s_settings.corner_text[i][sizeof(s_settings.corner_text[i]) - 1] = '\0';
  }
  if (s_settings.dist_unit > DIST_MI) s_settings.dist_unit = DIST_AUTO;
  // Old saves left padding where these two now live.
  if (s_settings.seconds_on_shake > 1) s_settings.seconds_on_shake = 0;
  if (s_settings.hour_no_zero > 1) s_settings.hour_no_zero = 0;
  if (s_settings.button_markers > 1) s_settings.button_markers = 0;
  if (s_settings.aqi_eu > 1) s_settings.aqi_eu = 0;
  if (s_settings.weather_units > WUNITS_IMPERIAL) s_settings.weather_units = WUNITS_AUTO;
  if (s_settings.weather_refresh < 3 || s_settings.weather_refresh > 72) s_settings.weather_refresh = WEATHER_REFRESH_DEFAULT / 10;
  if (s_settings.date_pad > PAD_BOTH_BLANK) s_settings.date_pad = PAD_ZERO;
  if (s_settings.seconds_burst_s < SECONDS_BURST_MIN_S || s_settings.seconds_burst_s > SECONDS_BURST_MAX_S) {
    s_settings.seconds_burst_s = SECONDS_BURST_DEFAULT_S;
  }
  for (int i = 0; i < WX_COUNT; i++) s_weather.wx[i] = WX_NONE;
  for (int i = 0; i < AQ_COUNT; i++) s_weather.aq[i] = WX_NONE;
  persist_read_data(WEATHER_KEY, &s_weather, sizeof(s_weather));
  bezel_text_init();
  apply_theme();
  apply_backlight();

  time_t now = time(NULL);
  s_now = *localtime(&now);
  s_battery = battery_state_service_peek();
  s_connected = connection_service_peek_pebble_app_connection();
  if (corner_used(CORNER_STEPS)) update_steps();
  if (corner_used(CORNER_DISTANCE)) update_distance();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);

  update_shake_subscription();
  subscribe_ticks();
  battery_state_service_subscribe(battery_handler);
  app_focus_service_subscribe(focus_handler);
  connection_service_subscribe(
      (ConnectionHandlers){ .pebble_app_connection_handler = connection_handler });
  update_health_subscription();

  app_message_register_inbox_received(inbox_handler);
  // The inbox must hold a full settings Save: about 230 bytes with plain text and
  // over 400 with emoji in the three custom texts.
  // The outbox only ever carries the two one-byte values of the weather request.
  app_message_open(512, dict_calc_buffer_size(2, sizeof(uint8_t), sizeof(uint8_t)));
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  app_focus_service_unsubscribe();
  accel_tap_service_unsubscribe();
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
