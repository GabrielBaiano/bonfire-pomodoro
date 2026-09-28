#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_COLS 260
#define MAX_PIXEL_ROWS 180
#define MAX_SPARKS 256
#define MAX_ASH_FLAKES 128

typedef struct {
    uint8_t r, g, b;
} RGB;

// Transparent / Sky color (special marker: 0, 0, 0 with is_sky flag)
static const RGB COLOR_BLACK = {0, 0, 0};

// Curated 3D Pixel Art Wood Palette (Rich oak, bark, heartwood, cel-highlights)
static const RGB PALETTE_WOOD[] = {
    {24, 12, 6},      // 0: Dark Outline / Deep crevice
    {52, 28, 16},     // 1: Deep shadow bark
    {88, 48, 28},     // 2: Dark oak bark
    {132, 72, 38},    // 3: Mid bark / longitudinal grain
    {176, 102, 54},   // 4: Warm heartwood
    {220, 138, 72},   // 5: Firelit timber
    {252, 178, 96}    // 6: Golden rim highlight
};

// End-cap cut face with concentric tree rings
static const RGB PALETTE_ENDCAP[] = {
    {24, 12, 6},      // 0: Outline
    {110, 60, 32},    // 1: Dark ring
    {162, 94, 50},    // 2: Mid ring
    {210, 134, 76},   // 3: Light ring
    {250, 175, 105}   // 4: Firelit cut face
};

// Embers & Flames (Deep red -> Hot orange -> Bright yellow -> White hot core)
static const RGB PALETTE_EMBERS[] = {
    {140, 18, 5},     // 0: Deep red ember
    {225, 45, 10},    // 1: Bright red flame
    {255, 110, 18},   // 2: Hot orange flame
    {255, 200, 48},   // 3: Yellow incandescence
    {255, 255, 205}   // 4: White hot core
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
    Vec3 p1, p2;
    float radius;
    float wood_health;
    float ash_amount;
    Vec3 axis, dir, tangent, bitangent;
    float length;
} Cylinder3D;

typedef struct {
    float x, y;
    float vx, vy;
    int life;
    int max_life;
    RGB color;
    bool active;
} Spark;

