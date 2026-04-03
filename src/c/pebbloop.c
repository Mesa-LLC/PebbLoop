/**
 * PebbLoop - A Zuma-style game for Pebble Basalt & Emery
 *
 * Controls:
 *   UP      - Rotate shooter counter-clockwise
 *   DOWN    - Rotate shooter clockwise
 *   SELECT  - Shoot ball / Restart
 *   BACK    - Exit game
 *   FLICK   - Swap loaded ball with next (wrist flick / accelerometer tap)
 */

#include <pebble.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

// Base pixel constants are defined for Basalt (144×168).
// All are scaled at runtime via SC() using s_scale.
#define BASE_BALL_R        5
#define BASE_BALL_D        10   // center-to-center arc distance (touching balls)
#define BASE_EXPLOSION_R   80
#define BASE_COIN_R        6
#define BASE_BARREL_LEN    14   // shooter barrel length in pixels
#define BASE_SHOOTER_R1    11   // outer shooter body radius
#define BASE_SHOOTER_R2    8    // inner shooter body radius
#define BASE_EYE_OFFSET_X  4    // eye horizontal offset from shooter center
#define BASE_EYE_OFFSET_Y  5    // eye vertical offset from shooter center
#define BASE_EYE_R_OUT     3    // outer eye radius
#define BASE_EYE_R_IN      1    // pupil radius
#define BASE_HIGHLIGHT_OFF 2    // ball highlight offset
#define BASE_BLACKHOLE_R_N 3    // black hole radius numerator factor (BALL_R*2*3/4)
#define BASE_BLACKHOLE_R_D 4

#define MAX_CHAIN       80
#define MAX_PROJS       4
#define NUM_COLORS      4
#define PATH_LEN        220
#define TOTAL_BALLS     48
#define TICK_MS         50
#define CHAIN_STEP_FP   50    // chain speed (normal), in arc fp units/tick
#define PROJ_SPEED_FP   1536  // ~6 px/tick at Basalt scale
#define ROTATE_DEG      8
#define PERSIST_KEY_HS  1     // persistent storage key for highscore

// Power-up spawn odds: 1-in-N chance per spawned ball (sparse)
#define POWERUP_ODDS    8

// Power-up effect durations in ticks (50ms each)
#define PU_BACKWARD_TICKS  120  // 6 seconds
#define PU_SLOWDOWN_TICKS  120  // 6 seconds
#define PU_ACCURACY_TICKS  240  // 12 seconds

// Backwards power-up: chain retreat speed (same fp units as CHAIN_STEP_FP)
#define BACKWARD_STEP_FP   50

// ── Scale system ──────────────────────────────────────────────────────────────
// s_scale is set at startup: 256 for Basalt (144 wide), ~356 for Emery (200 wide).
// SC(x) scales a Basalt-baseline pixel value to the current platform.
// All pixel geometry goes through SC() so the game looks identical on both watches.

static int32_t s_scale;    // fixed-point scale: (s_sw * 256) / 144
static bool    s_is_emery; // true when running on Emery (200×228)

#define SC(x)  ((int32_t)(x) * s_scale >> 8)

// Scaled runtime geometry values (computed once in win_load after s_scale is set)
static int s_ball_r;       // scaled ball radius
static int s_ball_d;       // scaled ball diameter (chain spacing)
static int s_explosion_r;  // scaled explosion radius
static int s_coin_r;       // scaled coin radius
static int s_barrel_len;   // scaled barrel length
static GPoint s_coin_spots[2];  // scaled coin spot positions (computed from screen size)

// ── Types ─────────────────────────────────────────────────────────────────────

typedef uint8_t BallColor;
#define COL_RED    0
#define COL_GREEN  1
#define COL_BLUE   2
#define COL_YELLOW 3

// Power-up types stored inside ChainBall.powerup
#define PU_NONE      0
#define PU_BACKWARD  1   // arrow symbol  — reverses chain
#define PU_SLOWDOWN  2   // pause symbol  — half speed
#define PU_ACCURACY  3   // target symbol — aim line
#define PU_EXPLOSION 4   // dot symbol    — radius clear

typedef struct {
  int32_t  pos_fp;
  BallColor color;
  uint8_t  powerup;   // PU_NONE or one of the PU_* values
} ChainBall;

typedef struct {
  int32_t x_fp, y_fp, dx_fp, dy_fp;
  BallColor color;
  uint8_t  powerup;
  bool active;
  bool pass_through;  // if true, flies off-screen without inserting into chain
} Proj;

typedef enum { ST_PLAY, ST_LOSE, ST_PAUSE, ST_SLIDE } State;

// ── Globals ───────────────────────────────────────────────────────────────────

static Window   *s_win;
static Layer    *s_layer;
static AppTimer *s_timer;
static int16_t   s_sw, s_sh;
static int16_t   s_scx;   // shooter center x
static GPoint    s_path[PATH_LEN];
static ChainBall s_chain[MAX_CHAIN];
static int       s_chain_len;
static Proj      s_projs[MAX_PROJS];
static int32_t   s_angle;
static BallColor s_loaded, s_queued;
static uint8_t   s_loaded_pu, s_queued_pu;   // power-up type of loaded/queued ball
static State     s_state;
static int       s_score;
static int       s_highscore;

static int32_t   s_spawn_accum_fp;

// ── Active power-up effect state ──────────────────────────────────────────────
static int  s_pu_backward_ttl;   // ticks remaining for backwards effect
static int  s_pu_slowdown_ttl;   // ticks remaining for slowdown effect
static int  s_pu_accuracy_ttl;   // ticks remaining for accuracy line

// ── Score popup ───────────────────────────────────────────────────────────────
#define POPUP_TTL_MAX  13
#define POPUP_FADE      2
#define POPUP_W        34
#define POPUP_H        22
typedef struct {
  int16_t   x, y;
  int       points;
  BallColor color;
  int       ttl;
} ScorePopup;
#define MAX_POPUPS 4
static ScorePopup s_popups[MAX_POPUPS];

// ── Coin system ───────────────────────────────────────────────────────────────
#define COIN_TTL_MAX   120
#define COIN_AWARD     50

typedef struct {
  bool    active;
  int     spot;
  int     ttl;
} Coin;
static Coin  s_coin;
static int   s_score_milestone;

// ── Slide-back animation state ────────────────────────────────────────────────
static int       s_slide_lo;
static int32_t   s_slide_target_fp;
static int       s_slide_tail_idx;
static int       s_cascade_level;
#define SLIDE_STEP_FP  480

// ── Back-button pause state ───────────────────────────────────────────────────
static bool s_back_pressed_once;

