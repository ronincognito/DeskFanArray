// ================================================================
// WaveShare Fan Controller — 6-fan PWM controller with touchscreen UI
// 5 swipeable screens: Custom, Stereotone, Monotone, Wave, Breeze, Pulse
// compile for board: Waveshare ESP32-S3-LCD-1.47
// ================================================================

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_lcd_touch_axs5106l.h"
#include <Adafruit_GFX.h>
#include "Fonts/FreeSans9pt7b.h"
#include "startup_image.h"
#include "esp_heap_caps.h"
#include <string.h>

// ================================================================
// HARDWARE PINS
// ================================================================

#define ROTATION 1
#define DISPLAY_SPI_SPEED 40000000 // default (unspecified) is much more conservative; try 80000000
                                    // if wiring is short/clean and the panel stays glitch-free
#define FORCE_WIPE_TRANSITION 0    // diagnostic only: set to 1 to disable the push transition
                                    // entirely (falls back to the old wipe) without removing any
                                    // code, to isolate whether a crash is coming from that path
#define GFX_BL 46
#define LCD_RST 40
#define PWM_FREQ 25000
#define PWM_RESOLUTION 8
#define TOUCH_SDA 42
#define TOUCH_SCL 41
#define TOUCH_RST 47
#define TOUCH_INT 48

#define NUM_FANS 6
const int FAN_PINS[NUM_FANS] = { 4, 5, 6, 7, 8, 9 };

Arduino_DataBus *bus = new Arduino_ESP32SPI(45, 21, 38, 39);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 47, 0, false, 172, 320, 34, 0, 34, 0);

// Screen layout constants needed by initPushTransition() below (moved up
// from the SHARED LAYOUT section, which is further down the file, since
// TRANSITION_HEIGHT is needed before that point).
#define TITLE_Y -2
#define DOTS_Y 168 // position page dots

// Content band animated by the push transition (see pushTransition()) --
// deliberately excludes the title strip and the page-dot row, which stay
// static during the slide instead of moving with the content.
#define TRANSITION_TOP    30                                // just below the title strip
#define TRANSITION_HEIGHT (DOTS_Y - 4 - TRANSITION_TOP)      // stop a few px above the page dots

// ================================================================
// PSRAM-BACKED OFF-SCREEN CANVAS (used for the push-transition animation)
// ================================================================
// Arduino_Canvas's own begin() allocates its framebuffer with
// aligned_alloc() (verified against the library source), which on ESP32
// comes out of internal RAM, not PSRAM. A push transition needs two full
// 320x172 16-bit screen buffers (rendering happens at full-screen
// coordinates, since the existing draw*Screen() functions use absolute
// y positions like BAR_TOP/DOTS_Y) plus one smaller composite buffer --
// more than internal RAM can comfortably spare -- so this subclass
// overrides begin() to force the framebuffer into PSRAM via
// heap_caps_malloc(). Everything else (drawing primitives, text, flush())
// is inherited unchanged from Arduino_Canvas.
class Arduino_CanvasPSRAM : public Arduino_Canvas {
 public:
  Arduino_CanvasPSRAM(int16_t w, int16_t h, Arduino_G *output)
    : Arduino_Canvas(w, h, output) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (!_framebuffer) {
      size_t s = (size_t)_width * _height * 2;
      _framebuffer = (uint16_t *)heap_caps_malloc(s, MALLOC_CAP_SPIRAM);
      if (!_framebuffer) return false;
    }
    return true; // the real gfx/output is already begin()'d elsewhere in setup()
  }
};

// Two off-screen canvases (outgoing / incoming screen) plus one composite
// buffer that gets blitted to the real display once per animation frame.
// Allocated in initPushTransition() (called from setup()); transitionReady
// stays false — and goToScreen() falls back to the plain wipe — if PSRAM
// wasn't available for any reason.
Arduino_CanvasPSRAM *transitionCanvasOld = nullptr;
Arduino_CanvasPSRAM *transitionCanvasNew = nullptr;
uint16_t *transitionComposite = nullptr;
bool transitionReady = false;

void initPushTransition() {
  transitionCanvasOld = new Arduino_CanvasPSRAM(gfx->width(), gfx->height(), gfx);
  transitionCanvasNew = new Arduino_CanvasPSRAM(gfx->width(), gfx->height(), gfx);
  // Only the content band (TRANSITION_HEIGHT rows) is ever composited/blitted per
  // frame -- the title strip and page dots stay static -- so the composite
  // buffer only needs to be that tall, not a full screen height.
  transitionComposite = (uint16_t *)heap_caps_malloc((size_t)gfx->width() * TRANSITION_HEIGHT * 2, MALLOC_CAP_SPIRAM);

  transitionReady = transitionCanvasOld->begin() && transitionCanvasNew->begin() && (transitionComposite != nullptr);
  if (!transitionReady) {
    Serial.println("Push transition: PSRAM allocation failed, falling back to wipe transition.");
  }
}

// ================================================================
// BACKLIGHT (idle dim/off + transition fade)
// ================================================================

#define BACKLIGHT_PWM_FREQ       5000
#define BACKLIGHT_PWM_RESOLUTION 8
#define BACKLIGHT_FULL  255
#define BACKLIGHT_DIM   80
#define BACKLIGHT_OFF   0
#define IDLE_DIM_MS   120000   // dim after 2 min of no touch
#define IDLE_OFF_MS   600000  // fully off after 10 min of no touch
#define STARTUP_BACKLIGHT_DELAY_MS 500 // hold backlight off this long after the splash screen is
                                        // drawn, before fading in (tune to taste)
#define STARTUP_SCREEN_HOLD_MS 2000    // roughly how long the splash stays fully visible before
                                        // wiping into the default screen (measured from when
                                        // drawStartupScreen() runs; doesn't separately account
                                        // for the ~150ms fade-in/out on top of this)

unsigned long lastActivityTime = 0;
int currentBacklight = BACKLIGHT_OFF; // starts OFF — see setup(), which kills the backlight in
                                       // hardware before this variable's initial value even matters

void setBacklight(int level) {
  if (level != currentBacklight) {
    currentBacklight = level;
    ledcWrite(GFX_BL, level);
  }
}

// Fades the backlight linearly from wherever it currently is up to
// BACKLIGHT_FULL, over `steps` increments spaced `stepDelayMs` apart.
// Shared by the startup fade-in (setup()) and every screen transition
// (goToScreen()) so there's one fade implementation, not two.
void fadeBacklightIn(int steps = 10, int stepDelayMs = 15) {
  for (int i = 1; i <= steps; i++) {
    setBacklight((i * BACKLIGHT_FULL) / steps);
    delay(stepDelayMs);
  }
}

// Mirror of fadeBacklightIn(): fades the backlight linearly down to
// BACKLIGHT_OFF instead of up to BACKLIGHT_FULL. Used for the startup
// splash -> default screen transition (dim to black, draw, fade back in)
// rather than a wipe.
void fadeBacklightOut(int steps = 10, int stepDelayMs = 15) {
  for (int i = steps - 1; i >= 0; i--) {
    setBacklight((i * BACKLIGHT_FULL) / steps);
    delay(stepDelayMs);
  }
}

// ================================================================
// COLORS
// ================================================================

#define RGB565(r, g, b) (((r) << 11) | ((g) << 5) | (b))
#define GREY565(level5bit) RGB565((level5bit), (level5bit) * 2, (level5bit))

#define COLOR_BAR_BG              RGB565(6, 0, 0)
#define COLOR_BAR_BG_ACTIVE       RGB565(10, 0, 0)   // lighter/warmer red-track when being dragged
#define COLOR_BAR_FILL            RGB565(31, 0, 0)   // full-bright red, in this file's own 5/6/5-bit
                                                      // RGB565() scale -- NOT the library's RGB565_RED,
                                                      // which assumes 8-bit inputs and silently
                                                      // overflowed uint16_t when this file's RGB565()
                                                      // macro (redefined above, 5/6/5-bit) re-expanded
                                                      // it; harmless via implicit truncation in a plain
                                                      // assignment, but a hard narrowing error once used
                                                      // inside a brace-initializer (see Control struct)

#define COLOR_DOT_ACTIVE          GREY565(16)
#define COLOR_DOT_INACTIVE        GREY565(4)
#define COLOR_CONTROL_BG          COLOR_BAR_BG
#define COLOR_CONTROL_DOT         COLOR_BAR_FILL
#define COLOR_CONTROL_CROSSHAIR   RGB565(6, 0, 0) // faint crosshair through the pad's thumb
#define CROSSHAIR_WIDTH           2

#define COLOR_WAVE_BAR            GREY565(4)

#define COLOR_BLUE_BAR_BG         RGB565(3, 3, 6)
#define COLOR_BLUE_BAR_BG_ACTIVE  RGB565(3, 6, 8)
#define COLOR_BLUE_BAR_FILL       RGB565(8, 14, 31)

#define COLOR_YELLOW_BAR_BG   RGB565(6, 6, 1)   // dark yellow track
#define COLOR_YELLOW_BAR_BG_ACTIVE RGB565(10, 10, 0)
#define COLOR_YELLOW_BAR_FILL RGB565(24, 24, 3)   // bright yellow thumb

#define COLOR_CONTROL_BG_ACTIVE   COLOR_BAR_BG_ACTIVE // Ondulation's pad, same treatment as (red) bar

// ================================================================
// SCREENS
// ================================================================

#define SCREEN_NATURAL_BREEZE 0
#define SCREEN_ONDULATION     1
#define SCREEN_MONOTONE       2
#define SCREEN_STEREOTONE     3
#define SCREEN_CUSTOM         4
#define SCREEN_PULSE          5
#define NUM_SCREENS           6

const char *SCREEN_TITLES[NUM_SCREENS] = {
  "breeze", "wave", "monotone", "stereotone", "custom", "pulse"
};

int currentScreen = SCREEN_CUSTOM; // placeholder — setup() overwrites this with the persisted default