typedef struct {
    float x, y;
    float vx, vy;
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

// Particles
static Spark g_sparks[MAX_SPARKS];
static AshFlake g_ash_flakes[MAX_ASH_FLAKES];

// 3D Logs
static Cylinder3D g_logs[4];

// Simulation dynamics
static float g_sim_time = 0.0f;
static float g_time_scale = 1.0f;
static float g_wind = 0.0f;
static float g_wind_target = 0.0f;
static bool g_paused = false;
static float g_collapse_progress = 0.0f;
static bool g_force_collapse = false;

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

static void init_cylinder(Cylinder3D *c, int id, Vec3 p1, Vec3 p2, float radius) {
    c->obj_id = id;
    c->p1 = p1;
    c->p2 = p2;
    c->radius = radius;
    c->wood_health = 100.0f;
    c->ash_amount = 0.0f;
    c->axis = vec3_sub(p2, p1);
    c->length = vec3_len(c->axis);
    c->dir = vec3_norm(c->axis);

    Vec3 ref_up = (Vec3){0.0f, 1.0f, 0.0f};
    if (fabsf(vec3_dot(ref_up, c->dir)) > 0.88f) {
        ref_up = (Vec3){1.0f, 0.0f, 0.0f};
    }
    c->tangent = vec3_norm(vec3_cross(c->dir, ref_up));
    c->bitangent = vec3_cross(c->dir, c->tangent);
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

    // Enter alternate screen, hide cursor
    safe_write(STDOUT_FILENO, "\033[?1049h\033[?25l\033[2J", 16);
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
    if (g_pixel_w < 30) g_pixel_w = 30;

    // 2 vertical pixels per text row, leaving 2 text rows for status info
    g_pixel_h = (g_term_rows - 2) * 2;
    if (g_pixel_h > MAX_PIXEL_ROWS) g_pixel_h = MAX_PIXEL_ROWS;
    if (g_pixel_h < 30) g_pixel_h = 30;
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

    // 3D Logs matching reference image irregular triangular teepee stack:
    // Log 1: Central-Left Diagonal Log
    init_cylinder(&g_logs[0], 1, (Vec3){-4.5f, -4.5f, 0.8f}, (Vec3){1.8f, 2.8f, -0.6f}, 1.45f);
    // Log 2: Right Leaning Log (leans over Log 1)
    init_cylinder(&g_logs[1], 2, (Vec3){6.0f, -4.2f, 0.4f}, (Vec3){1.2f, 3.4f, 0.3f}, 1.35f);
    // Log 3: Left Lower Branch
    init_cylinder(&g_logs[2], 3, (Vec3){-9.0f, -5.2f, 1.8f}, (Vec3){-3.8f, -4.2f, 1.1f}, 1.10f);
    // Log 4: Upright Left Fork (rising behind left flame)
    init_cylinder(&g_logs[3], 4, (Vec3){-3.8f, -1.2f, 2.2f}, (Vec3){-3.0f, 5.2f, 1.6f}, 1.00f);

    // Initial ignition spark in center kindling
    int cx = g_pixel_w / 2;
    int base_y = (int)(g_pixel_h * 0.82f);
    g_fire_heat[base_y][cx] = 0.45f;
}

static bool intersect_cylinder(const Cylinder3D *c, Vec3 ray_orig, Vec3 ray_dir,
                               float *out_t, Vec3 *out_pt, Vec3 *out_norm,
                               float *out_u, float *out_v, bool *out_endcap, float *out_rf) {
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt, best_norm;
    float best_u = 0, best_v = 0, best_rf = 1.0f;
    bool is_cap = false;

    // 1. Tube Body
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

    // 2. End-cap P1 (normal: -c->dir)
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

    // 3. End-cap P2 (normal: +c->dir)
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

static void spawn_spark(float x, float y, float vx, float vy, int life, RGB color) {
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!g_sparks[i].active) {
            g_sparks[i].x = x;
            g_sparks[i].y = y;
            g_sparks[i].vx = vx;
            g_sparks[i].vy = vy;
            g_sparks[i].life = life;
            g_sparks[i].max_life = life;
            g_sparks[i].color = color;
            g_sparks[i].active = true;
            break;
        }
    }
}

static void spawn_ash_flake(float x, float y) {
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (!g_ash_flakes[i].active) {
            g_ash_flakes[i].x = x;
            g_ash_flakes[i].y = y;
            g_ash_flakes[i].vx = (rand_f() - 0.5f) * 0.4f;
            g_ash_flakes[i].vy = 0.35f + rand_f() * 0.35f;
            g_ash_flakes[i].color = PALETTE_ASH[rand_range(1, 3)];
            g_ash_flakes[i].active = true;
            break;
        }
    }
}