// ── Trig ──────────────────────────────────────────────────────────────────────

static int32_t i_sin(int32_t deg) {
  return sin_lookup(DEG_TO_TRIGANGLE(((deg % 360) + 360) % 360)) * 1024 / TRIG_MAX_RATIO;
}
static int32_t i_cos(int32_t deg) {
  return cos_lookup(DEG_TO_TRIGANGLE(((deg % 360) + 360) % 360)) * 1024 / TRIG_MAX_RATIO;
}

// ── Path ──────────────────────────────────────────────────────────────────────

static int32_t s_arc[PATH_LEN];

static void build_path(void) {
  int16_t cx = s_sw / 2;
  int16_t cy = s_sh / 2;
  // All geometry offsets scaled from Basalt baseline so the spiral fills the
  // screen proportionally on both Basalt and Emery.
  int16_t path_cx = (int16_t)(cx - SC(9));
  int16_t end_cy  = (int16_t)(cy - SC(5));
  int32_t R_start = cy;
  int32_t R_end   = SC(16);
  int32_t start_deg = 270;
  int32_t total_deg = 720;

  for (int i = 0; i < PATH_LEN; i++) {
    int32_t t         = (int32_t)i * 1024 / (PATH_LEN - 1);
    int32_t theta_deg = start_deg + (total_deg * t / 1024);
    int32_t r         = R_start - ((R_start - R_end) * t / 1024);
    int16_t spiral_cy = (int16_t)(cy + ((end_cy - cy) * t / 1024));
    s_path[i].x = (int16_t)(path_cx + r * i_cos(theta_deg) / 1024);
    s_path[i].y = (int16_t)(spiral_cy + r * i_sin(theta_deg) / 1024);
  }
  s_path[PATH_LEN - 1].x = (int16_t)(cx - SC(2));
  s_path[PATH_LEN - 1].y = (int16_t)(cy - SC(23));

  s_arc[0] = 0;
  for (int i = 1; i < PATH_LEN; i++) {
    int dx = s_path[i].x - s_path[i - 1].x;
    int dy = s_path[i].y - s_path[i - 1].y;
    int32_t d2 = dx * dx + dy * dy;
    int32_t seg = 0;
    if (d2 > 0) {
      int32_t x = d2, s = 1;
      while (s * s < x) s++;
      seg = (s + x / s) / 2;
    }
    s_arc[i] = s_arc[i - 1] + (seg << 8);
  }
}

static int32_t arc_total(void) { return s_arc[PATH_LEN - 1]; }

static GPoint path_at_arc(int32_t arc_fp) {
  if (arc_fp <= 0) return s_path[0];
  if (arc_fp >= s_arc[PATH_LEN - 1]) return s_path[PATH_LEN - 1];
  int lo = 0, hi = PATH_LEN - 1;
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (s_arc[mid] <= arc_fp) lo = mid; else hi = mid;
  }
  int32_t seg_len = s_arc[hi] - s_arc[lo];
  if (seg_len == 0) return s_path[lo];
  int32_t frac = (arc_fp - s_arc[lo]) * 256 / seg_len;
  GPoint a = s_path[lo], b = s_path[hi];
  GPoint out;
  out.x = (int16_t)(a.x + ((b.x - a.x) * frac >> 8));
  out.y = (int16_t)(a.y + ((b.y - a.y) * frac >> 8));
  return out;
}

// ── Chain ─────────────────────────────────────────────────────────────────────

static void init_chain(void) {
  s_chain_len      = 0;
  s_spawn_accum_fp = 0;
}

static void compact_positions(int around) {
  int32_t d = (int32_t)s_ball_d << 8;
  for (int i = around + 1; i < s_chain_len; i++) {
    int32_t min_pos = s_chain[i - 1].pos_fp + d;
    if (s_chain[i].pos_fp < min_pos) s_chain[i].pos_fp = min_pos;
    else break;
  }
  for (int i = around - 1; i >= 0; i--) {
    int32_t max_pos = s_chain[i + 1].pos_fp - d;
    if (s_chain[i].pos_fp > max_pos) s_chain[i].pos_fp = max_pos;
    else break;
  }
}

static void spawn_popup(int lo, int hi, int points, BallColor bc) {
  int mid = (lo + hi) / 2;
  int32_t mid_pos = (mid < s_chain_len) ? s_chain[mid].pos_fp
                  : (lo < s_chain_len)  ? s_chain[lo].pos_fp : 0;
  GPoint p = path_at_arc(mid_pos);
  int16_t px = p.x;
  int16_t py = (int16_t)(p.y - s_ball_r - SC(12));
  int16_t half_w = POPUP_W / 2;
  if (px - half_w < 2)        px = 2 + half_w;
  if (px + half_w > s_sw - 2) px = s_sw - 2 - half_w;
  if (py < 2)                  py = 2;
  if (py + POPUP_H > s_sh - 2) py = s_sh - 2 - POPUP_H;
  int slot = 0;
  for (int i = 0; i < MAX_POPUPS; i++) {
    if (s_popups[i].ttl == 0) { slot = i; break; }
    if (s_popups[i].ttl < s_popups[slot].ttl) slot = i;
  }
  s_popups[slot] = (ScorePopup){ px, py, points, bc, POPUP_TTL_MAX };
}

// ── Power-up activation ───────────────────────────────────────────────────────

static void activate_backward(void) {
  s_pu_backward_ttl = PU_BACKWARD_TICKS;
}

static void activate_slowdown(void) {
  s_pu_slowdown_ttl = PU_SLOWDOWN_TICKS;
}

static void activate_accuracy(void) {
  s_pu_accuracy_ttl = PU_ACCURACY_TICKS;
}

// Explosion: remove all chain balls within s_explosion_r pixels of the given screen point.
static void activate_explosion(GPoint center) {
  int r2 = s_explosion_r * s_explosion_r;
  int i = 0;
  while (i < s_chain_len) {
    GPoint bp = path_at_arc(s_chain[i].pos_fp);
    int dx = bp.x - center.x, dy = bp.y - center.y;
    if (dx * dx + dy * dy <= r2) {
      for (int j = i; j < s_chain_len - 1; j++) s_chain[j] = s_chain[j + 1];
      s_chain_len--;
    } else {
      i++;
    }
  }
  // Double vibration: two firm pulses with a clear gap
  static const uint32_t explosion_vibe_segments[] = { 200, 150, 200 };
  VibePattern explosion_vibe = { .durations = explosion_vibe_segments, .num_segments = 3 };
  vibes_enqueue_custom_pattern(explosion_vibe);
}

// ── Match / cascade ───────────────────────────────────────────────────────────