// ================================================================
// PERSISTED DEFAULT SCREEN — which screen to boot into, saved across
// power cycles via a long press (see onLongPress() below) and read back
// in setup(). Uses the ESP-IDF NVS API directly (nvs_flash.h/nvs.h)
// rather than the Arduino `Preferences` wrapper: same underlying flash
// storage, but Preferences.h failed to resolve on at least one setup
// despite a core new enough to have it (this sketch already relies on
// the core-3.x single-pin ledcAttach() API elsewhere) — likely a stale
// library-index/cache issue rather than a missing core. nvs_flash.h and
// nvs.h are core ESP-IDF headers, resolved through a different, more
// fundamental include path than Arduino's per-sketch library search, so
// they sidestep whatever was going wrong with the Preferences library
// specifically.
// ================================================================

#define NVS_NAMESPACE "fanctrl"
#define NVS_KEY_DEFAULT_SCREEN "defScreen"

bool nvsReady = false; // set in initNvs(); load/save below silently no-op if NVS never came up

// Initializes the NVS flash partition. Must be called once, early in
// setup(), before loadDefaultScreen()/saveDefaultScreen() are used.
// Handles the two known first-boot conditions (no free pages / a newer
// NVS layout than what's currently written) by erasing and retrying —
// standard ESP-IDF boilerplate for this call.
void initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  nvsReady = (err == ESP_OK);
  if (!nvsReady) Serial.println("NVS init failed; default-screen persistence disabled for this session");
}

// Reads the persisted default screen, falling back to Natural Breeze if
// NVS isn't available, nothing has been saved yet, or the stored value is
// out of range (e.g. a saved index from a build with more screens than
// this one has).
int loadDefaultScreen() {
  if (!nvsReady) return SCREEN_NATURAL_BREEZE;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return SCREEN_NATURAL_BREEZE;

  uint8_t saved = SCREEN_NATURAL_BREEZE;
  nvs_get_u8(h, NVS_KEY_DEFAULT_SCREEN, &saved); // leaves `saved` at the fallback if the key doesn't exist yet
  nvs_close(h);

  return (saved < NUM_SCREENS) ? saved : SCREEN_NATURAL_BREEZE;
}

// Persists `screen` as the screen to boot into next time. Called from
// onLongPress() with the currently shown screen.
void saveDefaultScreen(int screen) {
  if (!nvsReady) return;

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

  nvs_set_u8(h, NVS_KEY_DEFAULT_SCREEN, (uint8_t)screen);
  nvs_commit(h);
  nvs_close(h);
}

// ================================================================
// STARTUP DEFAULTS — edit these to change what each screen
// starts up showing/driving, without touching logic elsewhere
// ================================================================

#define DEFAULT_CUSTOM_DUTY1 200
#define DEFAULT_CUSTOM_DUTY2 100
#define DEFAULT_CUSTOM_DUTY3 30
#define DEFAULT_CUSTOM_DUTY4 30
#define DEFAULT_CUSTOM_DUTY5 100
#define DEFAULT_CUSTOM_DUTY6 200

#define DEFAULT_MONOTONE_DUTY     60
#define DEFAULT_STEREO_LEFT_DUTY  50
#define DEFAULT_STEREO_RIGHT_DUTY 80

#define DEFAULT_ONDULATION_MAX_DUTY_FRAC 0.7f  // 0.0-1.0: 1.0 = dot at top (255 peak)
#define DEFAULT_ONDULATION_DIRECTION     0.5f  // -1.0 = full left, 0 = stopped, +1.0 = full right

#define DEFAULT_NATURAL_MAX_DUTY  200
#define DEFAULT_NATURAL_RATE_DUTY 128

// ================================================================
// TOUCH / GESTURE STATE (shared across all screens)
// ================================================================

bool touchWasDown = false;
int touchStartX = 0, touchStartY = 0, lastTouchY = 0, lastTouchX = 0;
unsigned long touchStartTime = 0; // millis() at touch-down, used for long-press detection

int activeFan = -1;      // Custom screen only: which of the 6 independent bars is being dragged
int activeControl = -1;  // Generic Control-table screens only: index into controls[currentScreen][]
                          // (Ondulation's pad is neither — it commits straight to GESTURE_PAD_DRAG)

// Ondulation's grey feedback-bar area (left of the pad) isn't part of the
// pad itself, but a vertical drag out there should still move the pad's dot
// up/down -- just without touching its left/right (direction) position, so
// it can't be used to change wave direction by accident. ondulationGreyTouch
// marks a touch-down out there (set at touch-down, cleared on release);
// padDragYOnly marks that the *resolved* gesture is this Y-only variant, so
// the continuing-touch GESTURE_PAD_DRAG handler knows to skip the X axis.
bool ondulationGreyTouch = false;
bool padDragYOnly = false;

enum GestureType { GESTURE_NONE,
                    GESTURE_SWIPE,
                    GESTURE_DRAG,
                    GESTURE_PAD_DRAG,
                    GESTURE_LONG_PRESS };
GestureType currentGesture = GESTURE_NONE;
const int GESTURE_THRESHOLD = 12;
float dragSensitivity = 1.5f;

const unsigned long LONG_PRESS_MS = 2000; // hold still this long (without crossing
                                           // GESTURE_THRESHOLD) to register a long press

const int PAD_TOUCH_MARGIN_RIGHT = 20; // extra touch-zone margin on the right (fixes accidental misses near screen edge)
const int PAD_TOUCH_MARGIN_OTHER = 8;  // small margin on other sides for the same reason

// Called once when a long press is recognized anywhere on screen (see
// GESTURE_LONG_PRESS handling in loop()). Action TBD.
//
// NOTE: this only fires for touches that start outside Ondulation's control
// pad. A touch inside the pad commits immediately to GESTURE_PAD_DRAG (by
// design — see the comment above touch-down handling in loop()), so it
// never passes through the GESTURE_NONE state this check runs in. If a
// long press should also work while held still inside the pad, that needs
// a small change to the pad's touch-down handling — say the word and I'll
// add it once the action itself is defined.
void onLongPress() {
  saveDefaultScreen(currentScreen);
  Serial.printf("Long press: \"%s\" screen saved as startup default\n", SCREEN_TITLES[currentScreen]);
  showMessage(String("'") + SCREEN_TITLES[currentScreen] + "' set as startup.");
}

// ================================================================
// GENERIC DRAG-CONTROL SYSTEM
//
// Monotone, Stereotone, Natural Breeze and Pulse all reduce to the same
// shape: one or two vertical bars, each occupying a horizontal touch zone,
// each dragged to adjust one duty value clamped to 0-255. This table
// describes that shape once; the actual touch handling in loop() is a
// single generic block instead of four near-identical copies.
//
// Custom (6 bars addressed via getFanZoneForTouch(), no fixed zone table
// needed) and Ondulation (a genuine 2D pad, not a 1D bar) don't fit this
// shape and are still handled by hand below.
// ================================================================

struct Control {
  int xMin, xMax;               // touch-zone bounds (screen x-coordinates)
  int *duty, *lastDuty;         // live value + last-drawn value (skip redundant redraws)
  const char *guidance;         // text shown in the top strip while dragging
  uint16_t guidanceColor;       // dot color preceding the guidance text -- matches this
                                 // control's own bar/thumb fill color (see showGuidance())
  void (*redraw)(bool active);  // draws this control's own bar + any companion visuals
  void (*apply)(int duty);      // optional: push the new duty to fan PWM outputs; nullptr if
                                 // a periodic update() reads the duty variable directly instead
};

int numControls[NUM_SCREENS] = { 0 };   // how many of the 2 slots below are used per screen
Control controls[NUM_SCREENS][2];       // populated once in setup(), after layout is computed

// ================================================================
// TITLE / GUIDANCE / MESSAGE TEXT (top strip of every screen)
// ================================================================

bool titleVisible = false;
unsigned long titleShownAt = 0;
const unsigned long TITLE_DURATION_MS = 3000;
bool guidanceActive = false;

bool messageActive = false;              // e.g. the "'custom' set as startup." long-press confirmation
unsigned long messageShownAt = 0;
const unsigned long MESSAGE_DURATION_MS = 2000;
#define COLOR_MESSAGE_BG RGB565(4, 17, 16) // background fill behind a message, so it reads as a
                                            // toast rather than plain title text

// ================================================================
// SHARED LAYOUT — bar geometry used across Custom/Monotone/Stereotone/
// the red&blue bars in Natural Breeze, and the grey wave-style bars
// used in Ondulation/Monotone/Stereotone/Natural Breeze
// ================================================================

#define BAR_LEFT_MARGIN 32
#define BAR_WIDTH  24
#define BAR_GAP    20
#define BAR_GAP_LARGE 32
#define BAR_TOP    35
#define BAR_MAX_H  120
#define BAR_BASE_Y (BAR_TOP + BAR_MAX_H)
#define BAR_RADIUS (BAR_WIDTH / 2)
// TITLE_Y, DOTS_Y and TRANSITION_TOP/HEIGHT now live near the top of the
// file (HARDWARE PINS section) -- initPushTransition() needs TRANSITION_HEIGHT
// before this point in the file.

#define WAVE_BAR_WIDTH 16
#define WAVE_BAR_GAP   12
#define WAVE_BAR_LEFT_MARGIN 32
#define WAVE_BAR_TOP    BAR_TOP
#define WAVE_BAR_MAX_H  BAR_MAX_H
#define WAVE_BAR_MIN_H  WAVE_BAR_WIDTH

#define CONTROL_W 100
#define CONTROL_H 120
#define CONTROL_RADIUS 12
#define DOT_SIZE 28  // size of interactive bar handle 'dot'
#define DOT_RADIUS (DOT_SIZE / 2)
#define PAGE_DOT_R 3 // radius page dot indicators

uint16_t barBuffer[BAR_WIDTH * BAR_MAX_H];
uint16_t waveBarBuffer[WAVE_BAR_WIDTH * WAVE_BAR_MAX_H];

// ================================================================
// SHARED HELPERS — small utility functions used by multiple screens
// ================================================================