static void update_simulation(void) {
    if (g_paused) return;

    g_sim_time += 0.045f * g_time_scale;

    // Wind dynamics
    if (rand_f() < 0.05f) {
        g_wind_target = (rand_f() - 0.5f) * 1.8f;
    }
    g_wind += (g_wind_target - g_wind) * 0.04f;

    // Combustion intensity across 7 stages
    float intensity = 0.0f;
    if (g_sim_time < 12.0f) {
        intensity = 0.15f + (g_sim_time / 12.0f) * 0.30f;
    } else if (g_sim_time < 35.0f) {
        intensity = 0.45f + ((g_sim_time - 12.0f) / 23.0f) * 0.55f;
    } else if (g_sim_time < 80.0f) {
        intensity = 1.0f; // Roaring peak
    } else if (g_sim_time < 120.0f) {
        intensity = 0.88f; // Ashening
    } else if (g_sim_time < 150.0f) {
        intensity = 0.65f; // Collapsing
    } else if (g_sim_time < 195.0f) {
        intensity = 0.35f; // Glowing embers
    } else {
        float fade = (g_sim_time - 195.0f) / 45.0f;
        intensity = fmaxf(0.0f, 0.22f - fade * 0.22f);
    }

    // Structural Collapse
    if ((g_sim_time > 110.0f || g_force_collapse) && g_collapse_progress < 1.0f) {
        g_collapse_progress += 0.015f * g_time_scale;
        if (g_collapse_progress > 1.0f) g_collapse_progress = 1.0f;

        // Shift 3D logs downward under gravity
        float fall = 0.018f * g_time_scale;
        g_logs[0].p2.y -= fall * 0.7f;
        g_logs[1].p2.y -= fall * 0.9f;
        g_logs[0].p2.x -= fall * 0.3f;
        g_logs[1].p2.x += fall * 0.3f;
        init_cylinder(&g_logs[0], 1, g_logs[0].p1, g_logs[0].p2, g_logs[0].radius);
        init_cylinder(&g_logs[1], 2, g_logs[1].p1, g_logs[1].p2, g_logs[1].radius);
    }

    int cx = g_pixel_w / 2;
    int base_y = (int)(g_pixel_h * 0.82f);
    float left_spire_x = cx - g_pixel_w * 0.12f;
    float right_spire_x = cx + g_pixel_w * 0.11f;

    // Base fire injection
    for (int x = 0; x < g_pixel_w; x++) {
        float dist = fabsf((float)(x - cx));
        float span = g_pixel_w * (0.24f + g_collapse_progress * 0.08f);
        if (dist <= span && intensity > 0.02f) {
            float prof = 1.0f - (dist / span);
            float base_h = prof * intensity * (0.85f + rand_f() * 0.30f);
            g_fire_heat[base_y][x] = fmaxf(g_fire_heat[base_y][x], base_h);
        } else {
            g_fire_heat[base_y][x] = 0.0f;
        }

        // White-hot core
        if (intensity > 0.30f && fabsf((float)(x - cx)) <= 3.5f) {
            g_fire_heat[base_y - 1][x] = fmaxf(g_fire_heat[base_y - 1][x], 1.0f * intensity);
        }

        // Twin spires roots
        if (intensity > 0.38f && g_collapse_progress < 0.70f) {
            if (fabsf(x - left_spire_x) <= 3.0f) {
                g_fire_heat[base_y - 1][x] = fmaxf(g_fire_heat[base_y - 1][x], 0.95f * intensity);
            }
            if (fabsf(x - right_spire_x) <= 3.2f) {
                g_fire_heat[base_y - 1][x] = fmaxf(g_fire_heat[base_y - 1][x], 1.0f * intensity);
            }
        }
    }

    // Convective fire propagation & twin spires dynamics
    for (int y = base_y - 1; y >= 2; y--) {
        float hr = (float)(base_y - y) / (base_y - 2);
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

            float decay = 0.025f + 0.035f * rand_f();
            float dl = fabsf(x - (left_spire_x + sinf(y * 0.18f + g_sim_time) * 1.5f));
            float dr = fabsf(x - (right_spire_x + sinf(y * 0.22f + g_sim_time) * 2.0f));
            float dc = fabsf((float)(x - cx));

            bool is_left = (dl < 5.5f) && (hr < 0.76f);
            bool is_right = (dr < 6.5f) && (hr < 0.94f); // Right spire is tallest!
            bool is_core = (dc < 4.2f) && (hr < 0.48f);

            if (is_left || is_right || is_core) {
                decay *= 0.50f;
            } else {
                decay *= 2.3f;
            }

            float val = fmaxf(0.0f, below - decay);
            g_next_fire[y][x] = val;

            // Z depth of flames for 3D interleaving
            if (is_left) g_fire_z[y][x] = 0.3f;
            else if (is_right) g_fire_z[y][x] = -0.3f;
            else g_fire_z[y][x] = 0.0f;
        }
    }

    for (int y = 2; y < base_y; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_heat[y][x] = g_next_fire[y][x];
        }
    }

    // Wood burning & flake detachment
    for (int i = 0; i < 4; i++) {
        if (intensity > 0.35f && g_logs[i].wood_health > 0.0f) {
            g_logs[i].wood_health -= 0.025f * g_time_scale;
            g_logs[i].ash_amount += 0.030f * g_time_scale;

            if (rand_f() < 0.12f && g_logs[i].ash_amount > 20.0f) {
                float t = rand_f();
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t));
                int px = (int)((p.x / 24.0f + 0.5f) * g_pixel_w);
                int py = (int)(((14.0f - p.y) / 14.0f) * g_pixel_h * 0.7f);
                if (px >= 0 && px < g_pixel_w && py >= 0 && py < g_pixel_h) {
                    spawn_ash_flake((float)px, (float)py);
                }
            }
        }
    }

    // Sparks off the spires
    if (intensity > 0.40f && rand_f() < 0.18f) {
        float sx = left_spire_x + (rand_f() - 0.5f) * 4.0f;
        float sy = base_y * 0.32f;
        spawn_spark(sx, sy, g_wind * 0.25f, -0.65f - rand_f() * 0.4f, rand_range(20, 40), PALETTE_EMBERS[1]);
    }
    if (intensity > 0.50f && rand_f() < 0.22f) {
        float sx = right_spire_x + (rand_f() - 0.5f) * 5.0f;
        float sy = base_y * 0.20f;
        spawn_spark(sx, sy, g_wind * 0.30f, -0.75f - rand_f() * 0.4f, rand_range(22, 45), PALETTE_EMBERS[2]);
    }

    // Update sparks
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!g_sparks[i].active) continue;
        g_sparks[i].x += g_sparks[i].vx;
        g_sparks[i].y += g_sparks[i].vy;
        g_sparks[i].life--;
        int ix = (int)roundf(g_sparks[i].x);
        int iy = (int)roundf(g_sparks[i].y);
        if (g_sparks[i].life <= 0 || ix < 0 || ix >= g_pixel_w || iy < 0 || iy >= g_pixel_h) {
            g_sparks[i].active = false;
        }
    }

    // Update ash flakes (Physical sand-like gravity & settling)
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (!g_ash_flakes[i].active) continue;
        g_ash_flakes[i].x += g_ash_flakes[i].vx + g_wind * 0.15f;
        g_ash_flakes[i].y += g_ash_flakes[i].vy;

        int ix = (int)roundf(g_ash_flakes[i].x);
        int iy = (int)roundf(g_ash_flakes[i].y);

        if (ix < 0 || ix >= g_pixel_w || iy >= g_pixel_h) {
            g_ash_flakes[i].active = false;
            continue;
        }

        // Settles on ground or on piled ash
        if (iy >= base_y || g_settled_ash[iy][ix]) {
            if (iy < g_pixel_h && ix >= 0 && ix < g_pixel_w) {
                g_settled_ash[iy][ix] = 1;
            }
            g_ash_flakes[i].active = false;
        }
    }
}