static void try_cascade_at(int lo, int tail_idx, int cascade_level);
static void fire_cascade_at(int lo, int tail_idx, int cascade_level);

// Returns number of balls popped, and activates any power-up on a successful triad+.
static int check_matches_at(int around) {
  if (s_chain_len < 3) return 0;

  BallColor c = s_chain[around].color;

  int lo = around;
  while (lo > 0 && s_chain[lo - 1].color == c) lo--;
  int hi = around;
  while (hi < s_chain_len - 1 && s_chain[hi + 1].color == c) hi++;

  int run = hi - lo + 1;
  if (run < 3) return 0;

  int tail_idx = lo - 1;

  // Check if any ball in the run is a power-up; collect the first one found.
  uint8_t triggered_pu = PU_NONE;
  GPoint  pu_center    = { 0, 0 };
  for (int k = lo; k <= hi; k++) {
    if (s_chain[k].powerup != PU_NONE) {
      triggered_pu = s_chain[k].powerup;
      pu_center    = path_at_arc(s_chain[k].pos_fp);
      break;
    }
  }

  int pts = run * 10;
  s_score += pts;
  spawn_popup(lo, hi, pts, c);
  for (int j = lo; j < s_chain_len - run; j++) s_chain[j] = s_chain[j + run];
  s_chain_len -= run;

  // Activate power-up effect after removal (indices are now stable)
  if (triggered_pu != PU_NONE) {
    switch (triggered_pu) {
      case PU_BACKWARD:  activate_backward();          break;
      case PU_SLOWDOWN:  activate_slowdown();           break;
      case PU_ACCURACY:  activate_accuracy();           break;
      case PU_EXPLOSION: activate_explosion(pu_center); break;
    }
    vibes_short_pulse();
  }

  s_cascade_level = 0;
  try_cascade_at(lo, tail_idx, 1);

  return run;
}

static void try_cascade_at(int lo, int tail_idx, int cascade_level) {
  if (tail_idx < 0 || lo >= s_chain_len) return;
  if (s_chain[tail_idx].color != s_chain[lo].color) return;

  int32_t d_fp = (int32_t)s_ball_d << 8;
  int32_t gap = s_chain[lo].pos_fp - (s_chain[tail_idx].pos_fp + d_fp);

  if (gap > 0) {
    s_slide_lo        = lo;
    s_slide_tail_idx  = tail_idx;
    s_slide_target_fp = s_chain[tail_idx].pos_fp + d_fp;
    s_cascade_level   = cascade_level;
    s_state           = ST_SLIDE;
  } else {
    fire_cascade_at(lo, tail_idx, cascade_level);
  }
}

static void fire_cascade_at(int lo, int tail_idx, int cascade_level) {
  BallColor cc = s_chain[tail_idx].color;
  int clo = lo;
  while (clo > 0 && s_chain[clo - 1].color == cc) clo--;
  int chi = lo;
  while (chi < s_chain_len - 1 && s_chain[chi + 1].color == cc) chi++;
  int crun = chi - clo + 1;
  if (crun >= 3) {
    // Collect power-up from cascade run too
    uint8_t triggered_pu = PU_NONE;
    GPoint  pu_center    = { 0, 0 };
    for (int k = clo; k <= chi; k++) {
      if (s_chain[k].powerup != PU_NONE) {
        triggered_pu = s_chain[k].powerup;
        pu_center    = path_at_arc(s_chain[k].pos_fp);
        break;
      }
    }
    int multiplier = cascade_level + 1;
    int pts = crun * 10 * multiplier;
    s_score += pts;
    spawn_popup(clo, chi, pts, cc);
    for (int j = clo; j < s_chain_len - crun; j++) s_chain[j] = s_chain[j + crun];
    s_chain_len -= crun;
    vibes_short_pulse();
    if (triggered_pu != PU_NONE) {
      switch (triggered_pu) {
        case PU_BACKWARD:  activate_backward();          break;
        case PU_SLOWDOWN:  activate_slowdown();           break;
        case PU_ACCURACY:  activate_accuracy();           break;
        case PU_EXPLOSION: activate_explosion(pu_center); break;
      }
    }
    int new_tail = clo - 1;
    try_cascade_at(clo, new_tail, cascade_level + 1);
  }
}

// ── Drawing ───────────────────────────────────────────────────────────────────

// Draw the base ball layers (outer dark rim + inner color + highlight)
static void draw_ball_base(GContext *ctx, GPoint p, BallColor c) {
  static const GColor mains[] = {
    {GColorRedARGB8}, {GColorIslamicGreenARGB8}, {GColorBlueARGB8}, {GColorChromeYellowARGB8}
  };
  static const GColor darks[] = {
    {GColorDarkCandyAppleRedARGB8}, {GColorDarkGreenARGB8},
    {GColorOxfordBlueARGB8}, {GColorWindsorTanARGB8}
  };
  int ci = c < NUM_COLORS ? c : 0;
  int highlight_off = SC(BASE_HIGHLIGHT_OFF);
  graphics_context_set_fill_color(ctx, (GColor){ .argb = darks[ci].argb });
  graphics_fill_circle(ctx, p, s_ball_r);
  graphics_context_set_fill_color(ctx, (GColor){ .argb = mains[ci].argb });
  graphics_fill_circle(ctx, p, s_ball_r - 1);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(p.x - highlight_off, p.y - highlight_off), 1);
}

// Draw power-up symbol on top of ball (small black icon, fits within ball_r-1)
static void draw_powerup_symbol(GContext *ctx, GPoint p, uint8_t pu) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx, GColorBlack);
  // Icon offsets scale with the ball so they always fit inside it.
  int s2 = SC(2), s3 = SC(3);
  // Ensure minimum 1px for tiny offsets
  if (s2 < 1) s2 = 1;
  if (s3 < 1) s3 = 1;
  switch (pu) {
    case PU_BACKWARD:
      // Small backwards arrow (pointing left), within ±s3 px
      graphics_draw_line(ctx, GPoint(p.x - s3, p.y),     GPoint(p.x + s2, p.y));
      graphics_draw_line(ctx, GPoint(p.x - s3, p.y),     GPoint(p.x - s2 + 1, p.y - s2));
      graphics_draw_line(ctx, GPoint(p.x - s3, p.y),     GPoint(p.x - s2 + 1, p.y + s2));
      break;
    case PU_SLOWDOWN:
      // Pause symbol: two vertical bars
      graphics_draw_line(ctx, GPoint(p.x - s2, p.y - s2), GPoint(p.x - s2, p.y + s2));
      graphics_draw_line(ctx, GPoint(p.x + 1,  p.y - s2), GPoint(p.x + 1,  p.y + s2));
      break;
    case PU_ACCURACY:
      // Target: small cross with a circle
      graphics_draw_circle(ctx, p, s2);
      graphics_draw_line(ctx, GPoint(p.x - s3, p.y),     GPoint(p.x + s3, p.y));
      graphics_draw_line(ctx, GPoint(p.x,     p.y - s3), GPoint(p.x,     p.y + s3));
      break;
    case PU_EXPLOSION:
      // Filled dot in center
      graphics_fill_circle(ctx, p, s2);
      break;
  }
}