float fmapf(float x, float inMin, float inMax, float outMin, float outMax) {
  return outMin + (x - inMin) * (outMax - outMin) / (inMax - inMin);
}

// Fills barBuffer with a rounded-capsule track (bgColor) and a single
// value-indicator dot (fillColor) positioned by duty (0-255, bottom-top).
// Used by Custom, Monotone, Stereotone, Natural Breeze and Pulse's bars.
//
// NOTE: this recomputes the full circle-mask geometry every call even
// though the track shape itself is static — only the dot position moves.
// At a 24x120 (or 16x120) buffer and ~25fps this is cheap on the ESP32,
// so it's left as-is; worth revisiting only if you ever see frame-timing
// issues on this path specifically.
void renderBarToBuffer(int duty, uint16_t bgColor, uint16_t fillColor) {
  const int W = BAR_WIDTH, H = BAR_MAX_H;
  const int outerR = W / 2;
  const int outerCapBottomStart = H - outerR;
  const int dotR = W / 2;
  int dotCenterY = map(duty, 0, 255, H - dotR, dotR);

  for (int py = 0; py < H; py++) {
    for (int px = 0; px < W; px++) {
      bool insideOuter;
      if (py < outerR) {
        int dx = px - outerR, dy = py - outerR;
        insideOuter = (dx * dx + dy * dy) <= (outerR * outerR);
      } else if (py >= outerCapBottomStart) {
        int dx = px - outerR, dy = py - (H - outerR - 1);
        insideOuter = (dx * dx + dy * dy) <= (outerR * outerR);
      } else {
        insideOuter = true;
      }

      int ddx = px - outerR, ddy = py - dotCenterY;
      bool insideDot = (ddx * ddx + ddy * ddy) <= (dotR * dotR);

      barBuffer[py * W + px] = insideDot ? fillColor : insideOuter ? bgColor
                                                                    : RGB565_BLACK;
    }
  }
}

// Convenience overload: default red bar colors (used by Custom/Monotone/Stereotone/Pulse's red bar)
void renderBarToBuffer(int duty) {
  renderBarToBuffer(duty, COLOR_BAR_BG, COLOR_BAR_FILL);
}

// Renders + blits a single control bar in one call. Every screen's
// "draw my bar" function (drawBar, drawMonotoneAll, drawStereoLeft/Right,
// drawNaturalRedBar/BlueBar, drawPulseRedBar/SpeedBar) is now a thin
// wrapper around this, picking the (x, duty, colors) for its own bar.
void drawControlBar(int x, int duty, uint16_t bg, uint16_t bgActive, uint16_t fill, bool active) {
  renderBarToBuffer(duty, active ? bgActive : bg, fill);
  gfx->draw16bitRGBBitmap(x, BAR_TOP, barBuffer, BAR_WIDTH, BAR_MAX_H);
}

// Fills waveBarBuffer with a solid grey capsule, height driven by duty (0-255).
// Used by every screen's non-interactive "feedback" bars.
void renderWaveBarToBuffer(int h) {
  const int W = WAVE_BAR_WIDTH, H = WAVE_BAR_MAX_H;
  h = constrain(h, WAVE_BAR_MIN_H, H);

  int shapeTop = H - h;
  int r = min(W, h) / 2;
  int capBottomStart = h - r;

  for (int py = 0; py < H; py++) {
    for (int px = 0; px < W; px++) {
      bool inside = false;
      if (py >= shapeTop) {
        int localY = py - shapeTop;
        if (localY < r) {
          int dx = px - W / 2, dy = localY - r;
          inside = (dx * dx + dy * dy) <= (r * r);
        } else if (localY >= capBottomStart) {
          int dx = px - W / 2, dy = localY - (h - r - 1);
          inside = (dx * dx + dy * dy) <= (r * r);
        } else {
          inside = true;
        }
      }
      waveBarBuffer[py * W + px] = inside ? COLOR_WAVE_BAR : RGB565_BLACK;
    }
  }
}

void drawWaveBarAt(int x, int duty) {
  int h = map(duty, 0, 255, WAVE_BAR_MIN_H, WAVE_BAR_MAX_H);
  renderWaveBarToBuffer(h);
  gfx->draw16bitRGBBitmap(x, WAVE_BAR_TOP, waveBarBuffer, WAVE_BAR_WIDTH, WAVE_BAR_MAX_H);
}

// Draws `count` wave bars at positions x[0..count-1], all at the same duty.
// Replaces the repeated "for (i<NUM_FANS) drawWaveBarAt(...)" loops that
// used to appear in Ondulation/Natural Breeze/Pulse's screen-draw functions.
void drawAllWaveBars(const int x[], int count, int duty) {
  for (int i = 0; i < count; i++) drawWaveBarAt(x[i], duty);
}

void drawPageDots() {
  int totalWidth = NUM_SCREENS * 12;
  int startX = (gfx->width() - totalWidth) / 2;
  for (int i = 0; i < NUM_SCREENS; i++) {
    uint16_t color = (i == currentScreen) ? COLOR_DOT_ACTIVE : COLOR_DOT_INACTIVE;
    gfx->fillCircle(startX + i * 12 + 4, DOTS_Y, PAGE_DOT_R, color);
  }
}

// Centered text in the top strip — used for both the screen title and guidance text.
void drawTitle(const char *title) {
  gfx->setFont(&FreeSans9pt7b);
  gfx->setTextColor(GREY565(8));

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((gfx->width() - w) / 2, TITLE_Y + 22);
  gfx->print(title);
  gfx->setFont();
}

// Like drawTitle(), but precedes the (still centered-as-a-group) text with a
// small dot -- same size/shape as a page dot -- in dotColor. Used only for
// guidance text, to tie it visually back to whichever control is being
// dragged (dotColor matches that control's own bar/thumb fill color).
#define GUIDANCE_DOT_GAP 8 // gap between the dot and the start of the text
void drawGuidanceTitle(const char *title, uint16_t dotColor) {
  gfx->setFont(&FreeSans9pt7b);
  gfx->setTextColor(GREY565(8));

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(title, 0, 0, &x1, &y1, &w, &h);

  int totalWidth = (PAGE_DOT_R * 2) + GUIDANCE_DOT_GAP + w;
  int startX = (gfx->width() - totalWidth) / 2;

  gfx->fillCircle(startX + PAGE_DOT_R, TITLE_Y + 17, PAGE_DOT_R, dotColor);

  gfx->setCursor(startX + (PAGE_DOT_R * 2) + GUIDANCE_DOT_GAP, TITLE_Y + 22);
  gfx->print(title);
  gfx->setFont();
}

// --- Title / guidance / message show-hide ---
// showTitle(): screen name, auto-hides after TITLE_DURATION_MS (see loop())
// showGuidance()/hideGuidance(): contextual label shown only while actively
// dragging a control; takes over the same top strip, and once shown, the
// original title does NOT reappear when guidance ends (stays blank until
// the next navigation event).
// showMessage()/hideMessage(): temporary feedback (e.g. a long-press
// confirmation), auto-hides after MESSAGE_DURATION_MS (see loop()) — same
// non-blocking pattern as the title's auto-hide, deliberately NOT a
// delay(), since that would freeze every other screen's animation for
// as long as the message is up.
//
// All three share the same strip, so each show*() call clears the other
// two's "active" flags before drawing — otherwise whichever one's timer
// happens to fire last would blank a strip that something else has since
// taken over (e.g. a message's 2s timer wiping out guidance text from a
// drag the user started 1s into the message being shown).

void showTitle() {
  titleVisible = true;
  guidanceActive = false;
  messageActive = false;
  titleShownAt = millis();
  drawTitle(SCREEN_TITLES[currentScreen]);
}

void hideTitle() {
  titleVisible = false;
  gfx->fillRect(0, TITLE_Y, gfx->width(), 30, RGB565_BLACK);
}

void showGuidance(const char *text, uint16_t dotColor) {
  guidanceActive = true;
  titleVisible = false;
  messageActive = false;
  gfx->fillRect(0, TITLE_Y, gfx->width(), 30, RGB565_BLACK);
  drawGuidanceTitle(text, dotColor);
}

void hideGuidance() {
  guidanceActive = false;
  gfx->fillRect(0, TITLE_Y, gfx->width(), 30, RGB565_BLACK);
}

void showMessage(const String &msg) {
  messageActive = true;
  titleVisible = false;
  guidanceActive = false;
  messageShownAt = millis();

  gfx->fillRect(0, TITLE_Y, gfx->width(), 30, COLOR_MESSAGE_BG);
  gfx->setFont(&FreeSans9pt7b);
  gfx->setTextColor(GREY565(20));
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((gfx->width() - w) / 2, TITLE_Y + 22);
  gfx->print(msg);
  gfx->setFont();
}

void hideMessage() {
  messageActive = false;
  gfx->fillRect(0, TITLE_Y, gfx->width(), 30, RGB565_BLACK);
}

// Directional wipe used during screen transitions (see goToScreen()).
// direction: +1 = wipe left-to-right, -1 = wipe right-to-left.
void wipeTransition(int direction) {
  const int steps = 30;
  const int stepWidth = gfx->width() / steps;

  for (int i = 0; i < steps; i++) {
    int x = (direction > 0) ? (i * stepWidth) : (gfx->width() - (i + 1) * stepWidth);
    gfx->fillRect(x, 0, stepWidth, DOTS_Y-3, RGB565_BLACK); // wipe all except the page dots along the bottom
    delay(8);
  }
  gfx->fillRect(0, 0, gfx->width(), DOTS_Y-3, RGB565_BLACK); // cover integer-division leftover
}

