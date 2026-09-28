/*
 * 3D Per-Object Pixel Art Bonfire Simulation (C Edition)
 *
 * Techniques:
 * - 3D Spherical Orbit Camera (Yaw θ, Pitch φ) with real-time controls & turntable
 * - 3D Stone Fire Ring (Base da Fogueira) with natural rock shading
 * - Segmented Wood Combustion (10 longitudinal segments per log):
 *   Fresh Wood -> Smoking -> Burning Flames -> Charred Black -> Brittle Ash
 * - Zero Floor Fire: Flames originate strictly from burning wood and central kindling
 * - Physical Self-Collapse Kinematics under gravity as structural mass burns away
 * - 3 Physical Stacking Modes: Fogueira Quadrada (Log Cabin), Tenda Cônica (Teepee), Pirâmide
 * - Discrete Object-Space Bark Plates (No orange tiger stripes, zero pixel creep)
 * - Concentric Growth Rings on Cut End-Caps
 * - 1-Pixel Cel-Art Outlines via G-Buffer Discontinuity
 * - ANSI 24-bit TrueColor Half-Block Character Output ('▀', '▄') with terminal transparency
 */

#define _POSIX_C_SOURCE 200809L
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_COLS 260
#define MAX_PIXEL_ROWS 180
#define MAX_SPARKS 256
#define MAX_ASH_FLAKES 128
#define MAX_LOGS 16
#define MAX_STONES 16
#define NUM_LOG_SEGS 10

typedef struct {
    uint8_t r, g, b;
} RGB;

static const RGB COLOR_BLACK = {0, 0, 0};

// Natural Oak & Pine Bark Palette (No orange neon stripes!)
static const RGB PALETTE_WOOD[] = {
    {20, 12, 8},      // 0: Dark Outline / Deep crevice
    {42, 27, 18},     // 1: Deep shadow bark (raw umber)
    {72, 48, 32},     // 2: Dark weathered oak
    {105, 72, 48},    // 3: Mid oak bark
    {138, 96, 64},    // 4: Warm dry timber fiber
    {168, 120, 82},   // 5: Muted wood highlight
    {200, 145, 102}   // 6: Warm firelit rim
};

// Charcoal and Carbonized Black Bark
static const RGB PALETTE_CHARRED[] = {
    {14, 10, 10},     // 0: Deep black crevice
    {26, 22, 22},     // 1: Charred black bark
    {46, 42, 42},     // 2: Dark charcoal
    {68, 62, 60},     // 3: Burnt grey bark
    {95, 88, 86}      // 4: Ash surface
};

// Granite & Basalt Stone Ring Palette
static const RGB PALETTE_STONE[] = {
    {18, 16, 14},     // 0: Stone outline / deep shade
    {42, 38, 35},     // 1: Dark basalt
    {68, 62, 58},     // 2: Mid granite grey
    {96, 88, 82},     // 3: Weathered mineral rock
    {128, 115, 102},  // 4: Warm firelit stone face
    {165, 145, 122}   // 5: Bright fire reflection
};

// End-cap cut face with concentric tree rings
static const RGB PALETTE_ENDCAP[] = {
    {20, 12, 8},      // 0: Outer bark rim
    {92, 60, 38},     // 1: Dark ring
    {130, 88, 56},    // 2: Mid ring
    {168, 116, 78},   // 3: Sapwood ring
    {205, 150, 105}   // 4: Pith core
};

// Embers & Flames (Deep red -> Hot orange -> Bright yellow -> White hot core)
static const RGB PALETTE_EMBERS[] = {
    {140, 18, 5},     // 0: Deep red ember
    {228, 48, 10},    // 1: Bright red flame
    {255, 115, 18},   // 2: Hot orange flame
    {255, 205, 48},   // 3: Yellow incandescence
    {255, 255, 210}   // 4: White hot core
};

// Ash & Charcoal (Charred crust -> Dark ash -> Chalky light ash)
static const RGB PALETTE_ASH[] = {
    {38, 35, 42},     // 0: Charcoal crust
    {68, 64, 74},     // 1: Dark ash
    {115, 112, 122},  // 2: Mid ash grey
    {165, 162, 172},  // 3: Light chalky ash
    {210, 208, 218}   // 4: White ash powder
};

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float temp;            // [0.0, 1.0]
    float burn_progress;   // [0.0, 1.0] (0.0=fresh, 0.4=charred black, 0.75=ash, 1.0=disintegrated)
    float structural_mass; // [1.0, 0.0]
} LogSegment;

typedef struct {
    int obj_id;
    Vec3 p1, p2;                       // Current geometry
    Vec3 p1_orig, p2_orig;             // Initial stack geometry
    Vec3 p1_collapsed, p2_collapsed;   // Physical collapse target under gravity
    float radius;
    float charred;
    float wood_health;
    float ash_amount;
    Vec3 axis, dir, tangent, bitangent;
    float length;
    LogSegment segments[NUM_LOG_SEGS];
} Cylinder3D;

typedef struct {
    int obj_id;
    Vec3 center;
    float radius;
} Stone3D;

#define OBJ_ASH_BED 500

typedef struct {
    Vec3 center;
    float radius_xz;
    float height;
    float heat;
    float volume;
} AshBed3D;

typedef struct {
    Vec3 pos;
    Vec3 vel;
    int life;
    int max_life;
    RGB color;
    bool active;
} Spark;

typedef struct {
    Vec3 pos;
    Vec3 vel;
    RGB color;
    bool active;
} AshFlake;

typedef struct {
    RGB color;
    bool is_sky;
} Pixel;

// Terminal and render state
static int g_term_cols = 80;
static int g_term_rows = 24;
static int g_pixel_w = 80;
static int g_pixel_h = 44;

static volatile sig_atomic_t g_resized = 0;
static volatile sig_atomic_t g_running = 1;
static struct termios g_orig_termios;

// Framebuffers
static Pixel g_frame[MAX_PIXEL_ROWS][MAX_COLS];
static int g_id_buf[MAX_PIXEL_ROWS][MAX_COLS];
static float g_depth_buf[MAX_PIXEL_ROWS][MAX_COLS];
static RGB g_shade_buf[MAX_PIXEL_ROWS][MAX_COLS];

// Fire simulation buffers
static float g_fire_heat[MAX_PIXEL_ROWS][MAX_COLS];
static float g_next_fire[MAX_PIXEL_ROWS][MAX_COLS];
static float g_fire_z[MAX_PIXEL_ROWS][MAX_COLS];

// 3D Ash Bed (Leito de cinzas acumulado na base)
static AshBed3D g_ash_bed;

// 3D Particles
static Spark g_sparks[MAX_SPARKS];
static AshFlake g_ash_flakes[MAX_ASH_FLAKES];

// 3D Logs & Stacking Modes
static Cylinder3D g_logs[MAX_LOGS];
static int g_num_logs = 6;
static int g_stack_mode = 0; // 0: Log Cabin (Fogueira Quadrada), 1: Teepee (Tenda Cônica), 2: Pyramid (Lean-to)

// 3D Stone Fire Ring (Base da Fogueira)
static Stone3D g_stones[MAX_STONES];
static int g_num_stones = 10;

// 3D Camera State
static float g_cam_yaw = 0.40f;       // Horizontal orbit angle (radians, ~23 deg)
static float g_cam_pitch = 0.35f;    // Elevation angle (radians, ~20 deg)
static bool g_auto_turntable = false; // Auto 360 degree turntable rotation

// Simulation dynamics
static float g_sim_time = 0.0f;
static float g_time_scale = 1.0f;
static float g_wind = 0.0f;
static float g_wind_target = 0.0f;
static float g_collapse_progress = 0.0f;
static bool g_force_collapse = false;
static bool g_paused = false;

// PRNG
static uint32_t g_rng = 0x8542b821;
static inline uint32_t xorshift32(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng = x;
}
static inline float rand_f(void) {
    return (xorshift32() & 0xFFFFFF) / 16777216.0f;
}
static inline int rand_range(int min_v, int max_v) {
    if (min_v >= max_v) return min_v;
    return min_v + (xorshift32() % (max_v - min_v + 1));
}