static void draw_chain_ball(GContext *ctx, GPoint p, BallColor c, uint8_t pu) {
  draw_ball_base(ctx, p, c);
  if (pu != PU_NONE) draw_powerup_symbol(ctx, p, pu);
}

static void draw_path_guide(GContext *ctx) {
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int i = 1; i < PATH_LEN; i++) {
    GPoint a = s_path[i - 1], b = s_path[i];
    if ((a.x >= -5 && a.x <= s_sw + 5 && a.y >= -5 && a.y <= s_sh + 5) ||
        (b.x >= -5 && b.x <= s_sw + 5 && b.y >= -5 && b.y <= s_sh + 5)) {
      graphics_draw_line(ctx, a, b);
    }
  }
}

static void draw_black_hole(GContext *ctx, GPoint center) {
  // r = ball_r * 2 * 3 / 4
  int r = (s_ball_r * 2 * BASE_BLACKHOLE_R_N) / BASE_BLACKHOLE_R_D;
  graphics_context_set_stroke_color(ctx, GColorOxfordBlue);
  graphics_draw_circle(ctx, center, r + 1);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, center, r);
}

static void draw_coin(GContext *ctx, GPoint p) {
  int cr = s_coin_r;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(p.x + 1, p.y + 1), cr);
  graphics_context_set_fill_color(ctx, GColorWindsorTan);
  graphics_fill_circle(ctx, p, cr);
  graphics_context_set_fill_color(ctx, GColorChromeYellow);
  graphics_fill_circle(ctx, p, cr - 1);
  graphics_context_set_fill_color(ctx, GColorIcterine);
  graphics_fill_circle(ctx, GPoint(p.x - 1, p.y - 1), cr - 3 > 1 ? cr - 3 : 1);
  graphics_context_set_stroke_color(ctx, GColorWindsorTan);
  graphics_draw_line(ctx, GPoint(p.x, p.y - SC(2)), GPoint(p.x, p.y + SC(2)));
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(p.x - SC(2), p.y - SC(2)), 1);
}