// "Push" transition: the incoming screen slides in and pushes the outgoing
// one off-screen, instead of wiping to black and fading the backlight.
//
// direction: +1 = outgoing screen exits left, incoming screen enters from
//                 the right (used going to the "next" screen)
//           -1 = outgoing screen exits right, incoming screen enters from
//                 the left (used going to the "previous" screen)
//
// The outgoing and incoming screens are each rendered once into their own
// off-screen PSRAM canvas (by temporarily pointing the shared `gfx`
// pointer at the canvas and calling the normal draw*Screen() functions
// unmodified -- still at full-screen coordinates, since BAR_TOP/DOTS_Y/etc
// are absolute), then every animation frame composites one shifted
// combination of just the TRANSITION_TOP..TRANSITION_TOP+TRANSITION_HEIGHT
// band of the two canvases into `transitionComposite` and blits that
// (smaller) region to the real screen with a single draw16bitRGBBitmap()
// call.
//
// The title strip and page dots are NOT part of the animation -- they're
// left untouched on the real screen throughout the slide, and only
// snapped to the new screen's state once, after the loop. That's both the
// "fix the dots in place" step (now done for the title too) and a
// performance fix: every real-screen frame is TRANSITION_HEIGHT rows
// instead of a full 172, which on this bus (see goToScreen()'s comment
// history: Arduino_ESP32SPI has no DMA and is capped at 32-pixel hardware
// FIFO bursts, so per-frame cost scales with pixel count) is a direct,
// proportional speedup.
void pushTransition(int direction) {
  const int W = gfx->width();
  Arduino_GFX *realGfx = gfx;

  // --- Render the outgoing (current) screen into the "old" canvas ---
  gfx = transitionCanvasOld;
  redrawCurrentScreen();

  // --- Advance state, then render the incoming screen into the "new" canvas ---
  currentScreen = (currentScreen + direction + NUM_SCREENS) % NUM_SCREENS;
  gfx = transitionCanvasNew;
  onEnterScreen(currentScreen);
  redrawCurrentScreen();

  gfx = realGfx;

  uint16_t *oldBuf = transitionCanvasOld->getFramebuffer();
  uint16_t *newBuf = transitionCanvasNew->getFramebuffer();

  const int steps = 10;
  for (int step = 1; step <= steps; step++) {
    // Ease-out cubic: fast at the start (like a flicked swipe), slowing
    // down as the new screen settles into place, instead of a constant
    // linear rate.
    float t = (float)step / steps;
    float inv = 1.0f - t;
    float eased = 1.0f - inv * inv * inv;
    int p = (int)(W * eased + 0.5f); // total pixels shifted so far, 0..W
    if (p > W) p = W;                // guard against float rounding on the last step

    for (int row = 0; row < TRANSITION_HEIGHT; row++) {
      int y = TRANSITION_TOP + row;
      uint16_t *destRow = transitionComposite + row * W;
      const uint16_t *oldRow = oldBuf + y * W;
      const uint16_t *newRow = newBuf + y * W;

      if (direction > 0) {
        // old shifts left by p; new enters from the right
        int oldVisible = W - p;
        if (oldVisible > 0) memcpy(destRow, oldRow + p, oldVisible * 2);
        if (p > 0) memcpy(destRow + oldVisible, newRow, p * 2);
      } else {
        // old shifts right by p; new enters from the left
        int newVisible = p;
        if (newVisible > 0) memcpy(destRow, newRow + (W - p), newVisible * 2);
        if (W - p > 0) memcpy(destRow + newVisible, oldRow, (W - p) * 2);
      }
    }

    gfx->draw16bitRGBBitmap(0, TRANSITION_TOP, transitionComposite, W, TRANSITION_HEIGHT);
  }

  // Snap the title strip and page dots to the new screen's state now that
  // navigation has landed -- they held the outgoing screen's state,
  // unchanged, for the whole animation above.
  hideTitle();
  showTitle();
  drawPageDots();
}


// ================================================================
// SCREEN: CUSTOM — 6 independently draggable bars
// ================================================================

int barX[NUM_FANS];
int fanDuty[NUM_FANS] = {
  DEFAULT_CUSTOM_DUTY1, DEFAULT_CUSTOM_DUTY2, DEFAULT_CUSTOM_DUTY3,
  DEFAULT_CUSTOM_DUTY4, DEFAULT_CUSTOM_DUTY5, DEFAULT_CUSTOM_DUTY6
};
int lastFanDuty[NUM_FANS] = { -1, -1, -1, -1, -1, -1 };