static inline float vec3_dot(Vec3 a, Vec3 b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return (Vec3){
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    };
}
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return (Vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}
static inline Vec3 vec3_add(Vec3 a, Vec3 b) {
    return (Vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}
static inline Vec3 vec3_scale(Vec3 a, float s) {
    return (Vec3){a.x * s, a.y * s, a.z * s};
}
static inline float vec3_len(Vec3 a) {
    return sqrtf(vec3_dot(a, a));
}
static inline Vec3 vec3_norm(Vec3 a) {
    float l = vec3_len(a);
    if (l < 1e-6f) return (Vec3){0, 1, 0};
    return vec3_scale(a, 1.0f / l);
}

static void recompute_cylinder_axes(Cylinder3D *c) {
    c->axis = vec3_sub(c->p2, c->p1);
    c->length = vec3_len(c->axis);
    c->dir = vec3_norm(c->axis);

    Vec3 ref_up = (Vec3){0.0f, 1.0f, 0.0f};
    if (fabsf(vec3_dot(ref_up, c->dir)) > 0.88f) {
        ref_up = (Vec3){1.0f, 0.0f, 0.0f};
    }
    c->tangent = vec3_norm(vec3_cross(c->dir, ref_up));
    c->bitangent = vec3_cross(c->dir, c->tangent);
}

static void init_cylinder(Cylinder3D *c, int id, Vec3 p1, Vec3 p2, Vec3 p1_collapsed, Vec3 p2_collapsed, float radius, float charred) {
    c->obj_id = id;
    c->p1 = p1;
    c->p2 = p2;
    c->p1_orig = p1;
    c->p2_orig = p2;
    c->p1_collapsed = p1_collapsed;
    c->p2_collapsed = p2_collapsed;
    c->radius = radius;
    c->charred = charred;
    c->wood_health = 100.0f;
    c->ash_amount = 0.0f;
    for (int s = 0; s < NUM_LOG_SEGS; s++) {
        c->segments[s].temp = 0.0f;
        c->segments[s].burn_progress = 0.0f;
        c->segments[s].structural_mass = 1.0f;
    }
    recompute_cylinder_axes(c);
}

static void safe_write(int fd, const void *buf, size_t count) {
    ssize_t ret = write(fd, buf, count);
    (void)ret;
}

static void reset_terminal(void) {
    safe_write(STDOUT_FILENO, "\033[?1049l\033[?25h\033[0m\n", 17);
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_running = 0;
    } else if (sig == SIGWINCH) {
        g_resized = 1;
    }
}

static void setup_terminal(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    atexit(reset_terminal);

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    safe_write(STDOUT_FILENO, "\033[?1049h\033[?25l\033[2J", 15);
}

static void update_dimensions(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_term_cols = ws.ws_col;
        g_term_rows = ws.ws_row;
    } else {
        g_term_cols = 80;
        g_term_rows = 24;
    }

    g_pixel_w = g_term_cols;
    if (g_pixel_w > MAX_COLS) g_pixel_w = MAX_COLS;
    if (g_pixel_w < 40) g_pixel_w = 40;

    g_pixel_h = (g_term_rows - 2) * 2;
    if (g_pixel_h > MAX_PIXEL_ROWS) g_pixel_h = MAX_PIXEL_ROWS;
    if (g_pixel_h < 30) g_pixel_h = 30;
}

// -----------------------------------------------------------------------------
// 3D STONE FIRE RING: Authentic circular containment base on the ground
// -----------------------------------------------------------------------------
static void build_stone_ring(void) {
    float ground_y = -4.2f;
    g_num_stones = 10;
    float ring_r = 7.0f;
    for (int i = 0; i < g_num_stones; i++) {
        float angle = i * (2.0f * (float)M_PI / (float)g_num_stones) + 0.15f;
        float sr = 0.95f + 0.20f * sinf(i * 3.7f);
        Vec3 c = {
            ring_r * cosf(angle),
            ground_y + sr * 0.82f,
            ring_r * sinf(angle)
        };
        g_stones[i].obj_id = 100 + i;
        g_stones[i].center = c;
        g_stones[i].radius = sr;
    }
}

static bool intersect_sphere(const Stone3D *st, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    Vec3 oc = vec3_sub(ro, st->center);
    float b = vec3_dot(oc, rd);
    float c = vec3_dot(oc, oc) - st->radius * st->radius;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    float sdisc = sqrtf(disc);
    float t = -b - sdisc;
    if (t < 0.1f) t = -b + sdisc;
    if (t < 0.1f) return false;
    *out_t = t;
    *out_pt = vec3_add(ro, vec3_scale(rd, t));
    *out_norm = vec3_norm(vec3_sub(*out_pt, st->center));
    return true;
}

static bool intersect_ash_bed(const AshBed3D *bed, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    if (bed->height < 0.08f) return false;
    float inv_r = 1.0f / bed->radius_xz;
    float inv_h = 1.0f / bed->height;

    // Ray transformed to unit ellipsoid dome space:
    // (x/r)^2 + ((y - cy)/h)^2 + (z/r)^2 <= 1, for y >= cy
    Vec3 p0 = (Vec3){ (ro.x - bed->center.x) * inv_r, (ro.y - bed->center.y) * inv_h, (ro.z - bed->center.z) * inv_r };
    Vec3 d0 = (Vec3){ rd.x * inv_r, rd.y * inv_h, rd.z * inv_r };

    float a = vec3_dot(d0, d0);
    float b = vec3_dot(p0, d0);
    float c = vec3_dot(p0, p0) - 1.0f;
    float disc = b * b - a * c;
    if (disc < 0.0f) return false;

    float sdisc = sqrtf(disc);
    float t1 = (-b - sdisc) / a;
    float t2 = (-b + sdisc) / a;

    float hit_t = -1.0f;
    if (t1 > 0.1f) {
        Vec3 pt1 = vec3_add(ro, vec3_scale(rd, t1));
        if (pt1.y >= bed->center.y - 0.05f) {
            hit_t = t1;
        }
    }
    if (hit_t < 0.0f && t2 > 0.1f) {
        Vec3 pt2 = vec3_add(ro, vec3_scale(rd, t2));
        if (pt2.y >= bed->center.y - 0.05f) {
            hit_t = t2;
        }
    }
    if (hit_t < 0.1f) return false;

    Vec3 pt = vec3_add(ro, vec3_scale(rd, hit_t));
    *out_t = hit_t;
    *out_pt = pt;

    Vec3 n_raw = (Vec3){
        (pt.x - bed->center.x) * (inv_r * inv_r),
        fmaxf(0.08f, pt.y - bed->center.y) * (inv_h * inv_h),
        (pt.z - bed->center.z) * (inv_r * inv_r)
    };
    *out_norm = vec3_norm(n_raw);
    return true;
}

// -----------------------------------------------------------------------------
// STACK GENERATORS: Real physical contact points, resting tiers, and gravity
// -----------------------------------------------------------------------------