static void draw_popups(GContext *ctx) {
  static const GColor full_colors[] = {
    {GColorRedARGB8}, {GColorIslamicGreenARGB8}, {GColorBlueARGB8}, {GColorChromeYellowARGB8},
  };
  static const GColor dim_colors[] = {
    {GColorDarkCandyAppleRedARGB8}, {GColorDarkGreenARGB8},
    {GColorOxfordBlueARGB8}, {GColorWindsorTanARGB8},
  };
  static const GColor vdim_colors[] = {
    {GColorBulgarianRoseARGB8}, {GColorDarkGreenARGB8},
    {GColorMidnightGreenARGB8}, {GColorCadetBlueARGB8},
  };
  char buf[8];
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  for (int i = 0; i < MAX_POPUPS; i++) {
    if (s_popups[i].ttl <= 0) continue;
    int elapsed   = POPUP_TTL_MAX - s_popups[i].ttl;
    int remaining = s_popups[i].ttl;
    int ci = s_popups[i].color < NUM_COLORS ? s_popups[i].color : 0;
    GColor text_col;
    if (elapsed == 0 || remaining == 1)                           text_col = vdim_colors[ci];
    else if (elapsed < POPUP_FADE || remaining <= POPUP_FADE)     text_col = dim_colors[ci];
    else                                                           text_col = full_colors[ci];
    snprintf(buf, sizeof(buf), "+%d", s_popups[i].points);
    int16_t tx = s_popups[i].x - POPUP_W / 2, ty = s_popups[i].y;
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, buf, font, GRect(tx+1, ty+1, POPUP_W, POPUP_H),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_context_set_text_color(ctx, text_col);
    graphics_draw_text(ctx, buf, font, GRect(tx, ty, POPUP_W, POPUP_H),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

// Draw the accuracy aim line.
static void draw_accuracy_line(GContext *ctx) {
  if (s_pu_accuracy_ttl <= 0) return;

  static const GColor line_colors[] = {
    {GColorRedARGB8}, {GColorIslamicGreenARGB8}, {GColorBlueARGB8}, {GColorChromeYellowARGB8}
  };
  int ci = s_loaded < NUM_COLORS ? s_loaded : 0;
  graphics_context_set_stroke_color(ctx, (GColor){ .argb = line_colors[ci].argb });

  // Shooter center and barrel tip
  int16_t cx = s_scx, cy = (int16_t)(s_sh / 2);
  int16_t bx = (int16_t)(cx + s_barrel_len * i_cos(s_angle) / 1024);
  int16_t by = (int16_t)(cy + s_barrel_len * i_sin(s_angle) / 1024);

  int32_t rdx = i_cos(s_angle);
  int32_t rdy = i_sin(s_angle);

  int64_t mag2 = (int64_t)rdx * rdx + (int64_t)rdy * rdy;
  int64_t hit_r2_mag2 = (int64_t)(s_ball_r * 2) * (s_ball_r * 2) * mag2;

  int     best_i = -1;
  int64_t best_t = INT64_MAX;

  for (int i = 0; i < s_chain_len; i++) {
    GPoint bp = path_at_arc(s_chain[i].pos_fp);
    int64_t ex = (int64_t)(bp.x - bx);
    int64_t ey = (int64_t)(bp.y - by);
    int64_t t = ex * (int64_t)rdx + ey * (int64_t)rdy;
    if (t < 0) continue;
    int64_t e2         = ex * ex + ey * ey;
    int64_t perp2_mag2 = e2 * mag2 - t * t;
    if (perp2_mag2 <= hit_r2_mag2 && t < best_t) {
      best_t = t;
      best_i = i;
    }
  }

  int16_t end_x, end_y;
  if (best_i >= 0) {
    GPoint bp = path_at_arc(s_chain[best_i].pos_fp);
    end_x = bp.x;
    end_y = bp.y;
  } else {
    end_x = bx; end_y = by;
    for (int step = 1; step <= 300; step++) {
      int16_t sx = (int16_t)(bx + (int32_t)step * rdx / 1024);
      int16_t sy = (int16_t)(by + (int32_t)step * rdy / 1024);
      if (sx < 0 || sx >= s_sw || sy < 0 || sy >= s_sh) break;
      end_x = sx; end_y = sy;
    }
  }

  int32_t total_dx = end_x - bx, total_dy = end_y - by;
  int32_t len2 = total_dx * total_dx + total_dy * total_dy;
  int32_t len_px = 0;
  if (len2 > 0) {
    int32_t s = 1;
    while (s * s < len2) s++;
    len_px = (s + len2 / s) / 2;
  }
  if (len_px == 0) return;

  int32_t sx_fp = (total_dx << 8) / len_px;
  int32_t sy_fp = (total_dy << 8) / len_px;

#define DASH_ON   4
#define DASH_OFF  3
#define DASH_CYCLE (DASH_ON + DASH_OFF)

  for (int pix = 0; pix < len_px; pix++) {
    if ((pix % DASH_CYCLE) >= DASH_ON) continue;
    int16_t px = (int16_t)(bx + (sx_fp * pix >> 8));
    int16_t py = (int16_t)(by + (sy_fp * pix >> 8));
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_pixel(ctx, GPoint(px + 1, py + 1));
    graphics_context_set_stroke_color(ctx, (GColor){ .argb = line_colors[ci].argb });
    graphics_draw_pixel(ctx, GPoint(px, py));
  }
}

// Draw HUD indicators for active power-ups (small icons at bottom of screen)
static void draw_powerup_hud(GContext *ctx) {
  int x = 3, y = s_sh - 12;
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, GColorWhite);

  if (s_pu_backward_ttl > 0) {
    graphics_draw_line(ctx, GPoint(x,     y + 3), GPoint(x + 5, y + 3));
    graphics_draw_line(ctx, GPoint(x,     y + 3), GPoint(x + 2, y + 1));
    graphics_draw_line(ctx, GPoint(x,     y + 3), GPoint(x + 2, y + 5));
    x += 9;
  }
  if (s_pu_slowdown_ttl > 0) {
    graphics_draw_line(ctx, GPoint(x,     y + 1), GPoint(x,     y + 5));
    graphics_draw_line(ctx, GPoint(x + 3, y + 1), GPoint(x + 3, y + 5));
    x += 9;
  }
  if (s_pu_accuracy_ttl > 0) {
    graphics_draw_circle(ctx, GPoint(x + 3, y + 3), 3);
    graphics_fill_circle(ctx, GPoint(x + 3, y + 3), 1);
    x += 9;
  }
  // Coin countdown: show seconds remaining in the same HUD style
  // Emery: GOTHIC_14 (~1.5× GOTHIC_09), taller rect
  if (s_coin.active && s_coin.ttl > 0) {
    int secs = (s_coin.ttl + 19) / 20;
    char cbuf[4];
    snprintf(cbuf, sizeof(cbuf), "%d", secs);
    if (s_is_emery) {
      // Align text center with icon center (s_sh - 9).
      // GOTHIC_14_BOLD visual center ~7px below rect top → rect top = y - 4
      graphics_context_set_text_color(ctx, GColorBlack);
      graphics_draw_text(ctx, cbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(x + 1, y - 3, 18, 18), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      graphics_context_set_text_color(ctx, GColorChromeYellow);
      graphics_draw_text(ctx, cbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(x, y - 4, 18, 18), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    } else {
      graphics_context_set_text_color(ctx, GColorBlack);
      graphics_draw_text(ctx, cbuf, fonts_get_system_font(FONT_KEY_GOTHIC_09),
        GRect(x + 1, y - 1, 12, 12), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      graphics_context_set_text_color(ctx, GColorChromeYellow);
      graphics_draw_text(ctx, cbuf, fonts_get_system_font(FONT_KEY_GOTHIC_09),
        GRect(x, y - 2, 12, 12), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    }
    x += 9;
  }
  (void)x;
}

static void draw_scene(Layer *layer, GContext *ctx) {
  int16_t cx = s_scx, cy = s_sh / 2;

  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(0, 0, s_sw, s_sh), 0, GCornerNone);

  draw_path_guide(ctx);
  draw_black_hole(ctx, s_path[PATH_LEN - 1]);

  // Chain (with power-up symbols)
  for (int i = 0; i < s_chain_len; i++) {
    draw_chain_ball(ctx, path_at_arc(s_chain[i].pos_fp), s_chain[i].color, s_chain[i].powerup);
  }

  if (s_coin.active && s_coin.ttl > 0) draw_coin(ctx, s_coin_spots[s_coin.spot]);

  // Accuracy aim line (drawn before popups so popups render on top)
  draw_accuracy_line(ctx);

  draw_popups(ctx);

  // Projectiles
  for (int i = 0; i < MAX_PROJS; i++) {
    if (!s_projs[i].active) continue;
    GPoint pp = { (int16_t)(s_projs[i].x_fp >> 8), (int16_t)(s_projs[i].y_fp >> 8) };
    draw_ball_base(ctx, pp, s_projs[i].color);
    if (s_projs[i].powerup != PU_NONE) draw_powerup_symbol(ctx, pp, s_projs[i].powerup);
  }

  // Shooter body
  int r1 = SC(BASE_SHOOTER_R1), r2 = SC(BASE_SHOOTER_R2);
  int ex = SC(BASE_EYE_OFFSET_X), ey = SC(BASE_EYE_OFFSET_Y);
  int er_out = SC(BASE_EYE_R_OUT), er_in = SC(BASE_EYE_R_IN);
  if (er_out < 1) er_out = 1;
  if (er_in  < 1) er_in  = 1;
  graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  graphics_fill_circle(ctx, GPoint(cx, cy), r1);
  graphics_context_set_fill_color(ctx, GColorDarkGreen);
  graphics_fill_circle(ctx, GPoint(cx, cy), r2);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(cx - ex, cy - ey), er_out);
  graphics_fill_circle(ctx, GPoint(cx + ex, cy - ey), er_out);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(cx - ex, cy - ey), er_in);
  graphics_fill_circle(ctx, GPoint(cx + ex, cy - ey), er_in);

  // Queued ball in shooter center
  draw_chain_ball(ctx, GPoint(cx, cy), s_queued, s_queued_pu);

  // Barrel
  int16_t bx = (int16_t)(cx + s_barrel_len * i_cos(s_angle) / 1024);
  int16_t by = (int16_t)(cy + s_barrel_len * i_sin(s_angle) / 1024);
  graphics_context_set_stroke_color(ctx, GColorDarkGreen);
  graphics_draw_line(ctx, GPoint(cx, cy), GPoint(bx, by));

  // Loaded ball at barrel tip
  draw_chain_ball(ctx, GPoint(bx, by), s_loaded, s_loaded_pu);

  // HUD: score (upper left)
  // Emery: GOTHIC_24_BOLD (~1.5× GOTHIC_14_BOLD), taller rect to fit
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", s_score);
  graphics_context_set_text_color(ctx, GColorChromeYellow);
  if (s_is_emery) {
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(3, -5, 80, 26), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  } else {
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(3, 2, 55, 16), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }

  // HUD: chain count (upper right)
  // Emery: GOTHIC_24 (~1.5× GOTHIC_14), wider rect to fit
  snprintf(buf, sizeof(buf), "x%d", s_chain_len);
  graphics_context_set_text_color(ctx, GColorWhite);
  if (s_is_emery) {
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_24),
      GRect(s_sw - 56, -5, 53, 26), GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  } else {
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(s_sw - 38, 2, 36, 16), GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  }

  // Power-up HUD icons
  draw_powerup_hud(ctx);

  // GAME OVER overlay
  if (s_state == ST_LOSE) {
    GRect panel = GRect(cx - 54, cy - 38, 108, 76);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, panel, 6, GCornersAll);
    graphics_context_set_stroke_color(ctx, GColorChromeYellow);
    graphics_draw_round_rect(ctx, panel, 6);
    graphics_context_set_text_color(ctx, GColorChromeYellow);
    graphics_draw_text(ctx, "GAME OVER", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(panel.origin.x, panel.origin.y + 4, 108, 22),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    snprintf(buf, sizeof(buf), "Score: %d", s_score);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(panel.origin.x, panel.origin.y + 28, 108, 18),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    bool new_record = (s_score > 0 && s_score >= s_highscore);
    if (new_record) snprintf(buf, sizeof(buf), "NEW BEST!");
    else            snprintf(buf, sizeof(buf), "Best: %d", s_highscore);
    graphics_context_set_text_color(ctx, new_record ? GColorIslamicGreen : GColorLightGray);
    graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(panel.origin.x, panel.origin.y + 48, 108, 18),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  // PAUSED overlay
  if (s_state == ST_PAUSE) {
    GRect ppanel = GRect(cx - 40, cy - 18, 80, 36);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, ppanel, 6, GCornersAll);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_round_rect(ctx, ppanel, 6);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "PAUSED", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(ppanel.origin.x, ppanel.origin.y + 6, 80, 22),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

// ── Tick ──────────────────────────────────────────────────────────────────────

static void tick(void *data);
static void schedule_tick(void) { s_timer = app_timer_register(TICK_MS, tick, NULL); }

// Shared helper: tick down popups and coin (called from both slide and play paths)
static void tick_ui_timers(void) {
  for (int i = 0; i < MAX_POPUPS; i++) {
    if (s_popups[i].ttl > 0) s_popups[i].ttl--;
  }
  if (s_coin.active && s_coin.ttl > 0) {
    s_coin.ttl--;
    if (s_coin.ttl == 0) s_coin.active = false;
  }
  if (s_pu_backward_ttl  > 0) s_pu_backward_ttl--;
  if (s_pu_slowdown_ttl  > 0) s_pu_slowdown_ttl--;
  if (s_pu_accuracy_ttl  > 0) s_pu_accuracy_ttl--;
}

// Shared coin-hit helper for a projectile at (px,py); returns true if coin was hit.
static bool check_coin_hit(int p_idx, int16_t px, int16_t py) {
  if (!s_coin.active || s_coin.ttl == 0) return false;
  GPoint cp = s_coin_spots[s_coin.spot];
  int cdx = px - cp.x, cdy = py - cp.y;
  int hit_r = s_coin_r + s_ball_r;
  if (cdx * cdx + cdy * cdy >= hit_r * hit_r) return false;
  s_score += COIN_AWARD;
  int slot = 0;
  for (int si = 0; si < MAX_POPUPS; si++) {
    if (s_popups[si].ttl == 0) { slot = si; break; }
    if (s_popups[si].ttl < s_popups[slot].ttl) slot = si;
  }
  s_popups[slot] = (ScorePopup){ cp.x, (int16_t)(cp.y - s_coin_r - SC(12)), COIN_AWARD, COL_YELLOW, POPUP_TTL_MAX };
  s_coin.active = false; s_coin.ttl = 0;
  s_projs[p_idx].active = false;
  vibes_short_pulse();
  return true;
}

static void tick(void *data) {
  if (s_state == ST_LOSE || s_state == ST_PAUSE) return;

  int32_t d_fp    = (int32_t)s_ball_d << 8;
  int32_t max_pos = arc_total();

  // ── Slide-back animation ─────────────────────────────────────────────────
  if (s_state == ST_SLIDE) {
    tick_ui_timers();

    for (int p = 0; p < MAX_PROJS; p++) {
      if (!s_projs[p].active) continue;
      s_projs[p].x_fp += s_projs[p].dx_fp;
      s_projs[p].y_fp += s_projs[p].dy_fp;
      int16_t px = (int16_t)(s_projs[p].x_fp >> 8);
      int16_t py = (int16_t)(s_projs[p].y_fp >> 8);
      if (px < -s_ball_r || px > s_sw + s_ball_r || py < -s_ball_r || py > s_sh + s_ball_r) {
        s_projs[p].active = false; continue;
      }
      check_coin_hit(p, px, py);
    }

    int32_t current = s_chain[s_slide_lo].pos_fp;
    int32_t step = SLIDE_STEP_FP;
    bool arrived = false;
    if (current - step <= s_slide_target_fp) { step = current - s_slide_target_fp; arrived = true; }
    for (int i = s_slide_lo; i < s_chain_len; i++) {
      s_chain[i].pos_fp -= step;
      if (s_chain[i].pos_fp < 0) s_chain[i].pos_fp = 0;
      if (i + 1 < s_chain_len && s_chain[i + 1].pos_fp - s_chain[i].pos_fp > d_fp + (d_fp >> 2)) break;
    }
    if (arrived) {
      s_state = ST_PLAY;
      fire_cascade_at(s_slide_lo, s_slide_tail_idx, s_cascade_level);
    }
    layer_mark_dirty(s_layer);
    schedule_tick();
    return;
  }

  // ── Normal play tick ─────────────────────────────────────────────────────
  tick_ui_timers();

  // Coin milestone
  if (s_score >= s_score_milestone) {
    s_score_milestone += 500;
    if (!s_coin.active || s_coin.ttl == 0) {
      s_coin.spot   = rand() % 2;
      s_coin.ttl    = COIN_TTL_MAX;
      s_coin.active = true;
    }
  }

  // ── Motor: advance / reverse chain ──────────────────────────────────────
  if (s_chain_len > 0) {
    if (s_pu_backward_ttl > 0) {
      for (int i = s_chain_len - 1; i >= 0; i--) {
        s_chain[i].pos_fp -= BACKWARD_STEP_FP;
        if (i + 1 < s_chain_len && s_chain[i].pos_fp > s_chain[i + 1].pos_fp - d_fp)
          s_chain[i].pos_fp = s_chain[i + 1].pos_fp - d_fp;
      }
      while (s_chain_len > 0 && s_chain[0].pos_fp < 0) {
        for (int j = 0; j < s_chain_len - 1; j++) s_chain[j] = s_chain[j + 1];
        s_chain_len--;
      }
    } else {
      int32_t step = (s_pu_slowdown_ttl > 0) ? (CHAIN_STEP_FP / 4) : CHAIN_STEP_FP;
      s_chain[0].pos_fp += step;
      if (s_chain[0].pos_fp > max_pos) s_chain[0].pos_fp = max_pos;
      for (int i = 0; i < s_chain_len - 1; i++) {
        int32_t pushed = s_chain[i].pos_fp + d_fp;
        if (s_chain[i + 1].pos_fp < pushed) {
          s_chain[i + 1].pos_fp = pushed;
          if (s_chain[i + 1].pos_fp > max_pos) s_chain[i + 1].pos_fp = max_pos;
        } else break;
      }
    }
  }

  // ── Spawn new balls at START ─────────────────────────────────────────────
  if (s_pu_backward_ttl == 0) {
    int32_t spawn_step = (s_pu_slowdown_ttl > 0) ? (CHAIN_STEP_FP / 4) : CHAIN_STEP_FP;
    s_spawn_accum_fp += spawn_step;
    while (s_spawn_accum_fp >= d_fp) {
      if (s_chain_len < MAX_CHAIN) {
        BallColor new_color = (BallColor)(rand() % NUM_COLORS);
        uint8_t pu = PU_NONE;
        bool neighbor_has_pu = (s_chain_len > 0 && s_chain[0].powerup != PU_NONE);
        bool same_color_run  = (s_chain_len > 0 && s_chain[0].color == new_color);
        if (!neighbor_has_pu && !same_color_run && (rand() % POWERUP_ODDS) == 0)
          pu = (uint8_t)(1 + rand() % 4);
        for (int j = s_chain_len; j > 0; j--) s_chain[j] = s_chain[j - 1];
        s_chain[0].pos_fp  = 0;
        s_chain[0].color   = new_color;
        s_chain[0].powerup = pu;
        s_chain_len++;
      }
      s_spawn_accum_fp -= d_fp;
    }
  }

  // ── Lose check ───────────────────────────────────────────────────────────
  if (s_chain_len > 0) {
    GPoint front = path_at_arc(s_chain[s_chain_len - 1].pos_fp);
    GPoint hole  = s_path[PATH_LEN - 1];
    int16_t dx = front.x - hole.x, dy = front.y - hole.y;
    if ((int32_t)dx * dx + (int32_t)dy * dy < (s_ball_r + s_ball_r * 2) * (s_ball_r + s_ball_r * 2)) {
      if (s_score > s_highscore) {
        s_highscore = s_score;
        persist_write_int(PERSIST_KEY_HS, s_highscore);
      }
      s_state = ST_LOSE;
      vibes_long_pulse();
      layer_mark_dirty(s_layer);
      return;
    }
  }

  // ── Projectiles ──────────────────────────────────────────────────────────
  for (int p = 0; p < MAX_PROJS; p++) {
    if (!s_projs[p].active) continue;
    s_projs[p].x_fp += s_projs[p].dx_fp;
    s_projs[p].y_fp += s_projs[p].dy_fp;
    int16_t px = (int16_t)(s_projs[p].x_fp >> 8);
    int16_t py = (int16_t)(s_projs[p].y_fp >> 8);
    if (px < -s_ball_r || px > s_sw + s_ball_r || py < -s_ball_r || py > s_sh + s_ball_r) {
      s_projs[p].active = false; continue;
    }
    if (check_coin_hit(p, px, py)) continue;
    if (s_projs[p].pass_through) continue;

    // Find closest chain ball
    int best_i = -1;
    int best_d2 = s_ball_d * s_ball_d;
    for (int i = 0; i < s_chain_len; i++) {
      GPoint bp = path_at_arc(s_chain[i].pos_fp);
      int dx = px - bp.x, dy = py - bp.y;
      int d2 = dx * dx + dy * dy;
      if (d2 < best_d2) { best_d2 = d2; best_i = i; }
    }

    if (best_i >= 0) {
      int ins;
      if (best_i + 1 < s_chain_len) {
        GPoint bp_next = path_at_arc(s_chain[best_i + 1].pos_fp);
        int dx2 = px - bp_next.x, dy2 = py - bp_next.y;
        ins = (dx2 * dx2 + dy2 * dy2 < best_d2) ? best_i + 1 : best_i;
      } else {
        ins = best_i;
      }

      if (s_chain_len < MAX_CHAIN) {
        int32_t ins_pos = s_chain[ins].pos_fp;
        for (int j = s_chain_len; j > ins; j--) s_chain[j] = s_chain[j - 1];
        s_chain[ins].color   = s_projs[p].color;
        s_chain[ins].powerup = s_projs[p].powerup;
        s_chain[ins].pos_fp  = ins_pos;
        s_chain_len++;
        compact_positions(ins);
        s_projs[p].active = false;
        if (check_matches_at(ins) > 0) vibes_short_pulse();
      }
    }
  }

  layer_mark_dirty(s_layer);
  schedule_tick();
}

// ── Input ─────────────────────────────────────────────────────────────────────

static uint8_t random_powerup(void) {
  return ((rand() % POWERUP_ODDS) == 0) ? (uint8_t)(1 + rand() % 4) : PU_NONE;
}

// Swap the loaded (ready-to-fire) ball with the queued (next) ball.
static void do_swap(void) {
  BallColor tmp_color = s_loaded;
  uint8_t   tmp_pu    = s_loaded_pu;
  s_loaded    = s_queued;
  s_loaded_pu = s_queued_pu;
  s_queued    = tmp_color;
  s_queued_pu = tmp_pu;
  layer_mark_dirty(s_layer);
}

// Accelerometer tap callback — a wrist flick triggers a swap.
static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_state == ST_PLAY || s_state == ST_SLIDE) {
    do_swap();
  }
}

static void do_fire(void) {
  for (int i = 0; i < MAX_PROJS; i++) {
    if (!s_projs[i].active) {
      s_projs[i].active       = true;
      s_projs[i].color        = s_loaded;
      s_projs[i].powerup      = s_loaded_pu;
      s_projs[i].pass_through = (s_state == ST_SLIDE);
      int16_t bx = (int16_t)(s_scx + s_barrel_len * i_cos(s_angle) / 1024);
      int16_t by = (int16_t)(s_sh / 2 + s_barrel_len * i_sin(s_angle) / 1024);
      s_projs[i].x_fp  = (int32_t)bx << 8;
      s_projs[i].y_fp  = (int32_t)by << 8;
      // Scale projectile speed proportionally so travel time feels the same
      int32_t scaled_speed = (int32_t)PROJ_SPEED_FP * s_scale >> 8;
      s_projs[i].dx_fp = scaled_speed * i_cos(s_angle) / 1024;
      s_projs[i].dy_fp = scaled_speed * i_sin(s_angle) / 1024;
      s_loaded    = s_queued;
      s_loaded_pu = s_queued_pu;
      s_queued    = (BallColor)(rand() % NUM_COLORS);
      s_queued_pu = PU_NONE;
      return;
    }
  }
}

static void game_reset(void) {
  s_state      = ST_PLAY;
  s_score      = 0;
  s_angle      = 270;
  s_loaded     = (BallColor)(rand() % NUM_COLORS);
  s_loaded_pu  = PU_NONE;
  s_queued     = (BallColor)(rand() % NUM_COLORS);
  s_queued_pu  = PU_NONE;
  for (int i = 0; i < MAX_PROJS; i++) { s_projs[i].active = false; s_projs[i].pass_through = false; }
  for (int i = 0; i < MAX_POPUPS; i++) s_popups[i].ttl = 0;
  s_coin.active     = false; s_coin.ttl = 0; s_coin.spot = 0;
  s_score_milestone = 500;
  s_slide_lo = 0; s_slide_target_fp = 0; s_slide_tail_idx = 0; s_cascade_level = 0;
  s_pu_backward_ttl = 0;
  s_pu_slowdown_ttl = 0;
  s_pu_accuracy_ttl = 0;
  s_back_pressed_once = false;
  init_chain();
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  schedule_tick();
  layer_mark_dirty(s_layer);
}

static void cb_up(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_PLAY || s_state == ST_SLIDE) {
    s_angle = (s_angle - ROTATE_DEG + 360) % 360;
    layer_mark_dirty(s_layer);
  }
}
static void cb_down(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_PLAY || s_state == ST_SLIDE) {
    s_angle = (s_angle + ROTATE_DEG) % 360;
    layer_mark_dirty(s_layer);
  }
}
static void cb_select(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_PLAY || s_state == ST_SLIDE) {
    do_fire();
  } else if (s_state == ST_PAUSE) {
    s_state = ST_PLAY;
    schedule_tick();
    layer_mark_dirty(s_layer);
  } else if (s_state == ST_LOSE) {
    game_reset();
  }
}
static void cb_back(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_LOSE) { window_stack_pop(true); return; }
  if (s_state == ST_PAUSE) { window_stack_pop(true); return; }
  if (s_state == ST_PLAY || s_state == ST_SLIDE) {
    s_state = ST_PAUSE;
    if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
    layer_mark_dirty(s_layer);
  }
}