void drawBar(int i, bool active = false) {
  drawControlBar(barX[i], fanDuty[i], COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
}

void drawCustomScreen() {
  gfx->fillScreen(RGB565_BLACK);
  for (int i = 0; i < NUM_FANS; i++) drawBar(i);
  drawPageDots();
}

// Touch-zone lookup: returns which of the 6 bars (0-5) a given x-coordinate
// belongs to, using the midpoint between adjacent bars as the boundary.
// (This is Custom's own equivalent of a Control's xMin/xMax zone — kept
// separate from the generic table since it's naturally a 6-way lookup
// rather than 2 fixed ranges.)
int getFanZoneForTouch(int tx) {
  for (int i = 0; i < NUM_FANS - 1; i++) {
    int boundary = (barX[i] + BAR_WIDTH + barX[i + 1]) / 2;
    if (tx < boundary) return i;
  }
  return NUM_FANS - 1;
}

void enterCustom() {
  for (int i = 0; i < NUM_FANS; i++) ledcWrite(FAN_PINS[i], fanDuty[i]);
}

// ================================================================
// SCREEN: MONOTONE — one bar drives all 6 fans, flanked by grey echoes
// ================================================================

#define MONO_GREY_WIDTH  16
#define MONO_GREY_GAP    12
#define MONO_CENTER_GAP  20  // gap between the red bar and each grey group

int monotoneBarX;
int monotoneGreyBarX[NUM_FANS]; // 0,1,2 = left group; 3,4,5 = right group
int monotoneDuty = DEFAULT_MONOTONE_DUTY;
int lastMonotoneDuty = -1;

// Draws the red control bar plus its flanking grey "echo" bars, all at the
// current duty. Used for the initial screen draw AND as this screen's
// Control.redraw callback (called on every drag update).
void drawMonotoneAll(bool active = false) {
  drawControlBar(monotoneBarX, monotoneDuty, COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
  drawAllWaveBars(monotoneGreyBarX, NUM_FANS, monotoneDuty);
}

void applyMonotone(int duty) {
  for (int i = 0; i < NUM_FANS; i++) ledcWrite(FAN_PINS[i], duty);
}

void drawMonotoneScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawMonotoneAll(false);
  drawPageDots();
}

void enterMonotone() {
  applyMonotone(monotoneDuty);
}

// ================================================================
// SCREEN: STEREOTONE — two bars, each driving a trio of fans (L/R)
// ================================================================

#define STEREO_GREY_WIDTH 16
#define STEREO_GREY_GAP   12
#define STEREO_GREY_GROUP_GAP 24

int stereoRedBarX[2];
int stereoGreyBarX[NUM_FANS]; // 0,1,2 = left group; 3,4,5 = right group
int leftDuty = DEFAULT_STEREO_LEFT_DUTY;
int rightDuty = DEFAULT_STEREO_RIGHT_DUTY;
int lastLeftDuty = -1, lastRightDuty = -1;

void drawStereoLeft(bool active = false) {
  drawControlBar(stereoRedBarX[0], leftDuty, COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
  drawAllWaveBars(&stereoGreyBarX[0], 3, leftDuty);
}

void drawStereoRight(bool active = false) {
  drawControlBar(stereoRedBarX[1], rightDuty, COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
  drawAllWaveBars(&stereoGreyBarX[3], 3, rightDuty);
}

void applyStereoLeft(int duty)  { for (int i = 0; i < 3; i++) ledcWrite(FAN_PINS[i], duty); }
void applyStereoRight(int duty) { for (int i = 3; i < NUM_FANS; i++) ledcWrite(FAN_PINS[i], duty); }

void drawStereotoneScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawStereoLeft();
  drawStereoRight();
  drawPageDots();
}

void enterStereotone() {
  applyStereoLeft(leftDuty);
  applyStereoRight(rightDuty);
}

// ================================================================
// SCREEN: ONDULATION — traveling sine wave across all 6 fans,
// shaped by a 2D joystick-style control pad (amplitude + direction/speed)
// ================================================================

int controlX, controlY;      // pad's screen position (computed in setup, after rotation is set)
float dotX, dotY;            // dot's position, local to the pad (0,0 = pad's top-left)
int waveBarX[NUM_FANS];      // shared with Natural Breeze's feedback bars too
float wavePhase = 0.0f;
unsigned long lastWaveUpdate = 0;

void clampDot() {
  dotX = constrain(dotX, (float)DOT_RADIUS, (float)(CONTROL_W - DOT_RADIUS - 1));
  dotY = constrain(dotY, (float)DOT_RADIUS, (float)(CONTROL_H - DOT_RADIUS - 1));
}

void drawControlPad(bool active = false) {
  uint16_t bg = active ? COLOR_CONTROL_BG_ACTIVE : COLOR_CONTROL_BG;
  gfx->fillRoundRect(controlX, controlY, CONTROL_W, CONTROL_H, CONTROL_RADIUS, bg);

  // Crosshair through the thumb's center, spanning the full width/height of
  // the pad -- drawn after the background but before the thumb, so the
  // thumb sits on top of it and only the arms beyond the thumb are visible.
  int cx = controlX + (int)dotX;
  int cy = controlY + (int)dotY;
  gfx->fillRect(controlX, cy - CROSSHAIR_WIDTH / 2, CONTROL_W, CROSSHAIR_WIDTH, COLOR_CONTROL_CROSSHAIR);
  gfx->fillRect(cx - CROSSHAIR_WIDTH / 2, controlY, CROSSHAIR_WIDTH, CONTROL_H, COLOR_CONTROL_CROSSHAIR);

  gfx->fillCircle(cx, cy, DOT_RADIUS, COLOR_CONTROL_DOT);
}

// dotX -> angular speed/direction: center = stopped, full left/right = +-1 cycle/3s
float computeWaveAngularSpeed() {
  float norm = (dotX - CONTROL_W / 2.0f) / (CONTROL_W / 2.0f);
  return norm * (2.0f * PI / 3.0f);
}

// dotY -> peak duty each fan can reach: top of pad = 255, bottom = ~64
int computeWaveMaxDuty() {
  return map((int)dotY, DOT_RADIUS, CONTROL_H - DOT_RADIUS - 1, 255, 64);
}

void updateOndulation() {
  unsigned long now = millis();
  float dt = (now - lastWaveUpdate) / 1000.0f;
  lastWaveUpdate = now;

  wavePhase += computeWaveAngularSpeed() * dt;
  int maxDuty = computeWaveMaxDuty();

  for (int i = 0; i < NUM_FANS; i++) {
    float phase_i = wavePhase - i * (2.0f * PI / NUM_FANS);
    float s = (sin(phase_i) + 1.0f) / 2.0f;
    int duty = (int)(s * maxDuty);

    ledcWrite(FAN_PINS[i], duty);
    drawWaveBarAt(waveBarX[i], duty);
  }
}

void drawOndulationScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawAllWaveBars(waveBarX, NUM_FANS, 0);
  drawControlPad();
  drawPageDots();
}

void enterOndulation() {
  lastWaveUpdate = millis(); // reset timing baseline to avoid a phase jump
}

// ================================================================
// SCREEN: NATURAL BREEZE — per-fan randomized gusts, eased toward
// shifting targets; red bar caps peak gust, blue bar sets how often/
// how briskly targets change ("gustiness")
// ================================================================

int naturalRedBarX, naturalBlueBarX;
int naturalMaxDuty = DEFAULT_NATURAL_MAX_DUTY;
int naturalRateDuty = DEFAULT_NATURAL_RATE_DUTY;
int lastNaturalMaxDuty = -1;
int lastNaturalRateDuty = -1;

float fanCurrentDuty[NUM_FANS] = { 0 };
float fanTargetDuty[NUM_FANS] = { 0 };
unsigned long fanNextChangeTime[NUM_FANS] = { 0 };

void drawNaturalRedBar(bool active = false) {
  drawControlBar(naturalRedBarX, naturalMaxDuty, COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
}

void drawNaturalBlueBar(bool active = false) {
  drawControlBar(naturalBlueBarX, naturalRateDuty, COLOR_BLUE_BAR_BG, COLOR_BLUE_BAR_BG_ACTIVE, COLOR_BLUE_BAR_FILL, active);
}
// No `apply` callback needed for either bar: updateNaturalBreeze() reads
// naturalMaxDuty/naturalRateDuty directly every frame, so dragging them
// just needs the redraw above — the Control.apply pointer stays nullptr.

void drawNaturalBreezeScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawAllWaveBars(waveBarX, NUM_FANS, 0);
  drawNaturalRedBar();
  drawNaturalBlueBar();
  drawPageDots();
}

void updateNaturalBreeze() {
  unsigned long now = millis();
  float rateNorm = naturalRateDuty / 255.0f;

  float easeSpeed = fmapf(rateNorm, 0.0f, 1.0f, 0.01f, 0.15f);
  unsigned long intervalMin = (unsigned long)fmapf(rateNorm, 0.0f, 1.0f, 4000, 400);
  unsigned long intervalMax = intervalMin * 2;

  for (int i = 0; i < NUM_FANS; i++) {
    if (now >= fanNextChangeTime[i]) {
      float r = random(0, 1001) / 1000.0f;
      fanTargetDuty[i] = naturalMaxDuty * (r * r); // squared -> mostly calm, occasional gust
      fanNextChangeTime[i] = now + random(intervalMin, intervalMax + 1);
    }

    fanTargetDuty[i] = min(fanTargetDuty[i], (float)naturalMaxDuty); // re-clamp if max lowered mid-gust
    fanCurrentDuty[i] += (fanTargetDuty[i] - fanCurrentDuty[i]) * easeSpeed;

    int duty = (int)fanCurrentDuty[i];
    ledcWrite(FAN_PINS[i], duty);
    drawWaveBarAt(waveBarX[i], duty);
  }
}

void enterNaturalBreeze() {
  for (int i = 0; i < NUM_FANS; i++) {
    fanNextChangeTime[i] = millis() + random(0, 1500); // stagger first gusts
  }
}

// ================================================================
// SCREEN: PULSE — synchronized growth, then staggered outward-in decay
// ================================================================

int pulseRedBarX, pulseBlueBarX;
int pulseGreyBarX[NUM_FANS];

int pulseMaxDuty = 200;      // red bar: peak duty each fan grows to
int pulseSpeedDuty = 128;    // blue bar: overall cycle speed
int lastPulseMaxDuty = -1;
int lastPulseSpeedDuty = -1;

// Decay stagger points, as a fraction of one pair's own decay duration.
// Pair 2 starts when pair 1 reaches this fraction of its decay; pair 3
// starts when pair 2 reaches this fraction of ITS decay (see updatePulse()).
// Tweak these to change how much the pairs overlap.
float PULSE_STAGGER_START = 0.5f; // pair N+1 starts when pair N is this far through decay

enum PulsePhase { PULSE_GROWTH,
                   PULSE_PAUSE_TOP,
                   PULSE_DECAY,
                   PULSE_PAUSE_BOTTOM };
PulsePhase pulsePhase = PULSE_GROWTH;
unsigned long pulsePhaseStart = 0;

// Growth: expressed as a RATE (duty units per ms), not a fixed duration —
// this makes growth speed independent of peak duty (red bar). Reaching a
// LOWER peak takes LESS time at the same rate; reaching a HIGHER peak
// takes MORE time. At top speed, the rate is fast enough to cross the
// full 0-255 range in 200ms; at slowest speed, in 1000ms.
const float PULSE_GROWTH_FULL_RANGE_MS_FAST = 200.0f;
const float PULSE_GROWTH_FULL_RANGE_MS_SLOW = 500.0f;

// Pause at the top (after reaching peak, before decay starts) — DOES scale
// with peak duty implicitly through timing alone, not duration itself.
const float PULSE_PAUSE_TOP_MS_FAST = 500.0f;
const float PULSE_PAUSE_TOP_MS_SLOW = 2000.0f;

// Decay: fixed duration for one pair's full decay, independent of peak duty
// (unchanged from before — this was already correct).
const unsigned long PULSE_DECAY_MS_BASE = 1200;
const unsigned long PULSE_PAUSE_BOTTOM_MS_BASE = 400;


float pulseFanDuty[NUM_FANS] = { 0 };

void drawPulseRedBar(bool active = false) {
  drawControlBar(pulseRedBarX, pulseMaxDuty, COLOR_BAR_BG, COLOR_BAR_BG_ACTIVE, COLOR_BAR_FILL, active);
}

void drawPulseSpeedBar(bool active = false) {
  drawControlBar(pulseBlueBarX, pulseSpeedDuty, COLOR_YELLOW_BAR_BG, COLOR_YELLOW_BAR_BG_ACTIVE, COLOR_YELLOW_BAR_FILL, active);
}
// Same reasoning as Natural Breeze: updatePulse() reads pulseMaxDuty/
// pulseSpeedDuty directly, so no `apply` callback is needed here either.

void drawPulseScreen() {
  gfx->fillScreen(RGB565_BLACK);
  drawAllWaveBars(pulseGreyBarX, NUM_FANS, 0);
  drawPulseRedBar();
  drawPulseSpeedBar();
  drawPageDots();
}

// Ease-out: fast initial drop, slowing near the end. t: 0=start, 1=finished.
float easeOutQuad(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

void updatePulse() {
  unsigned long now = millis();
  float speedNorm = pulseSpeedDuty / 255.0f; // 0..1, higher = faster cycle
  unsigned long elapsed = now - pulsePhaseStart;

  // Decay/bottom-pause still scale via the same multiplier approach as before —
  // these durations are independent of peak duty, only tied to the speed bar.
  float speedScale = fmapf(speedNorm, 0.0f, 1.0f, 2.5f, 0.6f);
  unsigned long decayMs = (unsigned long)(PULSE_DECAY_MS_BASE * speedScale);
  unsigned long pauseBottomMs = (unsigned long)(PULSE_PAUSE_BOTTOM_MS_BASE * speedScale);

  switch (pulsePhase) {

    case PULSE_GROWTH: {
      // Rate-based growth: duty climbs at a constant rate (duty/ms) set by
      // the speed bar alone — NOT scaled by pulseMaxDuty. So a low peak is
      // reached quickly, a high peak takes proportionally longer, but the
      // rate of climb itself never changes.
      float fullRangeMs = fmapf(speedNorm, 0.0f, 1.0f,
                                 PULSE_GROWTH_FULL_RANGE_MS_SLOW,
                                 PULSE_GROWTH_FULL_RANGE_MS_FAST);
      float growthRate = 255.0f / fullRangeMs; // duty units per ms

      float duty = growthRate * elapsed;

      if (duty >= pulseMaxDuty) {
        for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = pulseMaxDuty;
        pulsePhase = PULSE_PAUSE_TOP;
        pulsePhaseStart = now;
      } else {
        for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = duty;
      }
      break;
    }

    case PULSE_PAUSE_TOP: {
      for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = pulseMaxDuty;

      float topPauseMs = fmapf(speedNorm, 0.0f, 1.0f,
                                PULSE_PAUSE_TOP_MS_SLOW,
                                PULSE_PAUSE_TOP_MS_FAST);

      if (elapsed >= topPauseMs) {
        pulsePhase = PULSE_DECAY;
        pulsePhaseStart = now;
      }
      break;
    }

    case PULSE_DECAY: {
      unsigned long pair2Start = (unsigned long)(decayMs * PULSE_STAGGER_START);
      unsigned long pair3Start = pair2Start + (unsigned long)(decayMs * PULSE_STAGGER_START);
      unsigned long totalDecayMs = pair3Start + decayMs;

      unsigned long pairStartTimes[3] = { 0, pair2Start, pair3Start };
      int fanPair[NUM_FANS] = { 0, 1, 2, 2, 1, 0 };

      for (int i = 0; i < NUM_FANS; i++) {
        int pair = fanPair[i];
        long localElapsed = (long)elapsed - (long)pairStartTimes[pair];

        if (localElapsed <= 0) {
          pulseFanDuty[i] = pulseMaxDuty;
        } else {
          float t = (float)localElapsed / decayMs;
          float eased = easeOutQuad(t);
          pulseFanDuty[i] = pulseMaxDuty * (1.0f - eased);
        }
      }

      if (elapsed >= totalDecayMs) {
        for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = 0;
        pulsePhase = PULSE_PAUSE_BOTTOM;
        pulsePhaseStart = now;
      }
      break;
    }

    case PULSE_PAUSE_BOTTOM: {
      for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = 0;
      if (elapsed >= pauseBottomMs) {
        pulsePhase = PULSE_GROWTH;
        pulsePhaseStart = now;
      }
      break;
    }
  }

  for (int i = 0; i < NUM_FANS; i++) {
    int duty = (int)pulseFanDuty[i];
    ledcWrite(FAN_PINS[i], duty);
    drawWaveBarAt(pulseGreyBarX[i], duty);
  }
}

void enterPulse() {
  pulsePhase = PULSE_GROWTH;
  pulsePhaseStart = millis();
  for (int i = 0; i < NUM_FANS; i++) pulseFanDuty[i] = 0;
}

// ================================================================
// STARTUP SPLASH SCREEN — shown once at boot, not part of the swipeable
// NUM_SCREENS set (no page dots, not in screens[]). Image is a fixed
// RGB565 asset (startup_image.h, generated from startup_asset.png); the
// text is rendered live so it's a one-line edit to change, not a re-export.
// ================================================================

#define STARTUP_TEXT "Six fans. One controller."
#define STARTUP_TEXT_GAP 20 // px between the bottom of the image and the text baseline area

void drawStartupScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setFont(&FreeSans9pt7b);
  gfx->setTextColor(GREY565(8));
  int16_t tx1, ty1;
  uint16_t textW, textH;
  gfx->getTextBounds(STARTUP_TEXT, 0, 0, &tx1, &ty1, &textW, &textH);

  // Image + gap + text as one block, centered vertically and horizontally.
  int contentH = STARTUP_IMAGE_HEIGHT + STARTUP_TEXT_GAP + textH;
  int imgX = (gfx->width() - STARTUP_IMAGE_WIDTH) / 2;
  int imgY = (gfx->height() - contentH) / 2;

  gfx->draw16bitRGBBitmap(imgX, imgY, (uint16_t *)startupImage, STARTUP_IMAGE_WIDTH, STARTUP_IMAGE_HEIGHT);

  gfx->setCursor((gfx->width() - textW) / 2, imgY + STARTUP_IMAGE_HEIGHT + STARTUP_TEXT_GAP + textH);
  gfx->print(STARTUP_TEXT);
  gfx->setFont();
}

// ================================================================
// SCREEN NAVIGATION
//
// screens[] is a function-pointer table (draw/enter/update) indexed by
// screen constant. It replaces two switch statements (redrawCurrentScreen,
// onEnterScreen) and the per-screen "is this screen animated?" checks that
// used to live in loop(). Adding a 7th screen now means one more table row
// instead of touching three separate switches — and forgetting one is a
// compile error (unset entry -> null pointer) rather than a silent
// fallback to a blank placeholder screen.
// ================================================================

struct ScreenDef {
  void (*draw)();          // full redraw, called on every navigation
  void (*enter)();         // one-time setup when navigation lands here (may be nullptr)
  void (*update)();        // called every ~40ms while this screen is active (may be nullptr)
};

ScreenDef screens[NUM_SCREENS];

void initScreenTable() {
  screens[SCREEN_NATURAL_BREEZE] = { drawNaturalBreezeScreen, enterNaturalBreeze, updateNaturalBreeze };
  screens[SCREEN_ONDULATION]     = { drawOndulationScreen,   enterOndulation,   updateOndulation };
  screens[SCREEN_MONOTONE]       = { drawMonotoneScreen,     enterMonotone,     nullptr };
  screens[SCREEN_STEREOTONE]     = { drawStereotoneScreen,   enterStereotone,   nullptr };
  screens[SCREEN_CUSTOM]         = { drawCustomScreen,       enterCustom,       nullptr };
  screens[SCREEN_PULSE]          = { drawPulseScreen,        enterPulse,        updatePulse };
}

void redrawCurrentScreen() {
  screens[currentScreen].draw();
}

// Called once whenever navigation lands on a screen — each mode takes
// full ownership of the fan PWM outputs here, so nothing is left over
// from whatever the previously-active mode was driving.
void onEnterScreen(int screen) {
  if (screens[screen].enter) screens[screen].enter();
}

// Navigates to the next/previous screen (delta = +1 or -1). Normally uses
// the PSRAM-backed push transition (new screen slides in, pushing the old
// one out); falls back to the original wipe + backlight fade if the PSRAM
// canvases failed to allocate in initPushTransition().
void goToScreen(int delta) {
  if (transitionReady) {
    pushTransition(delta > 0 ? +1 : -1);
  } else {
    wipeTransition(delta > 0 ? -1 : +1);

    setBacklight(BACKLIGHT_OFF); // screen already black from the wipe; kill backlight before drawing new content

    currentScreen = (currentScreen + delta + NUM_SCREENS) % NUM_SCREENS;
    onEnterScreen(currentScreen);
    redrawCurrentScreen();
    showTitle();
    fadeBacklightIn();
  }

  lastActivityTime = millis(); // don't let the transition itself count as idle time
}

// ================================================================
// DISPLAY INITIALIZATION (ST7789 register sequence, board-specific)
// ================================================================

void lcd_reg_init(void) {
  static const uint8_t init_operations[] = {
    BEGIN_WRITE, WRITE_COMMAND_8, 0x11, END_WRITE, DELAY, 120,
    BEGIN_WRITE,
    WRITE_C8_D16, 0xDF, 0x98, 0x53, WRITE_C8_D8, 0xB2, 0x23,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 4, 0x00, 0x47, 0x00, 0x6F,
    WRITE_COMMAND_8, 0xBB, WRITE_BYTES, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,
    WRITE_C8_D16, 0xC0, 0x44, 0xA4, WRITE_C8_D8, 0xC1, 0x16,
    WRITE_COMMAND_8, 0xC3, WRITE_BYTES, 8, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,
    WRITE_COMMAND_8, 0xC4, WRITE_BYTES, 12, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,
    WRITE_COMMAND_8, 0xC8, WRITE_BYTES, 32,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
    0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
    WRITE_COMMAND_8, 0xD0, WRITE_BYTES, 5, 0x04, 0x06, 0x6B, 0x0F, 0x00,
    WRITE_C8_D16, 0xD7, 0x00, 0x30, WRITE_C8_D8, 0xE6, 0x14, WRITE_C8_D8, 0xDE, 0x01,
    WRITE_COMMAND_8, 0xB7, WRITE_BYTES, 5, 0x03, 0x13, 0xEF, 0x35, 0x35,
    WRITE_COMMAND_8, 0xC1, WRITE_BYTES, 3, 0x14, 0x15, 0xC0,
    WRITE_C8_D16, 0xC2, 0x06, 0x3A, WRITE_C8_D16, 0xC4, 0x72, 0x12,
    WRITE_C8_D8, 0xBE, 0x00, WRITE_C8_D8, 0xDE, 0x02,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x00, 0x02, 0x00,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x01, 0x02, 0x00,
    WRITE_C8_D8, 0xDE, 0x00, WRITE_C8_D8, 0x35, 0x00, WRITE_C8_D8, 0x3A, 0x05,
    WRITE_COMMAND_8, 0x2A, WRITE_BYTES, 4, 0x00, 0x22, 0x00, 0xCD,
    WRITE_COMMAND_8, 0x2B, WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0x3F,
    WRITE_C8_D8, 0xDE, 0x02,
    WRITE_COMMAND_8, 0xE5, WRITE_BYTES, 3, 0x00, 0x02, 0x00,
    WRITE_C8_D8, 0xDE, 0x00, WRITE_C8_D8, 0x36, 0x00, WRITE_COMMAND_8, 0x21,
    END_WRITE, DELAY, 10,
    BEGIN_WRITE, WRITE_COMMAND_8, 0x29, END_WRITE
  };
  bus->batchOperation(init_operations, sizeof(init_operations));
}

// ================================================================
// SETUP
// ================================================================

void setup(void) {
  // --- Kill the backlight before anything else ---
  // On power-up, GFX_BL can float or sit pulled high before this code runs,
  // lighting up whatever garbage happens to be in the display's RAM — that's
  // the brief white flash. Driving it low here, before gfx->begin()/
  // lcd_reg_init() even run, means there's nothing to see until we choose to
  // show it. ledcAttach() below takes over the pin later; writing it low
  // with plain digitalWrite() first means there's no gap where the PWM
  // channel is uninitialized but the pin is undriven.
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, LOW);

  Serial.begin(115200);
  delay(1000);

  initNvs(); // used later by loadDefaultScreen()/saveDefaultScreen(); harmless to do early

  // --- Display bring-up ---
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, 0);
  delay(10);
  digitalWrite(LCD_RST, 1);

  if (!gfx->begin(DISPLAY_SPI_SPEED)) Serial.println("gfx->begin() failed!");
  lcd_reg_init();
  gfx->setRotation(ROTATION); // must happen before any gfx->width()/height() calls below

  initPushTransition(); // allocate the PSRAM canvases now that gfx->width()/height() are valid