// 1. FOGUEIRA QUADRADA / CABANA DE TRONCOS (Log Cabin / Cribbing)
static void build_stack_log_cabin(void) {
    g_num_logs = 6;
    float ground_y = -4.2f;
    float r1 = 1.25f;
    float r2 = 1.15f;
    float r3 = 0.95f;
    float span = 3.6f;

    // Tier 1 (Ground along X): 2 logs resting flat on ground
    float y1 = ground_y + r1;
    init_cylinder(&g_logs[0], 1,
                  (Vec3){-5.0f, y1, -span}, (Vec3){5.0f, y1, -span},
                  (Vec3){-5.0f, y1, -span}, (Vec3){5.0f, y1, -span},
                  r1, 0.25f);
    init_cylinder(&g_logs[1], 2,
                  (Vec3){-5.0f, y1,  span}, (Vec3){5.0f, y1,  span},
                  (Vec3){-5.0f, y1,  span}, (Vec3){5.0f, y1,  span},
                  r1, 0.25f);

    // Tier 2 (Resting across Tier 1 along Z): 2 logs
    float y2 = y1 + r1 + r2 - 0.25f; // Notch contact
    float y2_coll = ground_y + r2;   // When base burns out, drops to ground
    init_cylinder(&g_logs[2], 3,
                  (Vec3){-span, y2, -5.0f}, (Vec3){-span, y2, 5.0f},
                  (Vec3){-span, y2_coll, -4.5f}, (Vec3){-span, y2_coll, 4.5f},
                  r2, 0.40f);
    init_cylinder(&g_logs[3], 4,
                  (Vec3){ span, y2, -5.0f}, (Vec3){ span, y2, 5.0f},
                  (Vec3){ span, y2_coll, -4.5f}, (Vec3){ span, y2_coll, 4.5f},
                  r2, 0.40f);

    // Tier 3 (Cross diagonally across top): 2 logs
    float y3 = y2 + r2 + r3 - 0.20f;
    float y3_coll = ground_y + r3 + 0.3f;
    init_cylinder(&g_logs[4], 5,
                  (Vec3){-4.0f, y3, -3.0f}, (Vec3){4.0f, y3, 3.0f},
                  (Vec3){-2.5f, y3_coll, -1.8f}, (Vec3){2.5f, y3_coll, 1.8f},
                  r3, 0.60f);
    init_cylinder(&g_logs[5], 6,
                  (Vec3){-4.0f, y3,  3.0f}, (Vec3){4.0f, y3, -3.0f},
                  (Vec3){-2.5f, y3_coll,  1.8f}, (Vec3){2.5f, y3_coll, -1.8f},
                  r3, 0.60f);
}

// 2. TENDA CÔNICA (Teepee / Cone)
static void build_stack_teepee(void) {
    g_num_logs = 5;
    float ground_y = -4.2f;
    float base_r = 4.2f;
    float apex_r = 0.6f;
    float apex_y = 2.4f;
    float r = 1.15f;

    for (int i = 0; i < 5; i++) {
        float angle = (i * 2.0f * (float)M_PI / 5.0f) + 0.3f;
        Vec3 p1 = {base_r * cosf(angle), ground_y + r, base_r * sinf(angle)};
        Vec3 p2 = {apex_r * cosf(angle), apex_y, apex_r * sinf(angle)};
        // Collapses inward toward center hearth
        Vec3 p2_coll = {apex_r * 0.4f * cosf(angle), ground_y + r + 0.4f, apex_r * 0.4f * sinf(angle)};
        init_cylinder(&g_logs[i], i + 1, p1, p2, p1, p2_coll, r, 0.45f);
    }
}

// 3. PIRÂMIDE COM ESCORA (Pyramid / Lean-to)
static void build_stack_pyramid(void) {
    g_num_logs = 5;
    float ground_y = -4.2f;
    float r_base = 1.35f;
    float r_cross = 1.10f;
    float y1 = ground_y + r_base;

    // 2 Base logs
    init_cylinder(&g_logs[0], 1,
                  (Vec3){-5.5f, y1, -2.5f}, (Vec3){5.5f, y1, -2.5f},
                  (Vec3){-5.5f, y1, -2.5f}, (Vec3){5.5f, y1, -2.5f},
                  r_base, 0.25f);
    init_cylinder(&g_logs[1], 2,
                  (Vec3){-5.5f, y1,  2.5f}, (Vec3){5.5f, y1,  2.5f},
                  (Vec3){-5.5f, y1,  2.5f}, (Vec3){5.5f, y1,  2.5f},
                  r_base, 0.25f);

    // 3 Leaning logs
    float y_apex = 2.2f;
    float y_coll = ground_y + r_cross + 0.2f;
    init_cylinder(&g_logs[2], 3,
                  (Vec3){-4.2f, y1 + r_base - 0.2f, -1.0f}, (Vec3){0.0f, y_apex, -0.2f},
                  (Vec3){-4.0f, y_coll, -0.6f}, (Vec3){0.0f, y_coll, -0.1f},
                  r_cross, 0.50f);
    init_cylinder(&g_logs[3], 4,
                  (Vec3){ 4.2f, y1 + r_base - 0.2f, -1.0f}, (Vec3){0.0f, y_apex, -0.2f},
                  (Vec3){ 4.0f, y_coll, -0.6f}, (Vec3){0.0f, y_coll, -0.1f},
                  r_cross, 0.50f);
    init_cylinder(&g_logs[4], 5,
                  (Vec3){ 0.0f, y1 + r_base - 0.2f,  3.0f}, (Vec3){0.0f, y_apex,  0.4f},
                  (Vec3){ 0.0f, y_coll,  2.4f}, (Vec3){0.0f, y_coll,  0.2f},
                  r_cross, 0.50f);
}

static void init_scene(void) {
    memset(g_fire_heat, 0, sizeof(g_fire_heat));
    memset(g_next_fire, 0, sizeof(g_next_fire));
    memset(g_sparks, 0, sizeof(g_sparks));
    memset(g_ash_flakes, 0, sizeof(g_ash_flakes));

    g_ash_bed.center = (Vec3){0.0f, -4.2f, 0.0f};
    g_ash_bed.radius_xz = 5.6f;
    g_ash_bed.height = 0.35f;
    g_ash_bed.heat = 0.35f;
    g_ash_bed.volume = 0.0f;

    g_sim_time = 0.0f;
    g_collapse_progress = 0.0f;
    g_force_collapse = false;

    build_stone_ring();

    if (g_stack_mode == 0) {
        build_stack_log_cabin();
    } else if (g_stack_mode == 1) {
        build_stack_teepee();
    } else {
        build_stack_pyramid();
    }
}

static bool intersect_cylinder(const Cylinder3D *c, Vec3 ray_orig, Vec3 ray_dir,
                               float *out_t, Vec3 *out_pt, Vec3 *out_norm,
                               float *out_u, float *out_v, bool *out_endcap, float *out_rf,
                               int *out_seg_idx) {
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float best_u = 0, best_v = 0, best_rf = 1.0f;
    int best_seg = 0;
    bool is_cap = false;

    // Tube Body
    Vec3 delta_p = vec3_sub(ray_orig, c->p1);
    Vec3 d_proj = vec3_sub(ray_dir, vec3_scale(c->dir, vec3_dot(ray_dir, c->dir)));
    Vec3 dp_proj = vec3_sub(delta_p, vec3_scale(c->dir, vec3_dot(delta_p, c->dir)));

    float a = vec3_dot(d_proj, d_proj);
    if (a > 1e-6f) {
        float b = 2.0f * vec3_dot(d_proj, dp_proj);
        float cv = vec3_dot(dp_proj, dp_proj) - c->radius * c->radius;
        float disc = b * b - 4.0f * a * cv;
        if (disc >= 0.0f) {
            float sdisc = sqrtf(disc);
            float t0 = (-b - sdisc) / (2.0f * a);
            float t1 = (-b + sdisc) / (2.0f * a);
            float t = (t0 > 0.1f) ? t0 : t1;
            if (t > 0.1f) {
                Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
                float h = vec3_dot(vec3_sub(pt, c->p1), c->dir);
                if (h >= 0.0f && h <= c->length) {
                    Vec3 axis_pt = vec3_add(c->p1, vec3_scale(c->dir, h));
                    Vec3 norm = vec3_scale(vec3_sub(pt, axis_pt), 1.0f / c->radius);
                    float v = h / c->length;
                    float angle = atan2f(vec3_dot(norm, c->bitangent), vec3_dot(norm, c->tangent));
                    float u = (angle + (float)M_PI) / (2.0f * (float)M_PI);

                    int seg = (int)(v * NUM_LOG_SEGS);
                    if (seg < 0) seg = 0;
                    if (seg >= NUM_LOG_SEGS) seg = NUM_LOG_SEGS - 1;

                    best_t = t;
                    best_pt = pt;
                    best_norm = norm;
                    best_u = u;
                    best_v = v;
                    best_seg = seg;
                    is_cap = false;
                    hit = true;
                }
            }
        }
    }

    // End-cap P1 (-c->dir)
    float denom1 = vec3_dot(ray_dir, vec3_scale(c->dir, -1.0f));
    if (fabsf(denom1) > 1e-5f) {
        float t = vec3_dot(vec3_sub(c->p1, ray_orig), vec3_scale(c->dir, -1.0f)) / denom1;
        if (t > 0.1f && t < best_t) {
            Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
            float r = vec3_len(vec3_sub(pt, c->p1));
            if (r <= c->radius) {
                best_t = t;
                best_pt = pt;
                best_norm = vec3_scale(c->dir, -1.0f);
                best_u = 0.0f;
                best_v = 0.0f;
                best_rf = r / c->radius;
                best_seg = 0;
                is_cap = true;
                hit = true;
            }
        }
    }

    // End-cap P2 (+c->dir)
    float denom2 = vec3_dot(ray_dir, c->dir);
    if (fabsf(denom2) > 1e-5f) {
        float t = vec3_dot(vec3_sub(c->p2, ray_orig), c->dir) / denom2;
        if (t > 0.1f && t < best_t) {
            Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
            float r = vec3_len(vec3_sub(pt, c->p2));
            if (r <= c->radius) {
                best_t = t;
                best_pt = pt;
                best_norm = c->dir;
                best_u = 0.0f;
                best_v = 1.0f;
                best_rf = r / c->radius;
                best_seg = NUM_LOG_SEGS - 1;
                is_cap = true;
                hit = true;
            }
        }
    }

    if (hit) {
        *out_t = best_t;
        *out_pt = best_pt;
        *out_norm = best_norm;
        *out_u = best_u;
        *out_v = best_v;
        *out_endcap = is_cap;
        *out_rf = best_rf;
        *out_seg_idx = best_seg;
    }
    return hit;
}