// 3D Scene Rendering with Per-Object Pixel Art Shader
static void render_scene(void) {
    memset(g_id_buf, 0, sizeof(g_id_buf));
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_depth_buf[y][x] = 1e9f;
            g_frame[y][x].is_sky = true;
            g_frame[y][x].color = COLOR_BLACK;
        }
    }

    float world_w = 24.0f;
    float world_h = 15.0f;
    Vec3 ray_dir = (Vec3){0.0f, 0.0f, 1.0f};
    Vec3 light_pos = (Vec3){0.0f, -1.8f, 0.0f};
    float light_intensity = 1.6f;

    // 1. Geometric Raycast Pass (3D Cylinders with Object-Space Snapping)
    for (int y = 0; y < g_pixel_h; y++) {
        float world_y = (((float)(g_pixel_h - 1 - y) / g_pixel_h) - 0.5f) * world_h;
        for (int x = 0; x < g_pixel_w; x++) {
            float world_x = (((float)x / g_pixel_w) - 0.5f) * world_w;
            Vec3 ray_orig = (Vec3){world_x, world_y, -40.0f};

            float closest_t = 1e9f;
            Vec3 hit_pt, hit_norm;
            float hit_u = 0, hit_v = 0, hit_rf = 1.0f;
            bool hit_cap = false;
            const Cylinder3D *hit_log = NULL;

            for (int i = 0; i < 4; i++) {
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
                g_depth_buf[y][x] = hit_pt.z; // Store Z depth for flame interleaving

                // 3D Point Light Calculation
                Vec3 l_vec = vec3_sub(light_pos, hit_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.10f * l_dist + 0.02f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(hit_norm, l_dir));
                float ambient = 0.28f + 0.12f * fmaxf(0.0f, hit_norm.y);
                float light_val = (ndotl * atten * light_intensity * 2.2f + ambient);

                if (hit_cap) {
                    // Tree Rings on End-Cap
                    int ring_q = (int)floorf(hit_rf * 7.0f);
                    int ring_band = ring_q % 2;
                    int col_idx = 1 + ring_band + ((light_val > 0.75f) ? 1 : 0);
                    if (col_idx > 4) col_idx = 4;
                    g_shade_buf[y][x] = PALETTE_ENDCAP[col_idx];
                } else {
                    // Object-Space Longitudinal Bark Grain (Zero Pixel Creep)
                    float num_u = 20.0f;
                    float num_v = hit_log->length * 4.0f;
                    float u_q = floorf(hit_u * num_u) / num_u;
                    float v_q = floorf(hit_v * num_v) / num_v;

                    float grain = sinf(u_q * 42.0f + sinf(v_q * 10.0f) * 1.5f) * 0.5f + 0.5f;
                    float fissure = sinf(u_q * 20.0f + v_q * 8.0f) * 0.5f + 0.5f;
                    float bark_mod = 0.85f + 0.35f * grain;

                    bool is_crack = (fissure > 0.75f) && (l_dist < 5.8f);

                    if (hit_log->ash_amount > 40.0f) {
                        int ash_idx = (int)(light_val * bark_mod * 3.2f);
                        if (ash_idx < 0) ash_idx = 0;
                        if (ash_idx > 4) ash_idx = 4;
                        g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                    } else if (is_crack) {
                        int emb_idx = (int)(light_val * 3.6f);
                        if (emb_idx < 0) emb_idx = 0;
                        if (emb_idx > 4) emb_idx = 4;
                        g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                    } else {
                        int band = (int)(light_val * bark_mod * 3.6f);
                        if (band < 1) band = 1;
                        if (band > 6) band = 6;
                        g_shade_buf[y][x] = PALETTE_WOOD[band];
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
                    } else if (fabsf(g_depth_buf[y][x] - g_depth_buf[ny][nx]) > 0.8f) {
                        is_edge = true;
                        break;
                    }
                }
            }

            g_frame[y][x].is_sky = false;
            if (is_edge) {
                g_frame[y][x].color = PALETTE_WOOD[0]; // Dark outline
            } else {
                g_frame[y][x].color = g_shade_buf[y][x];
            }
        }
    }

    // 3. Volumetric Fire Layer with 3D Depth Interleaving
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            float heat = g_fire_heat[y][x];
            if (heat > 0.10f) {
                RGB fcol;
                if (heat > 0.82f) fcol = PALETTE_EMBERS[4];
                else if (heat > 0.60f) fcol = PALETTE_EMBERS[3];
                else if (heat > 0.38f) fcol = PALETTE_EMBERS[2];
                else if (heat > 0.22f) fcol = PALETTE_EMBERS[1];
                else fcol = PALETTE_EMBERS[0];

                int log_id = g_id_buf[y][x];
                if (log_id > 0) {
                    float fz = g_fire_z[y][x];
                    float lz = g_depth_buf[y][x];
                    if (fz < lz || heat > 0.72f) {
                        // Flame licks in front of log
                        g_frame[y][x].color = fcol;
                        g_frame[y][x].is_sky = false;
                    }
                } else {
                    g_frame[y][x].color = fcol;
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

    // 5. Falling Ash Flakes
    for (int i = 0; i < MAX_ASH_FLAKES; i++) {
        if (!g_ash_flakes[i].active) continue;
        int ix = (int)roundf(g_ash_flakes[i].x);
        int iy = (int)roundf(g_ash_flakes[i].y);
        if (ix >= 0 && ix < g_pixel_w && iy >= 0 && iy < g_pixel_h) {
            g_frame[iy][ix].color = g_ash_flakes[i].color;
            g_frame[iy][ix].is_sky = false;
        }
    }

    // 6. Sparks
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (!g_sparks[i].active) continue;
        int ix = (int)roundf(g_sparks[i].x);
        int iy = (int)roundf(g_sparks[i].y);
        if (ix >= 0 && ix < g_pixel_w && iy >= 0 && iy < g_pixel_h) {
            g_frame[iy][ix].color = g_sparks[i].color;
            g_frame[iy][ix].is_sky = false;
        }
    }
}

