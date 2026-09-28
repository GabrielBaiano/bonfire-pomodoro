/*
 * 3D Per-Object Pixel Art Bonfire Simulation (C Edition)
 *
 * Techniques:
 * - 3D Spherical Orbit Camera (Yaw θ, Pitch φ) with free real-time rotation
 * - 3 Physical Wood Stacking Modes (Log Cabin, Teepee, Pyramid) with contact physics
 * - Discrete Object-Space Bark Plates (Zero Pixel Creep, NO orange tiger stripes)
 * - Concentric Growth Rings on Cut End-Caps
 * - Localized Charring & Crevice Embers (only on burning surfaces)
 * - 1-Pixel Cel-Art Outlines via G-Buffer Discontinuity
 * - Dynamic Half-Lambert Point-Light Wrap Illumination
 * - 3D Convective Fire Interleaving with Depth Buffer
 * - Falling Sand Ash Flakes & Rising 3D Sparks
 * - ANSI 24-bit TrueColor Half-Block Character Output ('▀', '▄')
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

typedef struct {
    uint8_t r, g, b;
} RGB;

static const RGB COLOR_BLACK = {0, 0, 0};

// Curated Natural Oak & Pine Bark Palette (No artificial orange stripes!)
static const RGB PALETTE_WOOD[] = {
    {20, 12, 8},      // 0: Dark Outline / Deep crevice
    {42, 27, 18},     // 1: Deep shadow bark (raw umber)
    {72, 48, 32},     // 2: Dark weathered oak
    {105, 72, 48},    // 3: Mid oak bark
    {138, 96, 64},    // 4: Warm dry timber fiber
    {168, 120, 82},   // 5: Muted wood highlight
    {200, 145, 102}   // 6: Warm firelit rim (natural, NOT neon orange!)
};

// Charcoal and Carbonized Bark
static const RGB PALETTE_CHARRED[] = {
    {16, 12, 12},     // 0: Deep black crevice
    {30, 26, 26},     // 1: Charred black bark
    {52, 48, 48},     // 2: Dark charcoal
    {80, 72, 68},     // 3: Burnt ash bark
    {115, 110, 108}   // 4: Light ash surface
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
    {48, 45, 52},     // 0: Charcoal crust
    {88, 84, 94},     // 1: Dark ash
    {135, 132, 142},  // 2: Mid ash grey
    {185, 182, 192},  // 3: Light chalky ash
    {230, 228, 235}   // 4: White ash powder
};

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    int obj_id;
    Vec3 p1, p2;                       // Current geometry
    Vec3 p1_orig, p2_orig;             // Initial stack geometry
    Vec3 p1_collapsed, p2_collapsed;   // Physical collapse target under gravity
    float radius;
    float charred;                     // Susceptibility to burning/charring
    float wood_health;
    float ash_amount;
    Vec3 axis, dir, tangent, bitangent;
    float length;
} Cylinder3D;

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

// Static ash grid for settled sand heap
static uint8_t g_settled_ash[MAX_PIXEL_ROWS][MAX_COLS];

// 3D Particles
static Spark g_sparks[MAX_SPARKS];
static AshFlake g_ash_flakes[MAX_ASH_FLAKES];

// 3D Logs & Stacking Modes
static Cylinder3D g_logs[MAX_LOGS];
static int g_num_logs = 6;
static int g_stack_mode = 0; // 0: Log Cabin (Fogueira Quadrada), 1: Teepee (Tenda Cônica), 2: Pyramid (Lean-to)

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
                  (Vec3){-span, y2_coll, -4.6f}, (Vec3){-span, y2_coll, 4.6f},
                  r2, 0.40f);
    init_cylinder(&g_logs[3], 4,
                  (Vec3){ span, y2, -5.0f}, (Vec3){ span, y2, 5.0f},
                  (Vec3){ span, y2_coll, -4.6f}, (Vec3){ span, y2_coll, 4.6f},
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
        // Collapses inward toward ground
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
    memset(g_settled_ash, 0, sizeof(g_settled_ash));
    memset(g_sparks, 0, sizeof(g_sparks));
    memset(g_ash_flakes, 0, sizeof(g_ash_flakes));

    g_sim_time = 0.0f;
    g_collapse_progress = 0.0f;
    g_force_collapse = false;

    if (g_stack_mode == 0) {
        build_stack_log_cabin();
    } else if (g_stack_mode == 1) {
        build_stack_teepee();
    } else {
        build_stack_pyramid();
    }

    int cx = g_pixel_w / 2;
    int cradle_y = (int)(g_pixel_h * 0.72f);
    g_fire_heat[cradle_y][cx] = 0.65f;
    g_fire_heat[cradle_y][cx - 1] = 0.50f;
    g_fire_heat[cradle_y][cx + 1] = 0.50f;
}

static bool intersect_cylinder(const Cylinder3D *c, Vec3 ray_orig, Vec3 ray_dir,
                               float *out_t, Vec3 *out_pt, Vec3 *out_norm,
                               float *out_u, float *out_v, bool *out_endcap, float *out_rf) {
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt, best_norm;
    float best_u = 0, best_v = 0, best_rf = 1.0f;
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

                    best_t = t;
                    best_pt = pt;
                    best_norm = norm;
                    best_u = u;
                    best_v = v;
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

    // 7-Stage Combustion Intensity Curve
    float intensity = 1.0f;
    if (g_sim_time < 12.0f) {
        intensity = 0.20f + (g_sim_time / 12.0f) * 0.35f;
    } else if (g_sim_time < 35.0f) {
        intensity = 0.55f + ((g_sim_time - 12.0f) / 23.0f) * 0.45f;
    } else if (g_sim_time < 85.0f) {
        intensity = 1.0f; // Peak Roaring Fire
    } else if (g_sim_time < 125.0f) {
        intensity = 0.88f; // Ashening
    } else if (g_sim_time < 155.0f) {
        intensity = 0.65f; // Structural Collapse
    } else if (g_sim_time < 200.0f) {
        intensity = 0.35f; // Ember Bed
    } else {
        float fade = (g_sim_time - 200.0f) / 45.0f;
        intensity = fmaxf(0.0f, 0.22f - fade * 0.22f); // Cold Ash Mound
    }

    // Physical Rotational Gravity Collapse around grounded base pivots!
    if ((g_sim_time > 115.0f || g_force_collapse) && g_collapse_progress < 1.0f) {
        g_collapse_progress += 0.015f * g_time_scale;
        if (g_collapse_progress > 1.0f) g_collapse_progress = 1.0f;

        float c = g_collapse_progress;
        for (int i = 0; i < g_num_logs; i++) {
            g_logs[i].p1.x = g_logs[i].p1_orig.x * (1.0f - c) + g_logs[i].p1_collapsed.x * c;
            g_logs[i].p1.y = g_logs[i].p1_orig.y * (1.0f - c) + g_logs[i].p1_collapsed.y * c;
            g_logs[i].p1.z = g_logs[i].p1_orig.z * (1.0f - c) + g_logs[i].p1_collapsed.z * c;

            g_logs[i].p2.x = g_logs[i].p2_orig.x * (1.0f - c) + g_logs[i].p2_collapsed.x * c;
            g_logs[i].p2.y = g_logs[i].p2_orig.y * (1.0f - c) + g_logs[i].p2_collapsed.y * c;
            g_logs[i].p2.z = g_logs[i].p2_orig.z * (1.0f - c) + g_logs[i].p2_collapsed.z * c;
            recompute_cylinder_axes(&g_logs[i]);
        }
    }

    // Compute Camera Vectors for 3D world projection
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

    // Projected screen positions for 3D fire emitters
    Vec3 left_spire_w = (Vec3){-1.2f, -1.6f, 0.4f};
    Vec3 right_spire_w = (Vec3){1.0f, -1.6f, -0.2f};
    Vec3 core_w = (Vec3){0.0f, -1.8f, 0.0f};

    Vec3 rel_l = vec3_sub(left_spire_w, cam_pos);
    Vec3 rel_r = vec3_sub(right_spire_w, cam_pos);
    Vec3 rel_c = vec3_sub(core_w, cam_pos);

    float left_spire_x = ((vec3_dot(rel_l, right) / world_w) + 0.5f) * g_pixel_w;
    float right_spire_x = ((vec3_dot(rel_r, right) / world_w) + 0.5f) * g_pixel_w;
    float cx = ((vec3_dot(rel_c, right) / world_w) + 0.5f) * g_pixel_w;

    int cradle_y = (int)((0.5f - (vec3_dot(rel_c, up) / world_h)) * g_pixel_h);
    if (cradle_y < 10) cradle_y = 10;
    if (cradle_y > g_pixel_h - 4) cradle_y = g_pixel_h - 4;

    // =========================================================================
    // FIRE INJECTION: Emanates from the BURNING WOOD & CRADLE
    // =========================================================================
    // 1. Heat emitted from contact surfaces of the burning logs
    for (int i = 0; i < g_num_logs; i++) {
        if (intensity < 0.15f) continue;
        int num_pts = (int)(g_logs[i].length * 6.0f);
        for (int s = 0; s < num_pts; s++) {
            float t = (float)s / num_pts;
            Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t));

            float dist_to_c = sqrtf(p.x * p.x + (p.y + 1.8f) * (p.y + 1.8f) + p.z * p.z);
            if (dist_to_c < 4.5f) {
                Vec3 p_rel = vec3_sub(p, cam_pos);
                int gx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
                int gy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
                if (gx >= 0 && gx < g_pixel_w && gy >= 0 && gy < g_pixel_h) {
                    float wood_fire = (0.92f - (dist_to_c / 4.5f) * 0.25f) * intensity;
                    g_fire_heat[gy][gx] = fmaxf(g_fire_heat[gy][gx], wood_fire);
                    g_fire_z[gy][gx] = vec3_dot(p_rel, fwd) - 0.25f;
                }
            }
        }
    }

    // 2. Glowing white-hot ember bed nestled inside the cradle
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -14; dx <= 14; dx++) {
            float d = sqrtf((dx * 0.75f) * (dx * 0.75f) + (dy * 1.8f) * (dy * 1.8f));
            if (d < 12.0f) {
                float h = (1.0f - powf(d / 12.0f, 1.8f)) * intensity;
                int gx = (int)cx + dx;
                int gy = cradle_y + dy;
                if (gx >= 0 && gx < g_pixel_w && gy >= 0 && gy < g_pixel_h) {
                    g_fire_heat[gy][gx] = fmaxf(g_fire_heat[gy][gx], h);
                    g_fire_z[gy][gx] = vec3_dot(rel_c, fwd) - 0.4f;
                }
            }
        }
    }

    // 3. Convective flame propagation: ROARING SPIRES
    for (int y = cradle_y; y >= 2; y--) {
        float hr = (float)(cradle_y - y) / cradle_y;
        for (int x = 0; x < g_pixel_w; x++) {
            int src_x = x;
            int wind_step = (g_wind > 0.35f) ? 1 : ((g_wind < -0.35f) ? -1 : 0);
            int jitter = (xorshift32() % 3) - 1;
            src_x += (rand_f() < 0.40f) ? wind_step : jitter;
            if (src_x < 0) src_x = 0;
            if (src_x >= g_pixel_w) src_x = g_pixel_w - 1;

            float below = g_fire_heat[y + 1][src_x];
            if (below <= 0.03f) {
                g_next_fire[y][x] = 0.0f;
                continue;
            }

            float decay = 0.020f + 0.028f * rand_f();
            float dl = fabsf((float)(x - (left_spire_x + sinf(y * 0.14f + g_sim_time) * 1.8f)));
            float dr = fabsf((float)(x - (right_spire_x + sinf(y * 0.18f + g_sim_time) * 2.2f)));
            float dc = fabsf((float)(x - cx));

            bool is_left = (dl < 8.0f) && (hr < 0.78f);
            bool is_right = (dr < 9.5f) && (hr < 0.94f);
            bool is_core = (dc < 6.5f) && (hr < 0.52f);

            if (is_left || is_right || is_core) {
                decay *= 0.48f;
            } else {
                decay *= 2.6f;
            }

            float val = fmaxf(0.0f, below - decay);
            g_next_fire[y][x] = val;

            if (is_left) g_fire_z[y][x] = vec3_dot(rel_l, fwd) - 0.2f;
            else if (is_right) g_fire_z[y][x] = vec3_dot(rel_r, fwd) - 0.2f;
            else g_fire_z[y][x] = vec3_dot(rel_c, fwd) - 0.45f;
        }
    }

    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_heat[y][x] = g_next_fire[y][x];
        }
    }

    // 4. Wood Degradation & Ash Formation
    for (int i = 0; i < g_num_logs; i++) {
        if (intensity > 0.35f && g_logs[i].wood_health > 0.0f) {
            g_logs[i].wood_health -= 0.025f * g_time_scale;
            g_logs[i].ash_amount += 0.030f * g_time_scale;

            if (rand_f() < 0.12f && g_logs[i].ash_amount > 20.0f) {
                float t = rand_f();
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t));
                p.y += g_logs[i].radius * 0.9f;
                Vec3 vel = (Vec3){(rand_f() - 0.5f) * 0.6f + g_wind * 0.4f, -(rand_f() * 0.5f + 0.2f), (rand_f() - 0.5f) * 0.6f};
                spawn_ash_3d(p, vel, PALETTE_ASH[rand_range(1, 3)]);
            }
        }
    }

    // 5. 3D Sparks Ejection
    if (intensity > 0.30f && rand_f() < 0.70f) {
        Vec3 spark_p = (Vec3){(rand_f() - 0.5f) * 2.5f, -1.6f, (rand_f() - 0.5f) * 2.5f};
        Vec3 spark_v = (Vec3){(rand_f() - 0.5f) * 1.5f + g_wind * 1.2f, rand_f() * 3.2f + 2.0f, (rand_f() - 0.5f) * 1.5f};
        RGB spark_col = PALETTE_EMBERS[rand_range(2, 4)];
        spawn_spark_3d(spark_p, spark_v, rand_range(16, 42), spark_col);
    }

    for (int i = 0; i < MAX_SPARKS; i++) {
        if (g_sparks[i].active) {
            g_sparks[i].pos.x += g_sparks[i].vel.x * 0.05f;
            g_sparks[i].pos.y += g_sparks[i].vel.y * 0.05f;
            g_sparks[i].pos.z += g_sparks[i].vel.z * 0.05f;
            g_sparks[i].vel.y -= 0.04f; // gravity deceleration
            g_sparks[i].life--;
            if (g_sparks[i].life <= 0 || g_sparks[i].pos.y < -4.5f) {
                g_sparks[i].active = false;
            }
        }
    }

    // 6. 3D Ash Flakes drift & accumulation on ground
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (g_ash_flakes[i].active) {
            g_ash_flakes[i].pos.x += g_ash_flakes[i].vel.x * 0.05f + sinf(g_ash_flakes[i].pos.y * 2.0f + g_sim_time) * 0.02f;
            g_ash_flakes[i].pos.y += g_ash_flakes[i].vel.y * 0.05f;
            g_ash_flakes[i].pos.z += g_ash_flakes[i].vel.z * 0.05f;

            if (g_ash_flakes[i].pos.y <= -4.15f) {
                Vec3 p_rel = vec3_sub(g_ash_flakes[i].pos, cam_pos);
                int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
                int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
                if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                    g_settled_ash[sy][sx] = 1;
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
    // 1. 3D Raycasting with Object-Space Bark Plates (No orange stripes!)
    // =========================================================================
    for (int y = 0; y < g_pixel_h; y++) {
        float wy = (((g_pixel_h - 1 - y) / (float)g_pixel_h) - 0.5f) * world_h;
        for (int x = 0; x < g_pixel_w; x++) {
            float wx = ((x / (float)g_pixel_w) - 0.5f) * world_w;

            Vec3 ray_orig = vec3_add(cam_pos, vec3_add(vec3_scale(right, wx), vec3_scale(up, wy)));
            Vec3 ray_dir = fwd;

            float closest_t = 1e9f;
            Vec3 hit_pt = {0,0,0}, hit_norm = {0,1,0};
            float hit_u = 0, hit_v = 0, hit_rf = 1.0f;
            bool hit_cap = false;
            Cylinder3D *hit_log = NULL;

            for (int i = 0; i < g_num_logs; i++) {
                float t, u, v, rf;
                Vec3 pt, norm;
                bool is_cap;
                if (intersect_cylinder(&g_logs[i], ray_orig, ray_dir, &t, &pt, &norm, &u, &v, &is_cap, &rf)) {
                    if (t < closest_t) {
                        closest_t = t;
                        hit_pt = pt;
                        hit_norm = norm;
                        hit_u = u;
                        hit_v = v;
                        hit_cap = is_cap;
                        hit_rf = rf;
                        hit_log = &g_logs[i];
                    }
                }
            }

            if (hit_log != NULL) {
                g_id_buf[y][x] = hit_log->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(hit_pt, cam_pos), fwd);

                // Dynamic fire point light with wrap-lighting
                Vec3 l_vec = vec3_sub(light_pos, hit_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                
                // Wrap lighting allows curved front of logs to catch radiant fire warmth
                float ndotl = fmaxf(0.0f, (vec3_dot(hit_norm, l_dir) + 0.45f) / 1.45f);
                float ambient = 0.28f + 0.12f * fmaxf(0.0f, hit_norm.y);
                float light_val = (ndotl * atten * light_intensity * 2.4f + ambient);

                if (hit_cap) {
                    // Tree Rings on End-Cap
                    float r_q = floorf(hit_rf * 8.0f) / 8.0f;
                    int ring_band = ((int)(r_q * 8.0f)) % 2;
                    int col_idx = 1 + ring_band + ((hit_rf > 0.70f) ? 1 : 0) + ((light_val > 0.80f) ? 1 : 0);
                    if (col_idx > 4) col_idx = 4;
                    g_shade_buf[y][x] = PALETTE_ENDCAP[col_idx];
                } else {
                    // Discrete object-space bark plates (Zero Pixel Creep, no tiger stripes)
                    float num_plates_u = 14.0f;
                    float num_plates_v = hit_log->length * 2.2f;
                    float u_plate = floorf(hit_u * num_plates_u);
                    float v_plate = floorf(hit_v * num_plates_v);

                    // Subtle pseudo-random plate shade variation
                    float plate_hash = sinf(u_plate * 12.9898f + v_plate * 78.233f) * 43758.5453f;
                    float plate_var = (plate_hash - floorf(plate_hash)) * 0.25f - 0.12f;

                    // Discrete furrow edge test (plate boundaries are dark crevices)
                    float u_frac = (hit_u * num_plates_u) - u_plate;
                    float v_frac = (hit_v * num_plates_v) - v_plate;
                    bool is_furrow = (u_frac < 0.12f || u_frac > 0.88f || (v_frac < 0.08f && ((int)u_plate % 2 == 0)));

                    // Local heat charring (based on distance to fire core and log charred factor)
                    float dist_to_core = sqrtf(hit_pt.x * hit_pt.x + (hit_pt.y + 1.8f) * (hit_pt.y + 1.8f) + hit_pt.z * hit_pt.z);
                    float heat_exposure = fmaxf(0.0f, 1.0f - dist_to_core / 4.8f) * hit_log->charred;

                    if (hit_log->ash_amount > 40.0f) {
                        int ash_idx = (int)((light_val + plate_var) * 2.2f);
                        if (ash_idx < 0) ash_idx = 0;
                        if (ash_idx > 4) ash_idx = 4;
                        g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                    } else if (heat_exposure > 0.45f) {
                        // Charred alligator bark with glowing ember in deep crevices
                        if (is_furrow && heat_exposure > 0.60f) {
                            int emb_idx = (int)(heat_exposure * 3.5f);
                            if (emb_idx < 0) emb_idx = 0;
                            if (emb_idx > 3) emb_idx = 3;
                            g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                        } else {
                            int c_idx = (int)((light_val + plate_var) * 2.2f);
                            if (c_idx < 0) c_idx = 0;
                            if (c_idx > 4) c_idx = 4;
                            g_shade_buf[y][x] = PALETTE_CHARRED[c_idx];
                        }
                    } else if (is_furrow) {
                        g_shade_buf[y][x] = PALETTE_WOOD[0]; // Dark crevice
                    } else {
                        // Natural weathered wood tone
                        int b_idx = (int)((light_val + plate_var) * 2.8f);
                        if (b_idx < 1) b_idx = 1;
                        if (b_idx > 6) b_idx = 6;
                        g_shade_buf[y][x] = PALETTE_WOOD[b_idx];
                    }
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
                g_frame[y][x].color = PALETTE_WOOD[0]; // Dark outline
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

    // 4. Settled Ash Heap
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            if (g_settled_ash[y][x]) {
                g_frame[y][x].color = PALETTE_ASH[((x + y) % 2 == 0) ? 2 : 3];
                g_frame[y][x].is_sky = false;
            }
        }
    }

    // 5. Ash Flakes Falling
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
        if (argc > 3) g_sim_time = atof(argv[3]);
        if (argc > 4) g_cam_yaw = atof(argv[4]) * (float)M_PI / 180.0f;
        if (argc > 5) g_cam_pitch = atof(argv[5]) * (float)M_PI / 180.0f;
        for (int i = 0; i < 40; i++) update_simulation();
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