static void spawn_spark_3d(Vec3 pos, Vec3 vel, int life, RGB color) {
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!g_sparks[i].active) {
            g_sparks[i].pos = pos;
            g_sparks[i].vel = vel;
            g_sparks[i].life = life;
            g_sparks[i].max_life = life;
            g_sparks[i].color = color;
            g_sparks[i].active = true;
            break;
        }
    }
}

static void spawn_ash_3d(Vec3 pos, Vec3 vel, RGB color) {
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (!g_ash_flakes[i].active) {
            g_ash_flakes[i].pos = pos;
            g_ash_flakes[i].vel = vel;
            g_ash_flakes[i].color = color;
            g_ash_flakes[i].active = true;
            break;
        }
    }
}

static void update_simulation(void) {
    if (g_paused) return;

    g_sim_time += 0.045f * g_time_scale;

    // Ambient Wind oscillation
    if (rand_f() < 0.05f) {
        g_wind_target = (rand_f() - 0.5f) * 1.8f;
    }
    g_wind += (g_wind_target - g_wind) * 0.04f;

    // Auto-turntable continuous orbit
    if (g_auto_turntable) {
        g_cam_yaw += 0.015f * g_time_scale;
        if (g_cam_yaw > 2.0f * (float)M_PI) g_cam_yaw -= 2.0f * (float)M_PI;
    }

    // -------------------------------------------------------------------------
    // PHYSICAL WOOD SEGMENT PROPAGATION (NO FAKE FLOOR FIRE!)
    // -------------------------------------------------------------------------
    // Center kindling nest position:
    Vec3 kindle_pos = (Vec3){0.0f, -2.6f, 0.0f};
    float kindle_heat = 0.0f;
    if (g_sim_time < 22.0f) {
        kindle_heat = fmaxf(0.0f, 1.0f - (g_sim_time / 22.0f));
    }

    // Update each log segment
    for (int i = 0; i < g_num_logs; i++) {
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
            Vec3 seg_p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));

            // Heat from initial kindling nest
            if (kindle_heat > 0.05f) {
                float d_k = vec3_len(vec3_sub(seg_p, kindle_pos));
                float d_surf = fmaxf(0.0f, d_k - g_logs[i].radius - 1.6f);
                if (d_surf < 2.8f) {
                    g_logs[i].segments[s].temp += 0.015f * (1.0f - d_surf / 2.8f) * kindle_heat * g_time_scale;
                }
            }

            // Cross-log fire radiation from burning segments
            for (int j = 0; j < g_num_logs; j++) {
                if (i == j) continue;
                for (int sj = 0; sj < NUM_LOG_SEGS; sj++) {
                    if (g_logs[j].segments[sj].temp > 0.35f) {
                        float tj = (sj + 0.5f) / (float)NUM_LOG_SEGS;
                        Vec3 pj = vec3_add(g_logs[j].p1, vec3_scale(g_logs[j].axis, tj));
                        float d_cross = vec3_len(vec3_sub(seg_p, pj));
                        float d_cross_surf = fmaxf(0.0f, d_cross - g_logs[i].radius - g_logs[j].radius);
                        if (d_cross_surf < 1.8f) {
                            g_logs[i].segments[s].temp += 0.005f * (1.0f - d_cross_surf / 1.8f) * g_time_scale;
                        }
                    }
                }
            }

            // Internal conduction along wood grain
            float cur = g_logs[i].segments[s].temp;
            float prev_t = (s > 0) ? g_logs[i].segments[s-1].temp : cur;
            float next_t = (s < NUM_LOG_SEGS - 1) ? g_logs[i].segments[s+1].temp : cur;
            if (prev_t > 0.35f || next_t > 0.35f) {
                g_logs[i].segments[s].temp += 0.0035f * g_time_scale;
            }

            // If temperature exceeds ignition threshold, start burning!
            if (g_logs[i].segments[s].temp > 0.35f) {
                g_logs[i].segments[s].temp = fminf(1.0f, g_logs[i].segments[s].temp + 0.004f * g_time_scale);
                g_logs[i].segments[s].burn_progress += 0.00045f * g_time_scale;
                g_logs[i].segments[s].structural_mass = fmaxf(0.0f, 1.0f - g_logs[i].segments[s].burn_progress * 1.15f);

                // Shed ash flakes as segment crumbles
                if (g_logs[i].segments[s].burn_progress > 0.60f && rand_f() < 0.05f) {
                    float angle = rand_f() * 6.28318f;
                    Vec3 rad_dir = vec3_add(vec3_scale(g_logs[i].tangent, cosf(angle)),
                                            vec3_scale(g_logs[i].bitangent, sinf(angle)));
                    Vec3 ash_p = vec3_add(seg_p, vec3_scale(rad_dir, g_logs[i].radius * 0.95f));
                    Vec3 vel = (Vec3){
                        (rand_f() - 0.5f) * 0.3f + g_wind * 0.3f,
                        -(rand_f() * 0.25f + 0.15f),
                        (rand_f() - 0.5f) * 0.3f
                    };
                    spawn_ash_3d(ash_p, vel, PALETTE_ASH[rand_range(2, 3)]);
                }
            }

            // Depletion after heavy burning (turns to ash embers)
            if (g_logs[i].segments[s].burn_progress > 0.85f) {
                g_logs[i].segments[s].temp = fmaxf(0.15f, g_logs[i].segments[s].temp - 0.001f * g_time_scale);
            }
        }
    }

    // -------------------------------------------------------------------------
    // SELF-COLLAPSE PHYSICS & 3D ASH BED ACCUMULATION
    // -------------------------------------------------------------------------
    float total_mass = 0.0f;
    int total_segs = g_num_logs * NUM_LOG_SEGS;
    int active_burning_segs = 0;
    for (int i = 0; i < g_num_logs; i++) {
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            total_mass += g_logs[i].segments[s].structural_mass;
            if (g_logs[i].segments[s].temp > 0.35f) active_burning_segs++;
        }
    }
    float avg_mass = total_mass / (float)total_segs;
    float burnt_mass = 1.0f - avg_mass;

    // Ash bed height builds up organically as wood is consumed
    g_ash_bed.height = fminf(1.4f, 0.35f + burnt_mass * 0.85f + g_ash_bed.volume);

    // Ash bed heat tracks active fire and stays hot as an ember bed
    if (active_burning_segs > 0) {
        g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.003f * g_time_scale);
    } else if (g_sim_time > 25.0f) {
        g_ash_bed.heat = fmaxf(0.12f, g_ash_bed.heat - 0.0003f * g_time_scale);
    }

    float collapse_factor = fmaxf(0.0f, 1.0f - avg_mass);
    if (g_force_collapse) collapse_factor = 1.0f;

    for (int i = 0; i < g_num_logs; i++) {
        float log_mass = 0.0f;
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            log_mass += g_logs[i].segments[s].structural_mass;
        }
        float log_loss = 1.0f - (log_mass / (float)NUM_LOG_SEGS);
        float c = fmaxf(collapse_factor * 0.75f, log_loss);
        if (g_force_collapse) c = 1.0f;
        if (c > 1.0f) c = 1.0f;

        g_logs[i].p1.x = g_logs[i].p1_orig.x * (1.0f - c) + g_logs[i].p1_collapsed.x * c;
        g_logs[i].p1.y = g_logs[i].p1_orig.y * (1.0f - c) + g_logs[i].p1_collapsed.y * c;
        g_logs[i].p1.z = g_logs[i].p1_orig.z * (1.0f - c) + g_logs[i].p1_collapsed.z * c;

        g_logs[i].p2.x = g_logs[i].p2_orig.x * (1.0f - c) + g_logs[i].p2_collapsed.x * c;
        g_logs[i].p2.y = g_logs[i].p2_orig.y * (1.0f - c) + g_logs[i].p2_collapsed.y * c;
        g_logs[i].p2.z = g_logs[i].p2_orig.z * (1.0f - c) + g_logs[i].p2_collapsed.z * c;
        recompute_cylinder_axes(&g_logs[i]);
    }

    // -------------------------------------------------------------------------
    // STRICT FIRE PROJECTION: Heat originates strictly from burning wood
    // -------------------------------------------------------------------------
    Vec3 target = (Vec3){0.0f, -1.2f, 0.0f};
    float cam_dist = 28.0f;
    Vec3 cam_pos = (Vec3){
        cam_dist * cosf(g_cam_pitch) * sinf(g_cam_yaw),
        target.y + cam_dist * sinf(g_cam_pitch),
        -cam_dist * cosf(g_cam_pitch) * cosf(g_cam_yaw)
    };
    Vec3 fwd = vec3_norm(vec3_sub(target, cam_pos));
    Vec3 up_w = (Vec3){0.0f, 1.0f, 0.0f};
    Vec3 right = vec3_norm(vec3_cross(fwd, up_w));
    Vec3 up = vec3_cross(right, fwd);

    float world_w = 22.0f;
    float world_h = 14.0f;

    // Reset next fire frame
    memset(g_next_fire, 0, sizeof(g_next_fire));

    // 1. Kindling flame (early stage only)
    if (kindle_heat > 0.05f) {
        Vec3 rel_k = vec3_sub(kindle_pos, cam_pos);
        int kx = (int)(((vec3_dot(rel_k, right) / world_w) + 0.5f) * g_pixel_w);
        int ky = (int)((0.5f - (vec3_dot(rel_k, up) / world_h)) * g_pixel_h);
        float kz = vec3_dot(rel_k, fwd);

        for (int dy = -3; dy <= 2; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                int px = kx + dx;
                int py = ky + dy;
                if (px >= 0 && px < g_pixel_w && py >= 0 && py < g_pixel_h) {
                    float d = sqrtf((dx * 0.9f)*(dx * 0.9f) + (dy * 1.5f)*(dy * 1.5f));
                    if (d < 3.2f) {
                        float h = kindle_heat * 0.85f * (1.0f - d / 3.2f);
                        g_fire_heat[py][px] = fmaxf(g_fire_heat[py][px], h);
                        g_fire_z[py][px] = kz - 0.25f;
                    }
                }
            }
        }
    }

    // 2. Heat anchored exclusively to burning wood segments!
    for (int i = 0; i < g_num_logs; i++) {
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            float temp = g_logs[i].segments[s].temp;
            if (temp > 0.40f) {
                float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));
                Vec3 rel_p = vec3_sub(p, cam_pos);
                int px = (int)(((vec3_dot(rel_p, right) / world_w) + 0.5f) * g_pixel_w);
                int py = (int)((0.5f - (vec3_dot(rel_p, up) / world_h)) * g_pixel_h);
                float pz = vec3_dot(rel_p, fwd);

                float flame_h = temp * 0.95f;
                for (int dy = -3; dy <= 1; dy++) {
                    for (int dx = -3; dx <= 3; dx++) {
                        int sx = px + dx;
                        int sy = py + dy;
                        if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                            float d = sqrtf((dx * 0.9f)*(dx * 0.9f) + (dy * 1.5f)*(dy * 1.5f));
                            if (d < 3.0f) {
                                float val = flame_h * (1.0f - d / 3.0f);
                                g_fire_heat[sy][sx] = fmaxf(g_fire_heat[sy][sx], val);
                                g_fire_z[sy][sx] = pz - 0.25f;
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Convective flame propagation upwards
    for (int y = g_pixel_h - 4; y >= 2; y--) {
        for (int x = 0; x < g_pixel_w; x++) {
            int src_x = x;
            int wind_step = (g_wind > 0.35f) ? 1 : ((g_wind < -0.35f) ? -1 : 0);
            int jitter = (xorshift32() % 3) - 1;
            src_x += (rand_f() < 0.40f) ? wind_step : jitter;
            if (src_x < 0) src_x = 0;
            if (src_x >= g_pixel_w) src_x = g_pixel_w - 1;

            float below = g_fire_heat[y + 1][src_x];
            if (below <= 0.04f) {
                g_next_fire[y][x] = 0.0f;
                continue;
            }

            float decay = 0.032f + 0.030f * rand_f();
            float val = fmaxf(0.0f, below - decay);
            g_next_fire[y][x] = val;
            g_fire_z[y][x] = g_fire_z[y + 1][src_x] - 0.02f;
        }
    }

    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_heat[y][x] = g_next_fire[y][x];
        }
    }

    // 4. Sparks Ejection from burning wood
    if (rand_f() < 0.65f) {
        // Pick a burning segment
        for (int i = 0; i < g_num_logs; i++) {
            int s = rand_range(0, NUM_LOG_SEGS - 1);
            if (g_logs[i].segments[s].temp > 0.50f) {
                float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));
                Vec3 spark_v = (Vec3){(rand_f() - 0.5f) * 1.5f + g_wind * 1.2f, rand_f() * 3.2f + 2.0f, (rand_f() - 0.5f) * 1.5f};
                RGB spark_col = PALETTE_EMBERS[rand_range(2, 4)];
                spawn_spark_3d(p, spark_v, rand_range(16, 42), spark_col);
                break;
            }
        }
    }

    for (int i = 0; i < MAX_SPARKS; i++) {
        if (g_sparks[i].active) {
            g_sparks[i].pos.x += g_sparks[i].vel.x * 0.05f;
            g_sparks[i].pos.y += g_sparks[i].vel.y * 0.05f;
            g_sparks[i].pos.z += g_sparks[i].vel.z * 0.05f;
            g_sparks[i].vel.y -= 0.04f; // gravity
            g_sparks[i].life--;
            if (g_sparks[i].life <= 0 || g_sparks[i].pos.y < -4.5f) {
                g_sparks[i].active = false;
            }
        }
    }

    // 5. 3D Ash Flakes drift & accumulation into 3D Ash Bed
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (g_ash_flakes[i].active) {
            g_ash_flakes[i].pos.x += g_ash_flakes[i].vel.x * 0.04f + sinf(g_ash_flakes[i].pos.y * 1.5f + g_sim_time) * 0.015f;
            g_ash_flakes[i].pos.y += g_ash_flakes[i].vel.y * 0.04f;
            g_ash_flakes[i].pos.z += g_ash_flakes[i].vel.z * 0.04f;

            float r_sq = g_ash_flakes[i].pos.x * g_ash_flakes[i].pos.x + g_ash_flakes[i].pos.z * g_ash_flakes[i].pos.z;
            float r_bed = g_ash_bed.radius_xz;
            float bed_h = (r_sq < r_bed * r_bed) ? (g_ash_bed.height * sqrtf(1.0f - r_sq / (r_bed * r_bed))) : 0.0f;
            float floor_y = -4.2f + bed_h;

            if (g_ash_flakes[i].pos.y <= floor_y) {
                if (r_sq < r_bed * r_bed) {
                    g_ash_bed.volume += 0.002f;
                    g_ash_bed.height = fminf(1.4f, 0.35f + g_ash_bed.volume);
                }
                g_ash_flakes[i].active = false;
            }
        }
    }
}