#if FORCE_WIPE_TRANSITION
  transitionReady = false; // diagnostic override -- see the #define near the top of the file
#endif

  // --- Backlight: hand the pin to PWM, but keep it OFF for now ---
  // Only turned on below, once the startup splash is fully drawn underneath
  // it — and again after dimming into the default screen, at the end of setup().
#ifdef GFX_BL
  ledcAttach(GFX_BL, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RESOLUTION);
  ledcWrite(GFX_BL, BACKLIGHT_OFF);
  currentBacklight = BACKLIGHT_OFF;
#endif
  lastActivityTime = millis();

  // --- Startup splash screen ---
  // Drawn immediately (backlight still off, so invisible), then faded in.
  // startupScreenShownAt anchors the hold time below regardless of how long
  // the layout/PWM setup that follows happens to take.
  drawStartupScreen();
  unsigned long startupScreenShownAt = millis();
  delay(STARTUP_BACKLIGHT_DELAY_MS);
  fadeBacklightIn();

  // --- Layout: Custom ---
  for (int i = 0; i < NUM_FANS; i++) {
    int gap = (i == 3) ? BAR_GAP_LARGE : BAR_GAP; // wider gap splits into left/right visual groups
    barX[i] = (i == 0) ? BAR_LEFT_MARGIN : barX[i - 1] + BAR_WIDTH + gap;
  }

  // --- Layout: Monotone ---
  monotoneBarX = (gfx->width() - BAR_WIDTH) / 2;

  const int monoGreyGroupWidth = 3 * MONO_GREY_WIDTH + 2 * MONO_GREY_GAP;
  int monoLeftGroupStart = monotoneBarX - MONO_CENTER_GAP - monoGreyGroupWidth;
  for (int i = 0; i < 3; i++) {
    monotoneGreyBarX[i] = monoLeftGroupStart + i * (MONO_GREY_WIDTH + MONO_GREY_GAP);
  }
  int monoRightGroupStart = monotoneBarX + BAR_WIDTH + MONO_CENTER_GAP;
  for (int i = 0; i < 3; i++) {
    monotoneGreyBarX[i + 3] = monoRightGroupStart + i * (MONO_GREY_WIDTH + MONO_GREY_GAP);
  }

  // --- Layout: Stereotone ---
  stereoRedBarX[0] = 32;
  stereoRedBarX[1] = gfx->width() - 32 - BAR_WIDTH;

  const int greyGroupWidth = 3 * STEREO_GREY_WIDTH + 2 * STEREO_GREY_GAP;
  const int totalGreyWidth = 2 * greyGroupWidth + STEREO_GREY_GROUP_GAP;
  const int greyStartX = (gfx->width() - totalGreyWidth) / 2;

  for (int i = 0; i < 3; i++) {
    stereoGreyBarX[i] = greyStartX + i * (STEREO_GREY_WIDTH + STEREO_GREY_GAP);
  }
  int secondGroupStart = greyStartX + greyGroupWidth + STEREO_GREY_GROUP_GAP;
  for (int i = 0; i < 3; i++) {
    stereoGreyBarX[i + 3] = secondGroupStart + i * (STEREO_GREY_WIDTH + STEREO_GREY_GAP);
  }

  // --- Layout: Ondulation (also feeds waveBarX[], shared with Natural Breeze) ---
  for (int i = 0; i < NUM_FANS; i++) {
    waveBarX[i] = WAVE_BAR_LEFT_MARGIN + i * (WAVE_BAR_WIDTH + WAVE_BAR_GAP);
  }

  controlX = gfx->width() - 16 - CONTROL_W;
  controlY = BAR_TOP;

  dotY = fmapf(DEFAULT_ONDULATION_MAX_DUTY_FRAC, 0.0f, 1.0f, CONTROL_H - DOT_RADIUS - 1, DOT_RADIUS);
  dotX = fmapf(DEFAULT_ONDULATION_DIRECTION, -1.0f, 1.0f, DOT_RADIUS, CONTROL_W - DOT_RADIUS - 1);
  clampDot();

  // --- Layout: Natural Breeze ---
  naturalBlueBarX = gfx->width() - 16 - BAR_WIDTH;
  naturalRedBarX = naturalBlueBarX - 32 - BAR_WIDTH;

  // -- Layout: Pulse ---
  pulseBlueBarX = gfx->width() - 16 - BAR_WIDTH;
  pulseRedBarX = pulseBlueBarX - 32 - BAR_WIDTH;

  for (int i = 0; i < NUM_FANS; i++) {
    pulseGreyBarX[i] = WAVE_BAR_LEFT_MARGIN + i * (WAVE_BAR_WIDTH + WAVE_BAR_GAP);
  }

  // --- Generic drag-controls: Monotone/Stereotone/Natural Breeze/Pulse ---
  // (Custom and Ondulation are hand-handled in loop(), see comment above Control struct.)

  numControls[SCREEN_MONOTONE] = 1;
  controls[SCREEN_MONOTONE][0] = { 0, gfx->width(), &monotoneDuty, &lastMonotoneDuty,
                                    "speed", COLOR_BAR_FILL, drawMonotoneAll, applyMonotone };

  numControls[SCREEN_STEREOTONE] = 2;
  {
    int leftGroupMin = min(stereoRedBarX[0], stereoGreyBarX[0]);
    int leftGroupMax = max(stereoRedBarX[0], stereoGreyBarX[2]) + BAR_WIDTH;
    int rightGroupMin = min(stereoRedBarX[1], stereoGreyBarX[3]);
    int rightGroupMax = max(stereoRedBarX[1], stereoGreyBarX[5]) + BAR_WIDTH;

    controls[SCREEN_STEREOTONE][0] = { leftGroupMin - PAD_TOUCH_MARGIN_OTHER, leftGroupMax + PAD_TOUCH_MARGIN_OTHER,
                                        &leftDuty, &lastLeftDuty, "left speed", COLOR_BAR_FILL, drawStereoLeft, applyStereoLeft };
    controls[SCREEN_STEREOTONE][1] = { rightGroupMin - PAD_TOUCH_MARGIN_OTHER, rightGroupMax + PAD_TOUCH_MARGIN_RIGHT,
                                        &rightDuty, &lastRightDuty, "right speed", COLOR_BAR_FILL, drawStereoRight, applyStereoRight };
  }

  numControls[SCREEN_NATURAL_BREEZE] = 2;
  {
    int midBoundary = (naturalRedBarX + BAR_WIDTH + naturalBlueBarX) / 2;
    // Control[0]'s left edge is 0, not just the red bar's own left edge: the
    // grey feedback bars to its left are display-only, so a vertical drag
    // anywhere out there is interpreted as dragging the red bar too, making
    // it much easier to hit than the narrow bar itself.
    controls[SCREEN_NATURAL_BREEZE][0] = { 0, midBoundary,
                                            &naturalMaxDuty, &lastNaturalMaxDuty, "peak gust", COLOR_BAR_FILL, drawNaturalRedBar, nullptr };
    controls[SCREEN_NATURAL_BREEZE][1] = { midBoundary, naturalBlueBarX + BAR_WIDTH + PAD_TOUCH_MARGIN_RIGHT,
                                            &naturalRateDuty, &lastNaturalRateDuty, "gustiness", COLOR_BLUE_BAR_FILL, drawNaturalBlueBar, nullptr };
  }

  numControls[SCREEN_PULSE] = 2;
  {
    int midBoundary = (pulseRedBarX + BAR_WIDTH + pulseBlueBarX) / 2;
    // Same reasoning as Natural Breeze above: control[0] absorbs the grey
    // feedback-bar area all the way to the left edge.
    controls[SCREEN_PULSE][0] = { 0, midBoundary,
                                   &pulseMaxDuty, &lastPulseMaxDuty, "peak pulse", COLOR_BAR_FILL, drawPulseRedBar, nullptr };
    controls[SCREEN_PULSE][1] = { midBoundary, pulseBlueBarX + BAR_WIDTH + PAD_TOUCH_MARGIN_RIGHT,
                                   &pulseSpeedDuty, &lastPulseSpeedDuty, "tempo", COLOR_YELLOW_BAR_FILL, drawPulseSpeedBar, nullptr };
  }

  initScreenTable();

  // --- PWM outputs ---
  for (int i = 0; i < NUM_FANS; i++) {
    ledcAttach(FAN_PINS[i], PWM_FREQ, PWM_RESOLUTION);
    ledcWrite(FAN_PINS[i], fanDuty[i]);
  }

  // --- Hold the splash for its target duration, then dim into the default screen ---
  unsigned long splashElapsed = millis() - startupScreenShownAt;
  if (splashElapsed < STARTUP_SCREEN_HOLD_MS) delay(STARTUP_SCREEN_HOLD_MS - splashElapsed);

  // Dim to black rather than wipe, since this is a one-shot boot transition,
  // not a swipe between the regular control screens.
  fadeBacklightOut();
  currentScreen = loadDefaultScreen(); // whichever screen was long-pressed as default; Natural Breeze if none yet
  onEnterScreen(currentScreen);
  redrawCurrentScreen();
  showTitle();
  fadeBacklightIn();
  lastActivityTime = millis();

  // --- Touch controller ---
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  bsp_touch_init(&Wire, TOUCH_RST, TOUCH_INT, gfx->getRotation(), gfx->width(), gfx->height());

  Serial.println("setup done");
}