static void click_provider(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   80, cb_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 80, cb_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, cb_select);
  window_single_click_subscribe(BUTTON_ID_BACK, cb_back);
}

// ── Window lifecycle ──────────────────────────────────────────────────────────

static void win_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_sw = b.size.w; s_sh = b.size.h;
  s_scx = s_sw / 2 - SC(2);

  // Compute scale factor relative to Basalt baseline width (144px).
  // s_scale is in 256ths: 256 on Basalt, ~356 on Emery (200px wide).
  s_scale    = ((int32_t)s_sw * 256) / 144;
  s_is_emery = (s_sw >= 200);

  // Compute all scaled runtime geometry values
  s_ball_r      = SC(BASE_BALL_R);
  s_ball_d      = SC(BASE_BALL_D);
  s_explosion_r = SC(BASE_EXPLOSION_R);
  s_coin_r      = SC(BASE_COIN_R);
  s_barrel_len  = SC(BASE_BARREL_LEN);
  if (s_ball_r     < 1) s_ball_r     = 1;
  if (s_ball_d     < 2) s_ball_d     = 2;
  if (s_explosion_r < 1) s_explosion_r = 1;
  if (s_coin_r     < 1) s_coin_r     = 1;
  if (s_barrel_len < 1) s_barrel_len = 1;

  // Coin spots: placed proportionally to screen size.
  // On Basalt (144×168): spots at (14,152) and (129,151).
  // These are ~10% and ~90% of width, ~90% of height — keep that ratio.
  s_coin_spots[0] = GPoint((int16_t)(s_sw * 10 / 144), (int16_t)(s_sh * 152 / 168));
  s_coin_spots[1] = GPoint((int16_t)(s_sw * 129 / 144), (int16_t)(s_sh * 151 / 168));

  build_path();
  s_layer = layer_create(b);
  layer_set_update_proc(s_layer, draw_scene);
  layer_add_child(root, s_layer);
  uint16_t ms = 0;
  time_t now = time_ms(NULL, &ms);
  srand((unsigned int)((uint32_t)now * 1000 + ms));
  s_highscore = persist_exists(PERSIST_KEY_HS) ? persist_read_int(PERSIST_KEY_HS) : 0;
  accel_tap_service_subscribe(accel_tap_handler);
  game_reset();
}

static void win_unload(Window *window) {
  accel_tap_service_unsubscribe();
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  layer_destroy(s_layer);
}

// ── App entry ─────────────────────────────────────────────────────────────────

static void app_focus_handler(bool in_focus) {
  if (!in_focus) {
    if (s_state == ST_PLAY || s_state == ST_SLIDE) {
      s_state = ST_PAUSE;
      if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
      if (s_layer) layer_mark_dirty(s_layer);
    }
  }
}

static void init(void) {
  light_enable_interaction();
  s_win = window_create();
  window_set_background_color(s_win, GColorDarkGray);
  window_set_click_config_provider(s_win, click_provider);
  window_set_window_handlers(s_win, (WindowHandlers){ .load = win_load, .unload = win_unload });
  window_stack_push(s_win, true);
  app_focus_service_subscribe(app_focus_handler);
}

static void deinit(void) {
  app_focus_service_unsubscribe();
  window_destroy(s_win);
}

int main(void) { init(); app_event_loop(); deinit(); }