static void render_scene(void) {
    // Clear Framebuffers
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_frame[y][x].is_sky = true;
            g_frame[y][x].color = COLOR_BLACK;
            g_id_buf[y][x] = 0;
            g_depth_buf[y][x] = 1e9f;
            g_shade_buf[y][x] = COLOR_BLACK;
        }
    }

    // Compute Camera Vectors
    Vec3 target = (Vec3){0.0f, -1.2f, 0.0f};
    float cam_dist = 28.0f;
    Vec3 cam_pos = (Vec3){
        cam_dist * cosf(g_cam_pitch) * sinf(g_cam_yaw),
        target.y + cam_dist * sinf(g_cam_pitch),
        -cam_dist * cosf(g_cam_pitch) * cosf(g_cam_yaw)
    };
    Vec3 fwd = vec3_norm(vec3_sub(target, cam_pos));
    Vec3 up_w = (Vec3){0.0f, 1.0f, 0.0f};
    Vec3 right = vec3_norm(vec3_cross(fwd, up_w));
    Vec3 up = vec3_cross(right, fwd);

    float world_w = 22.0f;
    float world_h = 14.0f;

    // Fire point light positioned in the hearth core
    float flicker = 1.0f + 0.16f * sinf(g_sim_time * 8.0f) + 0.10f * cosf(g_sim_time * 13.0f);
    Vec3 light_pos = (Vec3){0.0f, -1.8f, 0.0f};
    float light_intensity = 1.9f * flicker;

    // =========================================================================
    // 1. 3D Raycasting with Stones and Segmented Wood
    // =========================================================================
    for (int y = 0; y < g_pixel_h; y++) {
        float wy = (((g_pixel_h - 1 - y) / (float)g_pixel_h) - 0.5f) * world_h;
        for (int x = 0; x < g_pixel_w; x++) {
            float wx = ((x / (float)g_pixel_w) - 0.5f) * world_w;

            Vec3 ray_orig = vec3_add(cam_pos, vec3_add(vec3_scale(right, wx), vec3_scale(up, wy)));
            Vec3 ray_dir = fwd;

            float closest_t = 1e9f;
            int hit_type = 0; // 0: none, 1: stone, 2: log

            // Test 3D Stone Fire Ring
            Stone3D *hit_stone = NULL;
            Vec3 stone_pt = {0,0,0}, stone_norm = {0,1,0};
            for (int i = 0; i < g_num_stones; i++) {
                float t;
                Vec3 pt, norm;
                if (intersect_sphere(&g_stones[i], ray_orig, ray_dir, &t, &pt, &norm)) {
                    if (t < closest_t) {
                        closest_t = t;
                        stone_pt = pt;
                        stone_norm = norm;
                        hit_stone = &g_stones[i];
                        hit_type = 1;
                    }
                }
            }

            // Test 3D Segmented Logs
            Cylinder3D *hit_log = NULL;
            Vec3 log_pt = {0,0,0}, log_norm = {0,1,0};
            float hit_u = 0, hit_v = 0, hit_rf = 1.0f;
            int hit_seg = 0;
            bool hit_cap = false;

            for (int i = 0; i < g_num_logs; i++) {
                float t, u, v, rf;
                Vec3 pt, norm;
                bool is_cap;
                int seg;
                if (intersect_cylinder(&g_logs[i], ray_orig, ray_dir, &t, &pt, &norm, &u, &v, &is_cap, &rf, &seg)) {
                    if (t < closest_t) {
                        closest_t = t;
                        log_pt = pt;
                        log_norm = norm;
                        hit_u = u;
                        hit_v = v;
                        hit_cap = is_cap;
                        hit_rf = rf;
                        hit_seg = seg;
                        hit_log = &g_logs[i];
                        hit_type = 2;
                    }
                }
            }

            // Test 3D Ash Bed (Leito de cinzas central)
            Vec3 ash_bed_pt = {0,0,0}, ash_bed_norm = {0,1,0};
            float ab_t;
            Vec3 ab_p, ab_n;
            if (intersect_ash_bed(&g_ash_bed, ray_orig, ray_dir, &ab_t, &ab_p, &ab_n)) {
                if (ab_t < closest_t) {
                    closest_t = ab_t;
                    ash_bed_pt = ab_p;
                    ash_bed_norm = ab_n;
                    hit_type = 3;
                }
            }

            if (hit_type == 1 && hit_stone != NULL) {
                g_id_buf[y][x] = hit_stone->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(stone_pt, cam_pos), fwd);

                // Lighting on stone
                Vec3 l_vec = vec3_sub(light_pos, stone_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.07f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(stone_norm, l_dir));
                float ambient = 0.22f + 0.10f * fmaxf(0.0f, stone_norm.y);
                float s_val = ndotl * atten * light_intensity * 2.2f + ambient;

                float rock_noise = (sinf(stone_pt.x * 3.5f + stone_pt.z * 4.1f) * 0.5f + 0.5f) * 0.18f;
                int s_idx = (int)((s_val + rock_noise) * 2.8f);
                if (s_idx < 1) s_idx = 1;
                if (s_idx > 5) s_idx = 5;
                g_shade_buf[y][x] = PALETTE_STONE[s_idx];

            } else if (hit_type == 2 && hit_log != NULL) {
                g_id_buf[y][x] = hit_log->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(log_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, log_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, (vec3_dot(log_norm, l_dir) + 0.45f) / 1.45f);
                float ambient = 0.28f + 0.12f * fmaxf(0.0f, log_norm.y);
                float light_val = (ndotl * atten * light_intensity * 2.4f + ambient);

                float burn = hit_log->segments[hit_seg].burn_progress;
                float seg_temp = hit_log->segments[hit_seg].temp;

                if (hit_cap) {
                    float r_q = floorf(hit_rf * 8.0f) / 8.0f;
                    int ring_band = ((int)(r_q * 8.0f)) % 2;
                    int col_idx = 1 + ring_band + ((hit_rf > 0.70f) ? 1 : 0) + ((light_val > 0.80f) ? 1 : 0);
                    if (col_idx > 4) col_idx = 4;
                    g_shade_buf[y][x] = PALETTE_ENDCAP[col_idx];
                } else {
                    float num_plates_u = 14.0f;
                    float num_plates_v = hit_log->length * 2.2f;
                    float u_plate = floorf(hit_u * num_plates_u);
                    float v_plate = floorf(hit_v * num_plates_v);

                    float u_frac = (hit_u * num_plates_u) - u_plate;
                    float v_frac = (hit_v * num_plates_v) - v_plate;
                    bool is_furrow = (u_frac < 0.12f || u_frac > 0.88f || (v_frac < 0.08f && ((int)u_plate % 2 == 0)));

                    if (burn > 0.70f) {
                        // STAGE: Calcified Ash & Embers
                        if (is_furrow) {
                            if (seg_temp > 0.30f) {
                                int emb_idx = (int)(seg_temp * 3.5f);
                                if (emb_idx < 0) emb_idx = 0;
                                if (emb_idx > 3) emb_idx = 3;
                                g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                            } else {
                                g_shade_buf[y][x] = PALETTE_CHARRED[0];
                            }
                        } else {
                            // Upward facing normal holds ash mantle; underside sheds it
                            float up_factor = log_norm.y;
                            if (up_factor > 0.25f) {
                                // Top crest: thick chalky white and light ash
                                int ash_idx = (int)(light_val * 2.0f + 1.8f);
                                if (ash_idx < 2) ash_idx = 2;
                                if (ash_idx > 4) ash_idx = 4;
                                g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                            } else if (up_factor > -0.1f) {
                                // Sloped flanks: mid-grey ash
                                int ash_idx = (int)(light_val * 1.8f + 0.9f);
                                if (ash_idx < 1) ash_idx = 1;
                                if (ash_idx > 3) ash_idx = 3;
                                g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                            } else {
                                // Underside: charred carbon crust
                                int c_idx = (int)(light_val * 1.8f);
                                if (c_idx < 0) c_idx = 0;
                                if (c_idx > 2) c_idx = 2;
                                g_shade_buf[y][x] = PALETTE_CHARRED[c_idx];
                            }
                        }
                    } else if (burn > 0.40f) {
                        // STAGE: Active Combustion & Alligator Charring
                        if (is_furrow && seg_temp > 0.40f) {
                            int emb_idx = (int)(seg_temp * 3.5f);
                            if (emb_idx < 0) emb_idx = 0;
                            if (emb_idx > 3) emb_idx = 3;
                            g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                        } else {
                            if (burn > 0.55f && log_norm.y > 0.45f) {
                                // Early ash dusting on top of charred plates
                                g_shade_buf[y][x] = PALETTE_ASH[1];
                            } else {
                                int c_idx = (int)(light_val * 2.0f);
                                if (c_idx < 0) c_idx = 0;
                                if (c_idx > 4) c_idx = 4;
                                g_shade_buf[y][x] = PALETTE_CHARRED[c_idx];
                            }
                        }
                    } else if (burn > 0.15f) {
                        // STAGE: Scorched / Soot Bark
                        if (is_furrow) {
                            g_shade_buf[y][x] = PALETTE_CHARRED[0];
                        } else {
                            int b_idx = (int)(light_val * 2.0f);
                            if (b_idx < 1) b_idx = 1;
                            if (b_idx > 4) b_idx = 4;
                            g_shade_buf[y][x] = PALETTE_WOOD[b_idx];
                        }
                    } else {
                        // STAGE: Fresh Natural Oak Wood
                        if (is_furrow) {
                            g_shade_buf[y][x] = PALETTE_WOOD[0];
                        } else {
                            int b_idx = (int)(light_val * 2.8f);
                            if (b_idx < 1) b_idx = 1;
                            if (b_idx > 6) b_idx = 6;
                            g_shade_buf[y][x] = PALETTE_WOOD[b_idx];
                        }
                    }
                }

            } else if (hit_type == 3) {
                g_id_buf[y][x] = OBJ_ASH_BED;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(ash_bed_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, ash_bed_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(ash_bed_norm, l_dir));
                float ambient = 0.24f + 0.12f * fmaxf(0.0f, ash_bed_norm.y);
                float s_val = ndotl * atten * light_intensity * 2.2f + ambient;

                float r_core = sqrtf(ash_bed_pt.x * ash_bed_pt.x + ash_bed_pt.z * ash_bed_pt.z);
                float core_heat = g_ash_bed.heat * fmaxf(0.0f, 1.0f - r_core / 4.8f);

                // Continuous cellular fissure pattern
                float f1 = sinf(ash_bed_pt.x * 1.3f + ash_bed_pt.z * 0.7f);
                float f2 = cosf(ash_bed_pt.z * 1.4f - ash_bed_pt.x * 0.6f);
                float fissure = fabsf(f1 * f2);

                if (fissure < 0.18f && core_heat > 0.22f) {
                    // Deep incandescent ember vein peeking through the ash bed
                    int emb_idx = (int)(core_heat * 3.8f);
                    if (emb_idx < 0) emb_idx = 0;
                    if (emb_idx > 3) emb_idx = 3;
                    g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                } else {
                    // Cohesive calcified ash mantle
                    int ash_idx;
                    if (r_core < 1.8f && core_heat > 0.35f) {
                        ash_idx = (int)(s_val * 1.6f);
                        if (ash_idx < 1) ash_idx = 1;
                        if (ash_idx > 2) ash_idx = 2;
                    } else if (r_core < 3.8f) {
                        ash_idx = (int)(s_val * 2.2f);
                        if (ash_idx < 2) ash_idx = 2;
                        if (ash_idx > 3) ash_idx = 3;
                    } else {
                        ash_idx = (int)(s_val * 2.6f);
                        if (ash_idx < 2) ash_idx = 2;
                        if (ash_idx > 4) ash_idx = 4;
                    }
                    g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                }
            }
        }
    }

    // 2. 1-Pixel Cel-Art Outline Pass
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            int curr_id = g_id_buf[y][x];
            if (curr_id == 0) continue;

            bool is_edge = false;
            static const int d_coords[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int k = 0; k < 4; k++) {
                int ny = y + d_coords[k][0];
                int nx = x + d_coords[k][1];
                if (ny >= 0 && ny < g_pixel_h && nx >= 0 && nx < g_pixel_w) {
                    int nid = g_id_buf[ny][nx];
                    if (nid != curr_id) {
                        if (nid == 0 || g_depth_buf[y][x] < g_depth_buf[ny][nx]) {
                            is_edge = true;
                            break;
                        }
                    } else if (fabsf(g_depth_buf[y][x] - g_depth_buf[ny][nx]) > 0.75f) {
                        is_edge = true;
                        break;
                    }
                }
            }

            if (is_edge) {
                if (curr_id == OBJ_ASH_BED) {
                    g_frame[y][x].color = PALETTE_ASH[0];
                } else {
                    g_frame[y][x].color = PALETTE_WOOD[0]; // Dark outline
                }
                g_frame[y][x].is_sky = false;
            } else {
                g_frame[y][x].color = g_shade_buf[y][x];
                g_frame[y][x].is_sky = false;
            }
        }
    }

    // 3. Composite Fire Heat Layer with Depth Interleaving
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            float heat = g_fire_heat[y][x];
            if (heat > 0.05f) {
                RGB fire_col;
                if (heat > 0.85f) fire_col = PALETTE_EMBERS[4];
                else if (heat > 0.65f) fire_col = PALETTE_EMBERS[3];
                else if (heat > 0.40f) fire_col = PALETTE_EMBERS[2];
                else if (heat > 0.18f) fire_col = PALETTE_EMBERS[1];
                else fire_col = PALETTE_EMBERS[0];

                if (g_id_buf[y][x] > 0) {
                    if (g_fire_z[y][x] < g_depth_buf[y][x]) {
                        g_frame[y][x].color = fire_col;
                        g_frame[y][x].is_sky = false;
                    } else if (heat > 0.55f) {
                        g_frame[y][x].color = fire_col;
                        g_frame[y][x].is_sky = false;
                    }
                } else {
                    g_frame[y][x].color = fire_col;
                    g_frame[y][x].is_sky = false;
                }
            }
        }
    }

    // 4. Ash Flakes Falling in 3D
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (g_ash_flakes[i].active) {
            Vec3 p_rel = vec3_sub(g_ash_flakes[i].pos, cam_pos);
            int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
            int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
            float flake_z = vec3_dot(p_rel, fwd);
            if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                if (flake_z < g_depth_buf[sy][sx]) {
                    g_frame[sy][sx].color = g_ash_flakes[i].color;
                    g_frame[sy][sx].is_sky = false;
                }
            }
        }
    }

    // 6. Sparks Rising
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (g_sparks[i].active) {
            Vec3 p_rel = vec3_sub(g_sparks[i].pos, cam_pos);
            int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
            int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
            float spark_z = vec3_dot(p_rel, fwd);
            if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                if (spark_z < g_depth_buf[sy][sx] || g_fire_heat[sy][sx] > 0.2f) {
                    g_frame[sy][sx].color = g_sparks[i].color;
                    g_frame[sy][sx].is_sky = false;
                }
            }
        }
    }
}