// Flicker-free ANSI rendering using Unicode half-blocks '▀'
static void present_frame(void) {
    char buf[131072];
    int buf_len = 0;

    // Cursor to top-left
    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[H");

    int prev_fg_r = -1, prev_fg_g = -1, prev_fg_b = -1;
    int prev_bg_r = -1, prev_bg_g = -1, prev_bg_b = -1;
    bool prev_bg_transp = true;

    int text_rows = g_pixel_h / 2;
    for (int tr = 0; tr < text_rows; tr++) {
        int y_top = tr * 2;
        int y_bot = tr * 2 + 1;

        for (int x = 0; x < g_pixel_w; x++) {
            Pixel top = g_frame[y_top][x];
            Pixel bot = g_frame[y_bot][x];

            // Case A: Both are sky -> transparent space
            if (top.is_sky && bot.is_sky) {
                if (!prev_bg_transp) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[49m");
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf[buf_len++] = ' ';
                continue;
            }

            // Case B: Top is sky, Bottom has color -> lower half block
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
                // Unicode lower half block: ▄ (\xE2\x96\x84)
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "▄");
                continue;
            }

            // Case C: Top has color, Bottom is sky -> upper half block with transparent BG
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
                // Unicode upper half block: ▀ (\xE2\x96\x80)
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "▀");
                continue;
            }

            // Case D: Both have colors -> Top = FG, Bot = BG, Character = '▀'
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

            // Flush buffer chunk if getting close to capacity
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

    // Status bar on bottom line
    const char *stage_name = "1/7: Gravetos e Ignição";
    if (g_sim_time > 195.0f) stage_name = "7/7: Cinzas Frias";
    else if (g_sim_time > 150.0f) stage_name = "6/7: Leito de Brasas";
    else if (g_sim_time > 120.0f) stage_name = "5/7: Colapso das Toras";
    else if (g_sim_time > 80.0f) stage_name = "4/7: Madeira em Cinza";
    else if (g_sim_time > 35.0f) stage_name = "3/7: Fogueira Roaring";
    else if (g_sim_time > 12.0f) stage_name = "2/7: Chamas nas Toras";

    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len,
        "\033[1;33m[3D Pixel Art Bonfire]\033[0m %4.0fs | Fase: \033[1;37m%s\033[0m | [r] Reiniciar | [c] Colapsar | [+/-] Vel | [q] Sair ",
        g_sim_time, stage_name);

    if (buf_len > 0) {
        safe_write(STDOUT_FILENO, buf, buf_len);
    }
}

static void handle_input(void) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == 'q' || ch == 'Q') {
                g_running = 0;
            } else if (ch == 'r' || ch == 'R') {
                init_scene(); g_sim_time = 50.0f;
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
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--snapshot") == 0) {
        g_pixel_w = 120;
        g_pixel_h = 70;
        init_scene(); g_sim_time = 50.0f;
        for (int i = 0; i < 160; i++) update_simulation();
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

        // Target ~30-40 FPS
        ts.tv_sec = 0;
        ts.tv_nsec = 28000000L;
        nanosleep(&ts, NULL);
    }

    return 0;
}