// ================================================================
// LOOP
// ================================================================

void loop() {
  touch_data_t touch_data;
  bsp_touch_read();
  bool touched = bsp_touch_get_coordinates(&touch_data);

  if (touched) lastActivityTime = millis(); // any touch resets the idle-dim timer

  // ---- Per-screen continuous animation (runs regardless of touch) ----
  if (screens[currentScreen].update) {
    static unsigned long lastFrame = 0;
    unsigned long now = millis();
    if (now - lastFrame >= 40) {
      lastFrame = now;
      screens[currentScreen].update();
    }
  }

  // ---- Title auto-hide (skipped while guidance text is showing) ----
  if (!guidanceActive && titleVisible && millis() - titleShownAt >= TITLE_DURATION_MS) {
    hideTitle();
  }

  // ---- Message auto-hide (e.g. the long-press confirmation) ----
  if (messageActive && millis() - messageShownAt >= MESSAGE_DURATION_MS) {
    hideMessage();
  }

  // ---- Backlight idle dim/off ----
  unsigned long idleTime = millis() - lastActivityTime;
  if (idleTime >= IDLE_OFF_MS) setBacklight(BACKLIGHT_OFF);
  else if (idleTime >= IDLE_DIM_MS) setBacklight(BACKLIGHT_DIM);
  else setBacklight(BACKLIGHT_FULL);

  // ================================================================
  // TOUCH HANDLING
  //
  // Structure:
  //   if (touched)
  //     if (!touchWasDown)   -> FRESH TOUCH-DOWN: identify which zone/bar
  //                             was hit for this screen, remember it in
  //                             activeFan/activeControl. Do NOT show
  //                             guidance or commit to a gesture yet — we
  //                             don't know if this will turn into a swipe
  //                             (finger moves mostly sideways) or a control
  //                             drag (finger moves mostly up/down) until the
  //                             finger actually moves.
  //                             EXCEPTION: Ondulation's pad commits to
  //                             GESTURE_PAD_DRAG immediately, since it's a
  //                             2D control — see note below.
  //     else                 -> CONTINUING TOUCH (finger still down):
  //       if GESTURE_PAD_DRAG   -> update the Ondulation dot directly
  //       else
  //         if GESTURE_NONE      -> resolve the gesture once movement
  //                                 crosses GESTURE_THRESHOLD: horizontal
  //                                 movement wins -> GESTURE_SWIPE (navigate
  //                                 immediately); vertical movement on a
  //                                 screen with an active zone -> GESTURE_DRAG
  //                                 (and show that zone's guidance text).
  //                                 If GESTURE_THRESHOLD is never crossed and
  //                                 LONG_PRESS_MS elapses instead -> GESTURE_
  //                                 LONG_PRESS (see onLongPress(); works
  //                                 anywhere on screen, not tied to a zone —
  //                                 the pad exception above means it doesn't
  //                                 currently apply inside Ondulation's pad).
  //         if GESTURE_DRAG       -> apply the accumulated vertical delta
  //                                 to whichever duty variable is active
  //                                 for this screen, redraw only what changed
  //   else (not touched)
  //     if touchWasDown was true -> touch just ended: reset all "active"
  //                                 state and hide any guidance text
  //
  // Custom (activeFan) and the Control-table screens (activeControl) each
  // get one branch below wherever the structure above calls for
  // screen-specific behavior; Custom's is still hand-written (per-bar
  // zone lookup via getFanZoneForTouch), everything else goes through the
  // generic Control table.
  //
  // NOTE on Ondulation's pad: unlike every other screen, the pad is a
  // genuine 2D control — a horizontal drag inside it means "change wave
  // direction", not "swipe to another screen". So a touch starting inside
  // the pad's bounds can NEVER become a swipe; it always becomes
  // GESTURE_PAD_DRAG immediately at touch-down. This is intentional: the
  // pad area is simply not swipeable, by design. Swiping out of the
  // Ondulation screen must start from outside the pad (e.g. from the grey
  // feedback bars on the left).
  // ================================================================

  if (touched) {
    int tx = touch_data.coords[0].x;
    int ty = touch_data.coords[0].y;

    if (!touchWasDown) {
      // ---- FRESH TOUCH-DOWN: zone detection only, no gesture decided yet ----
      touchStartX = tx;
      touchStartY = ty;
      touchStartTime = millis();
      lastTouchY = ty;
      currentGesture = GESTURE_NONE;
      ondulationGreyTouch = false;
      padDragYOnly = false;

      if (currentScreen == SCREEN_CUSTOM) {
        activeFan = getFanZoneForTouch(tx);

      } else if (numControls[currentScreen] > 0) {
        // Generic zone lookup for Monotone/Stereotone/Natural Breeze/Pulse.
        activeControl = -1;
        for (int i = 0; i < numControls[currentScreen]; i++) {
          if (tx >= controls[currentScreen][i].xMin && tx < controls[currentScreen][i].xMax) {
            activeControl = i;
            break;
          }
        }

      } else if (currentScreen == SCREEN_ONDULATION &&
                 tx >= controlX &&
                 tx < controlX + CONTROL_W + PAD_TOUCH_MARGIN_RIGHT &&
                 ty >= controlY - PAD_TOUCH_MARGIN_OTHER &&
                 ty < controlY + CONTROL_H + PAD_TOUCH_MARGIN_OTHER) {
        // Ondulation's pad is a 2D control -> commits immediately, see note above.
        currentGesture = GESTURE_PAD_DRAG;
        lastTouchX = tx;
        lastTouchY = ty;
        showGuidance("wave height and direction", COLOR_CONTROL_DOT);
        drawControlPad(true); // trigger active-color redraw immediately

      } else if (currentScreen == SCREEN_ONDULATION && tx < controlX) {
        // Grey feedback-bar area, left of the pad: not committed yet (a
        // horizontal drag out here should still be able to swipe to another
        // screen) -- just remembered, and resolved to a Y-only pad drag
        // below once/if vertical movement wins past GESTURE_THRESHOLD.
        ondulationGreyTouch = true;
      }

    } else {
      // ---- CONTINUING TOUCH ----

      if (currentGesture == GESTURE_PAD_DRAG) {
        float prevDotX = dotX;
        float prevDotY = dotY;

        if (!padDragYOnly) dotX += tx - lastTouchX; // grey-zone drag: X (direction) stays untouched
        dotY += ty - lastTouchY;
        clampDot();

        if (dotX != prevDotX || dotY != prevDotY) {
          drawControlPad(true);
        }

        lastTouchX = tx;
        lastTouchY = ty;

      } else {
        int dx = tx - touchStartX;
        int dy = ty - touchStartY;

        // ---- Resolve swipe vs. drag, once movement crosses the threshold —
        // or, if it never does, resolve to a long press once enough time
        // has passed while still under the threshold. ----
        if (currentGesture == GESTURE_NONE) {
          if (abs(dx) > GESTURE_THRESHOLD || abs(dy) > GESTURE_THRESHOLD) {

            if (abs(dx) > abs(dy)) {
              // Dominant horizontal movement -> navigate, fire once
              currentGesture = GESTURE_SWIPE;
              goToScreen(dx < 0 ? +1 : -1);
              // activeFan/activeControl were set against the screen we just
              // navigated AWAY from, for the rest of this still-held touch
              // (a swipe doesn't release the finger). Left stale, they can
              // index into the NEW screen's controls[] with an index that
              // was never populated there -- e.g. touching Stereotone's
              // right zone (activeControl=1) then swiping to Monotone
              // (only controls[SCREEN_MONOTONE][0] is initialized) leaves
              // controls[SCREEN_MONOTONE][1].redraw as a null function
              // pointer, crashed into on release. Invalidate both now.
              activeFan = -1;
              activeControl = -1;

            } else if (currentScreen == SCREEN_CUSTOM && activeFan != -1) {
              currentGesture = GESTURE_DRAG;
              showGuidance("speed", COLOR_BAR_FILL);
              drawBar(activeFan, true);

            } else if (numControls[currentScreen] > 0 && activeControl != -1 && activeControl < numControls[currentScreen]) {
              currentGesture = GESTURE_DRAG;
              Control &c = controls[currentScreen][activeControl];
              showGuidance(c.guidance, c.guidanceColor);
              c.redraw(true);

            } else if (currentScreen == SCREEN_ONDULATION && ondulationGreyTouch) {
              // Vertical drag from the grey zone: move the pad's dot up/down
              // only -- see padDragYOnly in the GESTURE_PAD_DRAG handler above.
              currentGesture = GESTURE_PAD_DRAG;
              padDragYOnly = true;
              showGuidance("wave height", COLOR_CONTROL_DOT);
              drawControlPad(true);
            }
            // Note: if none of the above match (e.g. touch-down missed every
            // zone), currentGesture stays GESTURE_NONE and nothing happens —
            // correctly ignoring drags that started outside any control.

          } else if (millis() - touchStartTime >= LONG_PRESS_MS) {
            // Held roughly in place (never crossed GESTURE_THRESHOLD) for
            // LONG_PRESS_MS -> long press, registered anywhere on screen.
            currentGesture = GESTURE_LONG_PRESS;
            onLongPress();
          }
        }

        // ---- Apply an in-progress vertical drag to whichever control is active ----
        if (currentGesture == GESTURE_DRAG) {
          int deltaY = lastTouchY - ty;
          int deltaDuty = (int)(deltaY * dragSensitivity);

          if (deltaDuty != 0) {

            if (currentScreen == SCREEN_CUSTOM && activeFan != -1) {
              fanDuty[activeFan] = constrain(fanDuty[activeFan] + deltaDuty, 0, 255);
              if (fanDuty[activeFan] != lastFanDuty[activeFan]) {
                lastFanDuty[activeFan] = fanDuty[activeFan];
                ledcWrite(FAN_PINS[activeFan], fanDuty[activeFan]);
                drawBar(activeFan, true);
              }
              lastTouchY = ty;

            } else if (numControls[currentScreen] > 0 && activeControl != -1 && activeControl < numControls[currentScreen]) {
              Control &c = controls[currentScreen][activeControl];
              *c.duty = constrain(*c.duty + deltaDuty, 0, 255);
              if (*c.duty != *c.lastDuty) {
                *c.lastDuty = *c.duty;
                c.redraw(true);
                if (c.apply) c.apply(*c.duty);
              }
              lastTouchY = ty;
            }
          }
        }
      }
    }

  } else {
    // ---- RELEASE: fires exactly once, on the frame touch is lifted ----
    if (touchWasDown) {
      // Restore normal (non-active) bar colors before clearing which one was active
      if (currentScreen == SCREEN_CUSTOM && activeFan != -1) {
        drawBar(activeFan, false);
      } else if (numControls[currentScreen] > 0 && activeControl != -1 && activeControl < numControls[currentScreen]) {
        controls[currentScreen][activeControl].redraw(false);
      } else if (currentGesture == GESTURE_PAD_DRAG) {
        drawControlPad(false);
      }

      activeFan = -1;
      activeControl = -1;
      currentGesture = GESTURE_NONE;
      ondulationGreyTouch = false;
      padDragYOnly = false;
      if (guidanceActive) hideGuidance();
    }
  }

  touchWasDown = touched;
  delay(20);
}