static void present_frame(void) {
    static char buf[140000];
    int buf_len = 0;

    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[H");

    int prev_fg_r = -1, prev_fg_g = -1, prev_fg_b = -1;
    int prev_bg_r = -1, prev_bg_g = -1, prev_bg_b = -1;
    bool prev_bg_transp = true;

    int text_rows = g_pixel_h / 2;
    for (int r = 0; r < text_rows; r++) {
        int y_top = r * 2;
        int y_bot = r * 2 + 1;

        for (int x = 0; x < g_pixel_w; x++) {
            Pixel top = g_frame[y_top][x];
            Pixel bot = g_frame[y_bot][x];

            // Case A: Both sky -> space with transparent background
            if (top.is_sky && bot.is_sky) {
                if (!prev_bg_transp) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[49m");
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, " ");
                continue;
            }

            // Case B: Top is sky, Bottom is color -> ▄ with transparent BG
            if (top.is_sky && !bot.is_sky) {
                if (bot.color.r != prev_fg_r || bot.color.g != prev_fg_g || bot.color.b != prev_fg_b) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[38;2;%d;%d;%dm",
                                        bot.color.r, bot.color.g, bot.color.b);
                    prev_fg_r = bot.color.r; prev_fg_g = bot.color.g; prev_fg_b = bot.color.b;
                }
                if (!prev_bg_transp) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[49m");
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "▄");
                continue;
            }

            // Case C: Top is color, Bottom is sky -> ▀ with transparent BG
            if (!top.is_sky && bot.is_sky) {
                if (top.color.r != prev_fg_r || top.color.g != prev_fg_g || top.color.b != prev_fg_b) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[38;2;%d;%d;%dm",
                                        top.color.r, top.color.g, top.color.b);
                    prev_fg_r = top.color.r; prev_fg_g = top.color.g; prev_fg_b = top.color.b;
                }
                if (!prev_bg_transp) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[49m");
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "▀");
                continue;
            }

            // Case D: Both colors -> ▀ with FG and BG
            if (top.color.r != prev_fg_r || top.color.g != prev_fg_g || top.color.b != prev_fg_b) {
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[38;2;%d;%d;%dm",
                                    top.color.r, top.color.g, top.color.b);
                prev_fg_r = top.color.r; prev_fg_g = top.color.g; prev_fg_b = top.color.b;
            }
            if (bot.color.r != prev_bg_r || bot.color.g != prev_bg_g || bot.color.b != prev_bg_b) {
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[48;2;%d;%d;%dm",
                                    bot.color.r, bot.color.g, bot.color.b);
                prev_bg_r = bot.color.r; prev_bg_g = bot.color.g; prev_bg_b = bot.color.b;
                prev_bg_transp = false;
            }
            buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "▀");

            if (buf_len > 120000) {
                safe_write(STDOUT_FILENO, buf, buf_len);
                buf_len = 0;
            }
        }
        buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[0m\n");
        prev_fg_r = prev_fg_g = prev_fg_b = -1;
        prev_bg_r = prev_bg_g = prev_bg_b = -1;
        prev_bg_transp = true;
    }

    const char *stage_name = "1/7: Gravetos e Ignição";
    if (g_sim_time > 200.0f) stage_name = "7/7: Cinzas Frias";
    else if (g_sim_time > 155.0f) stage_name = "6/7: Leito de Brasas";
    else if (g_sim_time > 125.0f) stage_name = "5/7: Colapso por Gravidade";
    else if (g_sim_time > 85.0f) stage_name = "4/7: Madeira em Cinza";
    else if (g_sim_time > 35.0f) stage_name = "3/7: Fogueira Roaring";
    else if (g_sim_time > 12.0f) stage_name = "2/7: Chamas nas Toras";

    const char *mode_name = "Fogueira Quadrada";
    if (g_stack_mode == 1) mode_name = "Tenda Cônica";
    else if (g_stack_mode == 2) mode_name = "Pirâmide";

    int yaw_deg = (int)roundf(g_cam_yaw * 180.0f / (float)M_PI) % 360;
    if (yaw_deg < 0) yaw_deg += 360;
    int pitch_deg = (int)roundf(g_cam_pitch * 180.0f / (float)M_PI);

    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len,
        "\033[1;33m[3D Bonfire]\033[0m %4.0fs | Madeira: \033[1;32m%s\033[0m | Fase: \033[1;37m%s\033[0m | Cam: %d°/%d°%s | [m] Modo | [Setas/WASD] Girar | [t] Turntable | [c] Colapsar | [q] Sair ",
        g_sim_time, mode_name, stage_name, yaw_deg, pitch_deg, g_auto_turntable ? " (AUTO)" : "");

    if (buf_len > 0) {
        safe_write(STDOUT_FILENO, buf, buf_len);
    }
}

static void handle_input(void) {
    char ch;
    while (read(STDIN_FILENO, &ch, 1) > 0) {
        if (ch == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                if (seq[0] == '[') {
                    if (seq[1] == 'A') { // Up arrow -> pitch up
                        g_cam_pitch += 0.06f;
                        if (g_cam_pitch > 1.25f) g_cam_pitch = 1.25f;
                    } else if (seq[1] == 'B') { // Down arrow -> pitch down
                        g_cam_pitch -= 0.06f;
                        if (g_cam_pitch < -0.15f) g_cam_pitch = -0.15f;
                    } else if (seq[1] == 'C') { // Right arrow -> yaw right
                        g_cam_yaw += 0.08f;
                    } else if (seq[1] == 'D') { // Left arrow -> yaw left
                        g_cam_yaw -= 0.08f;
                    }
                }
            }
        } else if (ch == 'a' || ch == 'A' || ch == 'h') {
            g_cam_yaw -= 0.08f;
        } else if (ch == 'd' || ch == 'D' || ch == 'l') {
            g_cam_yaw += 0.08f;
        } else if (ch == 'w' || ch == 'W' || ch == 'k') {
            g_cam_pitch += 0.06f;
            if (g_cam_pitch > 1.25f) g_cam_pitch = 1.25f;
        } else if (ch == 's' || ch == 'S' || ch == 'j') {
            g_cam_pitch -= 0.06f;
            if (g_cam_pitch < -0.15f) g_cam_pitch = -0.15f;
        } else if (ch == 'm' || ch == 'M') {
            g_stack_mode = (g_stack_mode + 1) % 3;
            init_scene();
        } else if (ch == 't' || ch == 'T') {
            g_auto_turntable = !g_auto_turntable;
        } else if (ch == '0' || ch == 'z' || ch == 'Z') {
            g_cam_yaw = 0.40f;
            g_cam_pitch = 0.35f;
            g_auto_turntable = false;
        } else if (ch == 'q' || ch == 'Q') {
            g_running = 0;
        } else if (ch == 'r' || ch == 'R') {
            init_scene();
        } else if (ch == 'c' || ch == 'C') {
            g_force_collapse = true;
        } else if (ch == ' ') {
            g_paused = !g_paused;
        } else if (ch == '+' || ch == '=') {
            g_time_scale *= 1.4f;
            if (g_time_scale > 10.0f) g_time_scale = 10.0f;
        } else if (ch == '-' || ch == '_') {
            g_time_scale /= 1.4f;
            if (g_time_scale < 0.2f) g_time_scale = 0.2f;
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--snapshot") == 0) {
        g_pixel_w = 120;
        g_pixel_h = 70;
        if (argc > 6) g_stack_mode = atoi(argv[6]);
        init_scene();
        float target_sim = (argc > 3) ? atof(argv[3]) : 0.0f;
        if (argc > 4) g_cam_yaw = atof(argv[4]) * (float)M_PI / 180.0f;
        if (argc > 5) g_cam_pitch = atof(argv[5]) * (float)M_PI / 180.0f;
        while (g_sim_time < target_sim) {
            update_simulation();
        }
        render_scene();
        FILE *f = fopen(argv[2], "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", g_pixel_w, g_pixel_h);
            for (int y = 0; y < g_pixel_h; y++) {
                for (int x = 0; x < g_pixel_w; x++) {
                    fputc(g_frame[y][x].color.r, f);
                    fputc(g_frame[y][x].color.g, f);
                    fputc(g_frame[y][x].color.b, f);
                }
            }
            fclose(f);
        }
        return 0;
    }

    setup_terminal();
    update_dimensions();
    init_scene();

    struct timespec ts;
    while (g_running) {
        if (g_resized) {
            g_resized = 0;
            update_dimensions();
            safe_write(STDOUT_FILENO, "\033[2J", 4);
        }

        handle_input();
        update_simulation();
        render_scene();
        present_frame();

        ts.tv_sec = 0;
        ts.tv_nsec = 24000000L; // ~40 FPS
        nanosleep(&ts, NULL);
    }

    return 0;
}
