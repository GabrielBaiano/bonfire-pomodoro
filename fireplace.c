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
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_COLS 260
#define MAX_PIXEL_ROWS 180
#define MAX_SPARKS 256
#define MAX_ASH_FLAKES 128
#define MAX_SMOKE 80
#define MAX_RAIN 160
#define MAX_LOGS 16
#define MAX_STONES 24
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

// Ash & Charcoal — warm earth tones that read as accumulated burnt material
// Center stays hot dark charcoal, outer rim transitions to chalky dusty ash
static const RGB PALETTE_ASH[] = {
    {28, 22, 18},     // 0: Near-black charcoal crust (inner bed)
    {58, 50, 40},     // 1: Dark warm charcoal-brown
    {108, 96, 80},    // 2: Medium ashy brown-grey
    {165, 152, 132},  // 3: Warm pale ash (buff/sandy)
    {215, 208, 195}   // 4: Chalky white ash powder (top surface)
};

// Natural Hearth Earth & Dirt Soil Palette (Dark organic loam, dry gravel, earth)
static const RGB PALETTE_DIRT[] = {
    {24, 18, 14},     // 0: Deep shadow earth
    {45, 32, 22},     // 1: Moist loam soil
    {72, 52, 36},     // 2: Weathered dry dirt
    {102, 76, 52},    // 3: Warm sandy earth
    {138, 104, 72}    // 4: Firelit ground surface
};

// Foliage & Leaf Palette (Fresh leaf green -> dry autumn yellow-brown -> charred soot)
static const RGB PALETTE_LEAF[] = {
    {28, 45, 16},     // 0: Dark leaf shadow / underside
    {54, 92, 26},     // 1: Forest green leaf blade
    {88, 142, 36},    // 2: Vibrant sunlit green leaf
    {142, 115, 42},   // 3: Dry curing yellow-brown
    {86, 52, 24},     // 4: Crisp autumn brown
    {20, 15, 12}      // 5: Carbonized black leaf soot
};

// Volumetric Steam & Smoke Palettes
static const RGB PALETTE_STEAM[] = {
    {125, 135, 148},  // 0: Deep steam shadow
    {168, 178, 192},  // 1: Mid cool steam
    {212, 220, 230}   // 2: Bright vapor highlight
};

static const RGB PALETTE_SMOKE[] = {
    {36, 40, 48},     // 0: Dark soot smoke
    {58, 64, 76},     // 1: Mid blue-grey pyrolytic smoke
    {88, 94, 110}     // 2: Light billowing smoke
};

static const RGB PALETTE_SOOT_PUFF[] = {
    {16, 14, 14},     // 0: Black soot
    {28, 25, 25},     // 1: Charcoal dust
    {46, 42, 42}      // 2: Ash puff
};

static const RGB COLOR_RAIN = {165, 190, 220};

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    float temp;            // Thermal state [0.0 = 20C ambient, 1.0 = 900C peak]
    float moisture;        // Moisture content [0.0 = dry, 0.18 = 18% water content]
    float burn_progress;   // Pyrolysis progress [0.0 = fresh, 0.35 = char, 0.75 = ash, 1.0 = consumed]
    float structural_mass; // Mechanical strength [1.0 = solid, 0.0 = broken]
    float glow_intensity;  // Oxygen & wind-fed coal incandescence [0.0, 1.0]
} LogSegment;

typedef struct {
    int obj_id;
    Vec3 p1, p2;                       // Current dynamic endpoints
    Vec3 p1_orig, p2_orig;             // Initial stack geometry
    Vec3 p1_collapsed, p2_collapsed;   // Physical collapse target under gravity
    float radius;
    float charred;
    float wood_health;
    float ash_amount;
    Vec3 axis, dir, tangent, bitangent;
    float length;
    LogSegment segments[NUM_LOG_SEGS];
    float collapse_cur;                // Current individual collapse progression [0.0, 1.0]
    float collapse_speed;              // Settling downward velocity
    int support_log1, support_log2;    // Log IDs that support this log (-1 = ground)
    float sag_amount;                  // Center bowing deflection under gravity
    float roll_angle;                  // Tipping angle as resting supports burn
    bool snapped;                      // True if center fiber has structurally fractured
    bool fractured;                    // True if split into two sub-cylinders
    float fracture_prog;               // Progress of split displacement [0.0, 1.0]
    Vec3 break_p;                      // Dynamic break point position
    Vec3 break_p_orig;                 // Initial break point
    Vec3 break_p_target;               // Final settled break point
} Cylinder3D;

typedef struct {
    int obj_id;
    Vec3 center;
    Vec3 u_tan;      // Tangent axis along the ring perimeter
    Vec3 v_up;       // Vertical axis (up)
    Vec3 w_rad;      // Radial axis (outward)
    float ru, rv, rw;// Semi-axes: tangential width, height, radial depth
    float shade_var; // Individual rock color tone variation
} Stone3D;

#define OBJ_ASH_BED 500

typedef struct {
    Vec3 center;
    float radius_xz;
    float height;
    float heat;
    float volume;
} AshBed3D;

#define MAX_TWIGS 8
#define MAX_LEAVES 6
#define OBJ_TWIG_BASE 600
#define OBJ_LEAF_BASE 700

typedef struct {
    int obj_id;
    Vec3 p1, p2;
    float radius;
    float length;
    Vec3 dir, tangent, bitangent;
    float temp;
    float moisture;
    float burn_progress;
    bool active;
} Twig3D;

typedef struct {
    int obj_id;
    Vec3 pos;
    Vec3 normal;
    float rx, ry;   // Leaf dimensions
    Vec3 u_dir, v_dir;
    float temp;
    float burn_progress;
    bool active;
} Leaf3D;

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
    Vec3 pos;
    Vec3 vel;
    float size;
    float life;
    float max_life;
    int type; // 0: Steam, 1: Wood smoke, 2: Soot puff
    bool active;
} SmokeParticle;

typedef struct {
    Vec3 pos;
    float speed;
    float length;
    bool active;
} RainDrop;

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
static float g_burnt_mass = 0.0f;     // Total fractional burned mass across hearth (0.0 to 1.0)

// 3D Particles
static Spark g_sparks[MAX_SPARKS];
static AshFlake g_ash_flakes[MAX_ASH_FLAKES];
static SmokeParticle g_smoke[MAX_SMOKE];
static RainDrop g_rain[MAX_RAIN];

// 3D Logs & Stacking Modes
static Cylinder3D g_logs[MAX_LOGS];
static int g_num_logs = 6;
static int g_stack_mode = 0; // 0: Log Cabin (Fogueira Quadrada), 1: Teepee (Tenda Cônica), 2: Pyramid (Lean-to)

// 3D Kindling Twigs & Dry Leaves (Gravetos e Folhas)
static Twig3D g_twigs[MAX_TWIGS];
static int g_num_twigs = 6;
static Leaf3D g_leaves[MAX_LEAVES];
static int g_num_leaves = 5;

// 3D Stone Fire Ring (Base da Fogueira)
static Stone3D g_stones[MAX_STONES];
static int g_num_stones = 10;

// 3D Camera State
static float g_cam_yaw = 0.40f;       // Horizontal orbit angle (radians, ~23 deg)
static float g_cam_pitch = 0.35f;    // Elevation angle (radians, ~20 deg)
static bool g_auto_turntable = false; // Auto 360 degree turntable rotation

// Simulation dynamics (Physically dimensionalized in real seconds)
static float g_sim_time = 0.0f;       // Physical simulated time in seconds
static float g_anim_time = 0.0f;      // Display animation time (flicker, ash drift)
static float g_time_scale = 1.0f;     // 1.0 = Realtime, 30.0 = Fast Demo
static bool g_realtime_mode = true;   // Realtime 1.0x vs Fast Demo 30.0x
static bool g_show_hud = true;        // Display status header and physical clock
static float g_wind = 0.0f;
static float g_wind_target = 0.0f;
static float g_wind_turb = 0.0f;
static bool g_rain_active = false;
static float g_collapse_progress = 0.0f;
static bool g_force_collapse = false;
static bool g_paused = false;

// Audio Engine State
static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;
static volatile bool g_audio_mute = false;
static volatile float g_audio_fire_heat = 0.0f;
static volatile float g_audio_steam_rate = 0.0f;
static volatile int g_audio_pop_trigger = 0;
static volatile float g_audio_rain_intensity = 0.0f;

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
    c->collapse_cur = 0.0f;
    c->collapse_speed = 0.0f;
    c->support_log1 = -1;
    c->support_log2 = -1;
    c->sag_amount = 0.0f;
    c->roll_angle = 0.0f;
    c->snapped = false;
    c->fractured = false;
    c->fracture_prog = 0.0f;
    c->break_p = vec3_scale(vec3_add(p1, p2), 0.5f);
    c->break_p_orig = c->break_p;
    c->break_p_target = (Vec3){ c->break_p.x, -4.2f + radius, c->break_p.z };
    for (int s = 0; s < NUM_LOG_SEGS; s++) {
        c->segments[s].temp = 0.0f;
        c->segments[s].moisture = 0.18f; // 18% moisture content
        c->segments[s].burn_progress = 0.0f;
        c->segments[s].structural_mass = 1.0f;
        c->segments[s].glow_intensity = 0.0f;
    }
    recompute_cylinder_axes(c);
}

static void init_twig(Twig3D *tw, int id, Vec3 p1, Vec3 p2, float radius) {
    tw->obj_id = id;
    tw->p1 = p1;
    tw->p2 = p2;
    tw->radius = radius;
    Vec3 axis = vec3_sub(p2, p1);
    tw->length = vec3_len(axis);
    tw->dir = vec3_norm(axis);
    Vec3 ref_up = (Vec3){0.0f, 1.0f, 0.0f};
    if (fabsf(vec3_dot(ref_up, tw->dir)) > 0.88f) ref_up = (Vec3){1.0f, 0.0f, 0.0f};
    tw->tangent = vec3_norm(vec3_cross(tw->dir, ref_up));
    tw->bitangent = vec3_cross(tw->dir, tw->tangent);
    tw->temp = 0.0f;
    tw->moisture = 0.10f; // Thin twigs dry quickly
    tw->burn_progress = 0.0f;
    tw->active = true;
}

static void init_leaf(Leaf3D *lf, int id, Vec3 pos, Vec3 normal, float rx, float ry) {
    lf->obj_id = id;
    lf->pos = pos;
    lf->normal = vec3_norm(normal);
    lf->rx = rx;
    lf->ry = ry;
    Vec3 ref_up = (Vec3){0.0f, 1.0f, 0.0f};
    if (fabsf(vec3_dot(ref_up, lf->normal)) > 0.88f) ref_up = (Vec3){1.0f, 0.0f, 0.0f};
    lf->u_dir = vec3_norm(vec3_cross(lf->normal, ref_up));
    lf->v_dir = vec3_cross(lf->normal, lf->u_dir);
    lf->temp = 0.0f;
    lf->burn_progress = 0.0f;
    lf->active = true;
}

static void build_kindling(void) {
    g_num_twigs = 6;
    g_num_leaves = 5;

    // Criss-crossing kindling twigs in the hearth core on the dirt floor
    init_twig(&g_twigs[0], OBJ_TWIG_BASE + 0, (Vec3){-1.5f, -4.10f, -0.6f}, (Vec3){ 1.6f, -3.85f,  0.4f}, 0.28f);
    init_twig(&g_twigs[1], OBJ_TWIG_BASE + 1, (Vec3){-0.5f, -4.05f, -1.5f}, (Vec3){ 0.4f, -3.70f,  1.4f}, 0.25f);
    init_twig(&g_twigs[2], OBJ_TWIG_BASE + 2, (Vec3){-1.2f, -4.10f,  0.8f}, (Vec3){ 1.3f, -3.55f, -0.7f}, 0.30f);
    init_twig(&g_twigs[3], OBJ_TWIG_BASE + 3, (Vec3){ 1.4f, -4.05f, -0.9f}, (Vec3){-0.9f, -3.65f,  1.1f}, 0.26f);
    init_twig(&g_twigs[4], OBJ_TWIG_BASE + 4, (Vec3){ 0.2f, -3.75f,  0.3f}, (Vec3){-0.7f, -3.20f, -0.4f}, 0.22f);
    init_twig(&g_twigs[5], OBJ_TWIG_BASE + 5, (Vec3){-0.4f, -3.80f, -0.3f}, (Vec3){ 0.9f, -3.35f,  0.5f}, 0.24f);

    // Green foliage leaves attached to twigs
    init_leaf(&g_leaves[0], OBJ_LEAF_BASE + 0, (Vec3){ 0.7f, -3.55f,  0.45f}, (Vec3){ 0.2f, 0.9f,  0.3f}, 0.28f, 0.45f);
    init_leaf(&g_leaves[1], OBJ_LEAF_BASE + 1, (Vec3){-0.8f, -3.45f, -0.35f}, (Vec3){-0.3f, 0.8f, -0.4f}, 0.26f, 0.40f);
    init_leaf(&g_leaves[2], OBJ_LEAF_BASE + 2, (Vec3){ 1.0f, -3.40f, -0.55f}, (Vec3){ 0.4f, 0.8f, -0.3f}, 0.25f, 0.42f);
    init_leaf(&g_leaves[3], OBJ_LEAF_BASE + 3, (Vec3){-0.3f, -3.60f,  0.65f}, (Vec3){-0.2f, 0.9f,  0.3f}, 0.24f, 0.38f);
    init_leaf(&g_leaves[4], OBJ_LEAF_BASE + 4, (Vec3){ 0.1f, -3.25f, -0.25f}, (Vec3){ 0.1f, 0.9f, -0.2f}, 0.26f, 0.40f);
}

static void* audio_synth_thread(void *arg) {
    (void)arg;
    FILE *pipe = popen("aplay -q -f S16_LE -r 44100 -c 1 2>/dev/null || pw-play --format=s16le --rate=44100 --channels=1 2>/dev/null || paplay --raw --format=s16le --rate=44100 --channels=1 2>/dev/null", "w");

    #define AUDIO_CHUNK 512
    int16_t pcm_buf[AUDIO_CHUNK];

    float roar_lp = 0.0f;
    float roar_phase = 0.0f;
    float steam_hp = 0.0f;
    float steam_bp = 0.0f;

    struct {
        float phase;
        float freq;
        float amp;
        float decay;
        bool active;
    } snaps[4];
    memset(snaps, 0, sizeof(snaps));

    uint32_t a_rng = 0x12345678;

    while (g_audio_running) {
        if (!pipe || g_audio_mute) {
            struct timespec req = {0, 11600000L}; // ~11.6ms for 512 samples
            nanosleep(&req, NULL);
            if (!g_audio_running) break;
            continue;
        }

        float heat = g_audio_fire_heat;
        float steam_target = g_audio_steam_rate;
        float rain_intensity = g_audio_rain_intensity;
        int pop_trig = g_audio_pop_trigger;
        if (pop_trig > 0) {
            g_audio_pop_trigger = 0;
            for (int i = 0; i < 4 && pop_trig > 0; i++) {
                if (!snaps[i].active) {
                    a_rng ^= a_rng << 13; a_rng ^= a_rng >> 17; a_rng ^= a_rng << 5;
                    float rf = (a_rng & 0xFFFF) / 65536.0f;
                    snaps[i].active = true;
                    snaps[i].phase = 0.0f;
                    snaps[i].freq = 300.0f + rf * 700.0f;
                    snaps[i].amp = 0.45f + rf * 0.35f;
                    snaps[i].decay = 0.00012f + rf * 0.00015f;
                    pop_trig--;
                }
            }
        }

        if (heat > 0.15f) {
            a_rng ^= a_rng << 13; a_rng ^= a_rng >> 17; a_rng ^= a_rng << 5;
            float rf = (a_rng & 0xFFFF) / 65536.0f;
            if (rf < (0.0010f * heat)) {
                for (int i = 0; i < 4; i++) {
                    if (!snaps[i].active) {
                        snaps[i].active = true;
                        snaps[i].phase = 0.0f;
                        snaps[i].freq = 350.0f + rf * 850.0f;
                        snaps[i].amp = 0.22f + rf * 0.30f;
                        snaps[i].decay = 0.00015f + rf * 0.00020f;
                        break;
                    }
                }
            }
        }

        for (int n = 0; n < AUDIO_CHUNK; n++) {
            a_rng ^= a_rng << 13; a_rng ^= a_rng >> 17; a_rng ^= a_rng << 5;
            float white = ((int32_t)(a_rng & 0xFFFF) - 32768) / 32768.0f;

            // 1. Low Convection Roar (Brownian / Lowpass)
            float lp_coeff = 0.010f + 0.018f * heat;
            roar_lp += (white - roar_lp) * lp_coeff;
            roar_phase += 2.0f * (float)M_PI * 0.8f / 44100.0f;
            if (roar_phase > 2.0f * (float)M_PI) roar_phase -= 2.0f * (float)M_PI;
            float roar_mod = 0.85f + 0.15f * sinf(roar_phase);
            float roar_out = roar_lp * heat * 0.55f * roar_mod;

            // 2. Steam Sizzle / Hiss (Bandpass ~2-4 kHz)
            steam_hp += (white - steam_hp) * 0.40f;
            float high_sig = white - steam_hp;
            steam_bp += (high_sig - steam_bp) * 0.18f;
            float steam_out = steam_bp * fminf(1.0f, steam_target + rain_intensity * 0.40f) * 0.28f;

            // 3. Rain Drizzle
            float rain_out = 0.0f;
            if (rain_intensity > 0.01f) {
                rain_out = white * 0.035f * rain_intensity;
            }

            // 4. Snaps / Crackles
            float snap_out = 0.0f;
            for (int i = 0; i < 4; i++) {
                if (snaps[i].active) {
                    snaps[i].phase += 2.0f * (float)M_PI * snaps[i].freq / 44100.0f;
                    float s_sig = (sinf(snaps[i].phase) + (white * 0.35f)) * snaps[i].amp;
                    snap_out += s_sig;
                    snaps[i].amp -= snaps[i].amp * snaps[i].decay * 44100.0f / 512.0f;
                    if (snaps[i].amp <= 0.005f) {
                        snaps[i].active = false;
                    }
                }
            }

            float mix = roar_out + steam_out + rain_out + snap_out;
            if (mix > 1.0f) mix = 1.0f;
            if (mix < -1.0f) mix = -1.0f;
            pcm_buf[n] = (int16_t)(mix * 32000.0f);
        }

        size_t written = fwrite(pcm_buf, sizeof(int16_t), AUDIO_CHUNK, pipe);
        if (written < AUDIO_CHUNK) {
            struct timespec req = {0, 5000000L};
            nanosleep(&req, NULL);
        } else {
            fflush(pipe);
        }
    }

    if (pipe) {
        pclose(pipe);
    }
    return NULL;
}

static void start_audio_engine(void) {
    if (!g_audio_running) {
        g_audio_running = true;
        pthread_create(&g_audio_thread, NULL, audio_synth_thread, NULL);
    }
}

static void stop_audio_engine(void) {
    if (g_audio_running) {
        g_audio_running = false;
        pthread_join(g_audio_thread, NULL);
    }
}

static void safe_write(int fd, const void *buf, size_t count) {
    ssize_t ret = write(fd, buf, count);
    (void)ret;
}

static void reset_terminal(void) {
    stop_audio_engine();
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
    g_num_stones = 16;
    float ring_r = 6.8f;
    for (int i = 0; i < g_num_stones; i++) {
        float base_angle = i * (2.0f * (float)M_PI / (float)g_num_stones);
        float angle = base_angle + 0.09f * sinf(i * 2.3f + 1.2f);
        float r_dist = ring_r + 0.35f * sinf(i * 3.7f + 0.5f);

        // Anisotropic dimensions: flattened and elongated along ring perimeter
        float ru = 1.45f + 0.20f * sinf(i * 4.1f);  // Tangential width (interlocking)
        float rv = 0.78f + 0.15f * cosf(i * 2.7f);  // Height (flattened boulder)
        float rw = 1.10f + 0.18f * sinf(i * 5.3f);  // Radial depth

        Vec3 c = {
            r_dist * cosf(angle),
            ground_y + rv * 0.82f + 0.06f * sinf(i * 1.9f),
            r_dist * sinf(angle)
        };

        // Orthonormal frame for oriented ellipsoid
        Vec3 u = (Vec3){ -sinf(angle), 0.0f, cosf(angle) }; // Tangent along ring
        Vec3 v = (Vec3){ 0.0f, 1.0f, 0.0f };                // Vertical up
        Vec3 w = (Vec3){ cosf(angle), 0.0f, sinf(angle) };  // Radial outward

        g_stones[i].obj_id = 100 + i;
        g_stones[i].center = c;
        g_stones[i].u_tan = u;
        g_stones[i].v_up = v;
        g_stones[i].w_rad = w;
        g_stones[i].ru = ru;
        g_stones[i].rv = rv;
        g_stones[i].rw = rw;
        g_stones[i].shade_var = 0.12f * sinf(i * 4.8f + 2.1f);
    }
}

static bool intersect_stone(const Stone3D *st, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    Vec3 oc = vec3_sub(ro, st->center);
    float inv_u = 1.0f / st->ru;
    float inv_v = 1.0f / st->rv;
    float inv_w = 1.0f / st->rw;

    // Project ray into stone's local unit-sphere coordinates
    Vec3 p0 = (Vec3){
        vec3_dot(oc, st->u_tan) * inv_u,
        vec3_dot(oc, st->v_up)  * inv_v,
        vec3_dot(oc, st->w_rad) * inv_w
    };
    Vec3 d0 = (Vec3){
        vec3_dot(rd, st->u_tan) * inv_u,
        vec3_dot(rd, st->v_up)  * inv_v,
        vec3_dot(rd, st->w_rad) * inv_w
    };

    float a = vec3_dot(d0, d0);
    float b = vec3_dot(p0, d0);
    float c = vec3_dot(p0, p0) - 1.0f;
    float disc = b * b - a * c;
    if (disc < 0.0f) return false;

    float sdisc = sqrtf(disc);
    float t = (-b - sdisc) / a;
    if (t < 0.1f) t = (-b + sdisc) / a;
    if (t < 0.1f) return false;

    *out_t = t;
    Vec3 hit_p = vec3_add(ro, vec3_scale(rd, t));
    *out_pt = hit_p;

    // Normal in local ellipsoid space
    Vec3 loc_p = vec3_add(p0, vec3_scale(d0, t));
    Vec3 loc_n = (Vec3){ loc_p.x * inv_u, loc_p.y * inv_v, loc_p.z * inv_w };
    Vec3 world_n = (Vec3){
        loc_n.x * st->u_tan.x + loc_n.y * st->v_up.x + loc_n.z * st->w_rad.x,
        loc_n.x * st->u_tan.y + loc_n.y * st->v_up.y + loc_n.z * st->w_rad.y,
        loc_n.x * st->u_tan.z + loc_n.y * st->v_up.z + loc_n.z * st->w_rad.z
    };
    *out_norm = vec3_norm(world_n);
    return true;
}

static bool intersect_twig(const Twig3D *tw, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    if (!tw->active || tw->burn_progress >= 1.0f) return false;
    Vec3 delta_p = vec3_sub(ro, tw->p1);
    Vec3 d_proj = vec3_sub(rd, vec3_scale(tw->dir, vec3_dot(rd, tw->dir)));
    Vec3 dp_proj = vec3_sub(delta_p, vec3_scale(tw->dir, vec3_dot(delta_p, tw->dir)));
    float a = vec3_dot(d_proj, d_proj);
    if (a < 1e-6f) return false;
    float b = 2.0f * vec3_dot(d_proj, dp_proj);
    float cv = vec3_dot(dp_proj, dp_proj) - tw->radius * tw->radius;
    float disc = b * b - 4.0f * a * cv;
    if (disc < 0.0f) return false;
    float sdisc = sqrtf(disc);
    float t0 = (-b - sdisc) / (2.0f * a);
    float t1 = (-b + sdisc) / (2.0f * a);
    float t = (t0 > 0.1f) ? t0 : t1;
    if (t <= 0.1f) return false;
    Vec3 pt = vec3_add(ro, vec3_scale(rd, t));
    if (pt.y < -4.24f) return false; // Occluded by dirt ground
    float h = vec3_dot(vec3_sub(pt, tw->p1), tw->dir);
    if (h < 0.0f || h > tw->length) return false;
    Vec3 axis_pt = vec3_add(tw->p1, vec3_scale(tw->dir, h));
    Vec3 d_axis = vec3_sub(pt, axis_pt);
    *out_t = t;
    *out_pt = pt;
    *out_norm = vec3_norm(d_axis);
    return true;
}

static bool intersect_leaf(const Leaf3D *lf, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    if (!lf->active || lf->burn_progress >= 0.95f) return false;
    float denom = vec3_dot(rd, lf->normal);
    if (fabsf(denom) < 1e-5f) return false;
    float t = vec3_dot(vec3_sub(lf->pos, ro), lf->normal) / denom;
    if (t < 0.1f) return false;
    Vec3 pt = vec3_add(ro, vec3_scale(rd, t));
    if (pt.y < -4.24f) return false;
    Vec3 rel = vec3_sub(pt, lf->pos);
    float u = vec3_dot(rel, lf->u_dir) / lf->rx;
    float v = vec3_dot(rel, lf->v_dir) / lf->ry;
    if (u * u + v * v > 1.0f) return false; // Elliptical leaf disc
    *out_t = t;
    *out_pt = pt;
    *out_norm = (denom < 0.0f) ? lf->normal : vec3_scale(lf->normal, -1.0f);
    return true;
}

// -----------------------------------------------------------------------------
// STACK GENERATORS: Real physical contact points, resting tiers, and gravity
// -----------------------------------------------------------------------------

// 1. FOGUEIRA QUADRADA / CABANA DE TRONCOS (Log Cabin / Cribbing)
static void build_stack_log_cabin(void) {
    g_num_logs = 6;
    float ground_y = -4.2f;
    float span = 3.6f;

    // Tier 1 (Ground along X): 2 logs resting flat on ground with unequal radii and asymmetric cuts
    float r0 = 1.32f;
    float y0 = ground_y + r0;
    init_cylinder(&g_logs[0], 1,
                  (Vec3){-4.8f, y0, -span - 0.15f}, (Vec3){5.3f, y0, -span + 0.15f},
                  (Vec3){-4.8f, y0, -span - 0.15f}, (Vec3){5.3f, y0, -span + 0.15f},
                  r0, 0.25f);
    g_logs[0].support_log1 = -1;
    g_logs[0].support_log2 = -1;

    float r1 = 1.18f;
    float y1 = ground_y + r1;
    init_cylinder(&g_logs[1], 2,
                  (Vec3){-5.2f, y1,  span + 0.20f}, (Vec3){4.6f, y1,  span - 0.10f},
                  (Vec3){-5.2f, y1,  span + 0.20f}, (Vec3){4.6f, y1,  span - 0.10f},
                  r1, 0.25f);
    g_logs[1].support_log1 = -1;
    g_logs[1].support_log2 = -1;

    // Tier 2 (Resting across Tier 1 along Z): 2 logs
    float r2 = 1.10f;
    float y2 = ground_y + fmaxf(r0, r1) + r2 - 0.15f;
    float y2_coll = ground_y + r2;
    init_cylinder(&g_logs[2], 3,
                  (Vec3){-span - 0.20f, y2, -5.1f}, (Vec3){-span + 0.15f, y2, 4.7f},
                  (Vec3){-span - 0.10f, y2_coll, -4.6f}, (Vec3){-span + 0.10f, y2_coll, 4.3f},
                  r2, 0.35f);
    g_logs[2].support_log1 = 0; // Supported by log 0
    g_logs[2].support_log2 = 1; // Supported by log 1

    float r3 = 1.26f;
    float y3 = ground_y + fmaxf(r0, r1) + r3 - 0.15f;
    float y3_coll = ground_y + r3;
    init_cylinder(&g_logs[3], 4,
                  (Vec3){ span - 0.10f, y3, -4.6f}, (Vec3){ span + 0.25f, y3, 5.3f},
                  (Vec3){ span - 0.05f, y3_coll, -4.2f}, (Vec3){ span + 0.15f, y3_coll, 4.8f},
                  r3, 0.35f);
    g_logs[3].support_log1 = 0; // Supported by log 0
    g_logs[3].support_log2 = 1; // Supported by log 1

    // Tier 3 (Cross diagonally across top): 2 logs
    float r4 = 0.92f;
    float y4 = y2 + r2 + r4 - 0.15f;
    float y4_coll = ground_y + r4 + 0.35f;
    init_cylinder(&g_logs[4], 5,
                  (Vec3){-3.8f, y4, -3.2f}, (Vec3){4.2f, y4, 2.7f},
                  (Vec3){-2.2f, y4_coll, -1.6f}, (Vec3){2.4f, y4_coll, 1.4f},
                  r4, 0.50f);
    g_logs[4].support_log1 = 2; // Supported by Tier 2
    g_logs[4].support_log2 = 3;

    float r5 = 1.04f;
    float y5 = y3 + r3 + r5 - 0.15f;
    float y5_coll = ground_y + r5 + 0.30f;
    init_cylinder(&g_logs[5], 6,
                  (Vec3){-4.3f, y5,  2.8f}, (Vec3){3.9f, y5, -3.1f},
                  (Vec3){-2.4f, y5_coll,  1.5f}, (Vec3){2.2f, y5_coll, -1.6f},
                  r5, 0.50f);
    g_logs[5].support_log1 = 2; // Supported by Tier 2
    g_logs[5].support_log2 = 3;
}

// 2. TENDA CÔNICA (Teepee / Cone)
static void build_stack_teepee(void) {
    g_num_logs = 5;
    float ground_y = -4.2f;
    float apex_r = 0.65f;
    float apex_y = 2.4f;

    static const float radii[5] = { 1.08f, 1.25f, 1.14f, 1.22f, 1.05f };
    static const float angle_offsets[5] = { 0.30f, 1.58f, 2.75f, 4.02f, 5.35f };

    for (int i = 0; i < 5; i++) {
        float angle = angle_offsets[i];
        float r = radii[i];
        float contact_r = 4.30f + 0.18f * sinf(i * 3.1f);
        Vec3 p1 = { contact_r * cosf(angle), ground_y - 0.45f, contact_r * sinf(angle) };
        Vec3 p2 = { apex_r * cosf(angle) + 0.08f * sinf(i * 1.5f), apex_y + 0.12f * cosf(i * 2.0f), apex_r * sinf(angle) };
        Vec3 p2_coll = { apex_r * 0.35f * cosf(angle), ground_y + r * 0.70f, apex_r * 0.35f * sinf(angle) };

        init_cylinder(&g_logs[i], i + 1, p1, p2, p1, p2_coll, r, 0.45f);
        g_logs[i].support_log1 = -1;
        g_logs[i].support_log2 = -1;
    }
}

// 3. PIRÂMIDE COM ESCORA (Pyramid / Lean-to)
static void build_stack_pyramid(void) {
    g_num_logs = 5;
    float ground_y = -4.2f;
    float r_base0 = 1.38f;
    float r_base1 = 1.26f;
    float y1_0 = ground_y + r_base0;
    float y1_1 = ground_y + r_base1;

    // 2 Base logs (slight asymmetry in length and position)
    init_cylinder(&g_logs[0], 1,
                  (Vec3){-5.4f, y1_0, -2.4f}, (Vec3){5.6f, y1_0, -2.6f},
                  (Vec3){-5.4f, y1_0, -2.4f}, (Vec3){5.6f, y1_0, -2.6f},
                  r_base0, 0.25f);
    g_logs[0].support_log1 = -1;
    g_logs[0].support_log2 = -1;

    init_cylinder(&g_logs[1], 2,
                  (Vec3){-5.6f, y1_1,  2.6f}, (Vec3){5.2f, y1_1,  2.4f},
                  (Vec3){-5.6f, y1_1,  2.6f}, (Vec3){5.2f, y1_1,  2.4f},
                  r_base1, 0.25f);
    g_logs[1].support_log1 = -1;
    g_logs[1].support_log2 = -1;

    // 3 Leaning logs with distinct thicknesses
    float y_apex = 2.2f;
    float r_cross2 = 1.08f;
    float r_cross3 = 1.15f;
    float r_cross4 = 1.12f;

    float y_coll2 = ground_y + r_cross2 + 0.2f;
    init_cylinder(&g_logs[2], 3,
                  (Vec3){-4.2f, y1_0 + r_base0 - 0.2f, -1.1f}, (Vec3){-0.1f, y_apex + 0.1f, -0.2f},
                  (Vec3){-3.8f, y_coll2, -0.6f}, (Vec3){-0.1f, y_coll2, -0.1f},
                  r_cross2, 0.50f);
    g_logs[2].support_log1 = 0;
    g_logs[2].support_log2 = -1;

    float y_coll3 = ground_y + r_cross3 + 0.2f;
    init_cylinder(&g_logs[3], 4,
                  (Vec3){ 4.3f, y1_0 + r_base0 - 0.2f, -0.9f}, (Vec3){ 0.1f, y_apex, -0.3f},
                  (Vec3){ 3.9f, y_coll3, -0.6f}, (Vec3){ 0.1f, y_coll3, -0.1f},
                  r_cross3, 0.50f);
    g_logs[3].support_log1 = 0;
    g_logs[3].support_log2 = -1;

    float y_coll4 = ground_y + r_cross4 + 0.2f;
    init_cylinder(&g_logs[4], 5,
                  (Vec3){ 0.2f, y1_1 + r_base1 - 0.2f,  3.1f}, (Vec3){ 0.0f, y_apex + 0.15f, 0.4f},
                  (Vec3){ 0.1f, y_coll4,  2.3f}, (Vec3){ 0.0f, y_coll4,  0.2f},
                  r_cross4, 0.50f);
    g_logs[4].support_log1 = 1;
    g_logs[4].support_log2 = -1;
}

static void init_scene(void) {
    memset(g_fire_heat, 0, sizeof(g_fire_heat));
    memset(g_next_fire, 0, sizeof(g_next_fire));
    memset(g_fire_z, 0, sizeof(g_fire_z));
    memset(g_sparks, 0, sizeof(g_sparks));
    memset(g_ash_flakes, 0, sizeof(g_ash_flakes));
    memset(g_smoke, 0, sizeof(g_smoke));
    memset(g_rain, 0, sizeof(g_rain));

    g_ash_bed.center = (Vec3){0.0f, -4.2f, 0.0f};
    g_ash_bed.radius_xz = 4.8f;
    g_ash_bed.height = 0.35f;
    g_ash_bed.heat = 0.35f;
    g_ash_bed.volume = 0.0f;
    g_burnt_mass = 0.0f;

    g_sim_time = 0.0f;
    g_anim_time = 0.0f;
    g_collapse_progress = 0.0f;
    g_force_collapse = false;

    build_stone_ring();
    build_kindling();

    if (g_stack_mode == 0) {
        build_stack_log_cabin();
    } else if (g_stack_mode == 1) {
        build_stack_teepee();
    } else {
        build_stack_pyramid();
    }
}

static bool intersect_sub_cylinder(Vec3 p1, Vec3 p2, float radius, float sag_amount,
                                   Vec3 ray_orig, Vec3 ray_dir,
                                   float *out_t, Vec3 *out_pt, Vec3 *out_norm,
                                   float *out_u, float *out_v, bool *out_endcap, float *out_rf,
                                   bool test_cap1, bool test_cap2) {
    Vec3 axis = vec3_sub(p2, p1);
    float length = vec3_len(axis);
    if (length < 1e-4f) return false;
    Vec3 dir = vec3_scale(axis, 1.0f / length);

    Vec3 ref_up = (Vec3){0.0f, 1.0f, 0.0f};
    if (fabsf(vec3_dot(ref_up, dir)) > 0.88f) {
        ref_up = (Vec3){1.0f, 0.0f, 0.0f};
    }
    Vec3 tangent = vec3_norm(vec3_cross(dir, ref_up));
    Vec3 bitangent = vec3_cross(dir, tangent);

    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float best_u = 0.0f, best_v = 0.0f, best_rf = 1.0f;
    bool is_cap = false;

    int max_passes = (sag_amount > 0.01f) ? 2 : 1;
    for (int pass = 0; pass < max_passes; pass++) {
        Vec3 ro = ray_orig;
        if (pass == 1) ro.y += sag_amount * 0.75f;

        Vec3 delta_p = vec3_sub(ro, p1);
        Vec3 d_proj = vec3_sub(ray_dir, vec3_scale(dir, vec3_dot(ray_dir, dir)));
        Vec3 dp_proj = vec3_sub(delta_p, vec3_scale(dir, vec3_dot(delta_p, dir)));

        float a = vec3_dot(d_proj, d_proj);
        if (a > 1e-6f) {
            float b = 2.0f * vec3_dot(d_proj, dp_proj);
            float cv = vec3_dot(dp_proj, dp_proj) - radius * radius;
            float disc = b * b - 4.0f * a * cv;
            if (disc >= 0.0f) {
                float sdisc = sqrtf(disc);
                float t0 = (-b - sdisc) / (2.0f * a);
                float t1 = (-b + sdisc) / (2.0f * a);
                float t = (t0 > 0.1f) ? t0 : t1;
                if (t > 0.1f && t < best_t) {
                    Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
                    float h = vec3_dot(vec3_sub(pt, p1), dir);
                    if (h >= 0.0f && h <= length) {
                        float v = h / length;
                        float sag_y = (sag_amount > 0.01f) ? (4.0f * v * (1.0f - v) * sag_amount) : 0.0f;
                        Vec3 axis_pt = vec3_add(p1, vec3_scale(dir, h));
                        axis_pt.y -= sag_y;

                        Vec3 d_axis = vec3_sub(pt, axis_pt);
                        float dist_to_axis = vec3_len(d_axis);
                        if (dist_to_axis <= radius * 1.15f) {
                            if (pt.y < -4.24f) continue;
                            Vec3 norm = vec3_scale(d_axis, 1.0f / (dist_to_axis + 1e-6f));
                            float angle = atan2f(vec3_dot(norm, bitangent), vec3_dot(norm, tangent));
                            float u = (angle + (float)M_PI) / (2.0f * (float)M_PI);

                            best_t = t;
                            best_pt = pt;
                            best_norm = norm;
                            best_u = u;
                            best_v = v;
                            is_cap = false;
                            hit = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (test_cap1) {
        float denom1 = vec3_dot(ray_dir, vec3_scale(dir, -1.0f));
        if (fabsf(denom1) > 1e-5f) {
            float t = vec3_dot(vec3_sub(p1, ray_orig), vec3_scale(dir, -1.0f)) / denom1;
            if (t > 0.1f && t < best_t) {
                Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
                float r = vec3_len(vec3_sub(pt, p1));
                if (r <= radius && pt.y >= -4.24f) {
                    best_t = t;
                    best_pt = pt;
                    best_norm = vec3_scale(dir, -1.0f);
                    best_u = 0.0f;
                    best_v = 0.0f;
                    best_rf = r / radius;
                    is_cap = true;
                    hit = true;
                }
            }
        }
    }

    if (test_cap2) {
        float denom2 = vec3_dot(ray_dir, dir);
        if (fabsf(denom2) > 1e-5f) {
            float t = vec3_dot(vec3_sub(p2, ray_orig), dir) / denom2;
            if (t > 0.1f && t < best_t) {
                Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
                float r = vec3_len(vec3_sub(pt, p2));
                if (r <= radius) {
                    best_t = t;
                    best_pt = pt;
                    best_norm = dir;
                    best_u = 0.0f;
                    best_v = 1.0f;
                    best_rf = r / radius;
                    is_cap = true;
                    hit = true;
                }
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

static bool intersect_cylinder(const Cylinder3D *c, Vec3 ray_orig, Vec3 ray_dir,
                               float *out_t, Vec3 *out_pt, Vec3 *out_norm,
                               float *out_u, float *out_v, bool *out_endcap, float *out_rf,
                               int *out_seg_idx) {
    if (!c->fractured) {
        float t, u, v, rf;
        Vec3 pt, norm;
        bool is_cap;
        if (intersect_sub_cylinder(c->p1, c->p2, c->radius, c->sag_amount, ray_orig, ray_dir,
                                   &t, &pt, &norm, &u, &v, &is_cap, &rf, true, true)) {
            int seg = (int)(v * NUM_LOG_SEGS);
            if (seg < 0) seg = 0;
            if (seg >= NUM_LOG_SEGS) seg = NUM_LOG_SEGS - 1;
            *out_t = t; *out_pt = pt; *out_norm = norm;
            *out_u = u; *out_v = v; *out_endcap = is_cap;
            *out_rf = rf; *out_seg_idx = seg;
            return true;
        }
        return false;
    }

    // Fractured cylinder: test both sub-halves
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float best_u = 0.0f, best_v = 0.0f, best_rf = 1.0f;
    bool best_cap = false;
    int best_seg = 0;

    // Sub A: p1 -> break_p (segments 0..4)
    float tA, uA, vA, rfA;
    Vec3 ptA, normA;
    bool capA;
    if (intersect_sub_cylinder(c->p1, c->break_p, c->radius * 0.95f, 0.0f, ray_orig, ray_dir,
                               &tA, &ptA, &normA, &uA, &vA, &capA, &rfA, true, true)) {
        if (tA < best_t) {
            best_t = tA; best_pt = ptA; best_norm = normA;
            best_u = uA; best_v = vA * 0.5f; best_cap = capA; best_rf = rfA;
            best_seg = (int)(vA * (NUM_LOG_SEGS / 2));
            if (best_seg >= NUM_LOG_SEGS / 2) best_seg = NUM_LOG_SEGS / 2 - 1;
            hit = true;
        }
    }

    // Sub B: break_p -> p2 (segments 5..9)
    float tB, uB, vB, rfB;
    Vec3 ptB, normB;
    bool capB;
    if (intersect_sub_cylinder(c->break_p, c->p2, c->radius * 0.95f, 0.0f, ray_orig, ray_dir,
                               &tB, &ptB, &normB, &uB, &vB, &capB, &rfB, true, true)) {
        if (tB < best_t) {
            best_t = tB; best_pt = ptB; best_norm = normB;
            best_u = uB; best_v = 0.5f + vB * 0.5f; best_cap = capB; best_rf = rfB;
            best_seg = (NUM_LOG_SEGS / 2) + (int)(vB * (NUM_LOG_SEGS - NUM_LOG_SEGS / 2));
            if (best_seg >= NUM_LOG_SEGS) best_seg = NUM_LOG_SEGS - 1;
            hit = true;
        }
    }

    if (hit) {
        *out_t = best_t; *out_pt = best_pt; *out_norm = best_norm;
        *out_u = best_u; *out_v = best_v; *out_endcap = best_cap;
        *out_rf = best_rf; *out_seg_idx = best_seg;
        return true;
    }
    return false;
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

static void spawn_smoke_3d(Vec3 pos, Vec3 vel, float size, float life, int type) {
    for (int i = 0; i < MAX_SMOKE; i++) {
        if (!g_smoke[i].active) {
            g_smoke[i].pos = pos;
            g_smoke[i].vel = vel;
            g_smoke[i].size = size;
            g_smoke[i].life = life;
            g_smoke[i].max_life = life;
            g_smoke[i].type = type;
            g_smoke[i].active = true;
            break;
        }
    }
}

static void spawn_rain_drop(void) {
    for (int i = 0; i < MAX_RAIN; i++) {
        if (!g_rain[i].active) {
            g_rain[i].pos = (Vec3){
                (rand_f() - 0.5f) * 16.0f,
                rand_f() * 4.0f + 8.0f,
                (rand_f() - 0.5f) * 16.0f
            };
            g_rain[i].speed = rand_f() * 6.0f + 20.0f;
            g_rain[i].length = rand_f() * 0.35f + 0.35f;
            g_rain[i].active = true;
            break;
        }
    }
}

static void stoke_fire_add_wood(void) {
    g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.40f);

    // Revive kindling
    for (int i = 0; i < g_num_twigs; i++) {
        if (!g_twigs[i].active) {
            g_twigs[i].active = true;
            g_twigs[i].burn_progress = 0.0f;
            g_twigs[i].moisture = 0.05f;
            g_twigs[i].temp = 0.45f;
        }
    }
    for (int l = 0; l < g_num_leaves; l++) {
        if (!g_leaves[l].active) {
            g_leaves[l].active = true;
            g_leaves[l].burn_progress = 0.0f;
            g_leaves[l].temp = 0.55f;
        }
    }

    // Add fresh log or replace consumed log
    int target_slot = -1;
    if (g_num_logs < MAX_LOGS) {
        target_slot = g_num_logs;
        g_num_logs++;
    } else {
        float min_mass = 999.0f;
        int min_idx = 0;
        for (int i = 0; i < g_num_logs; i++) {
            float m = 0.0f;
            for (int s = 0; s < NUM_LOG_SEGS; s++) m += g_logs[i].segments[s].structural_mass;
            if (m < min_mass) {
                min_mass = m;
                min_idx = i;
            }
        }
        target_slot = min_idx;
    }

    if (target_slot >= 0) {
        float angle = rand_f() * (float)M_PI;
        float dist = 2.4f;
        Vec3 p1 = (Vec3){ cosf(angle) * dist, -2.4f, sinf(angle) * dist };
        Vec3 p2 = (Vec3){ -cosf(angle) * dist, -2.2f, -sinf(angle) * dist };
        Vec3 p1_col = (Vec3){ p1.x * 0.9f, -3.9f, p1.z * 0.9f };
        Vec3 p2_col = (Vec3){ p2.x * 0.9f, -3.9f, p2.z * 0.9f };
        init_cylinder(&g_logs[target_slot], 100 + target_slot, p1, p2, p1_col, p2_col, 0.48f, 0.0f);
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            g_logs[target_slot].segments[s].moisture = 0.12f;
            g_logs[target_slot].segments[s].temp = 0.20f;
        }
    }

    g_audio_pop_trigger += 4;
    Vec3 hearth_c = (Vec3){0.0f, -3.5f, 0.0f};
    for (int sp = 0; sp < 28; sp++) {
        Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.8f, rand_f() * 4.0f + 2.0f, (rand_f() - 0.5f) * 2.8f};
        spawn_spark_3d(hearth_c, sp_v, rand_range(20, 50), PALETTE_EMBERS[rand_range(2, 4)]);
    }
}

static void update_simulation(void) {
    if (g_paused) return;

    // Physical time dimensionalization:
    // dt_frame = 0.025s (approx 40 FPS real terminal display)
    // Physical simulation step dt = 0.025s * g_time_scale
    float dt = 0.025f * g_time_scale;
    g_sim_time += dt;
    g_anim_time += 0.025f;

    // Ambient Wind oscillation & non-linear turbulence
    if (rand_f() < 0.05f) {
        g_wind_target = (rand_f() - 0.5f) * 1.8f;
    }
    g_wind += (g_wind_target - g_wind) * 0.04f;
    g_wind_turb = sinf(g_sim_time * 1.5f) * 0.35f + sinf(g_sim_time * 0.45f) * 0.5f + g_wind;

    // Auto-turntable continuous orbit
    if (g_auto_turntable) {
        g_cam_yaw += 0.015f * (g_realtime_mode ? 1.0f : fminf(2.5f, g_time_scale * 0.08f));
        if (g_cam_yaw > 2.0f * (float)M_PI) g_cam_yaw -= 2.0f * (float)M_PI;
    }

    // Sub-stepping for numerical stability:
    float sub_dt_target = 0.025f;
    int num_substeps = (int)ceilf(dt / sub_dt_target);
    if (num_substeps < 1) num_substeps = 1;
    if (num_substeps > 120) num_substeps = 120;
    float step_dt = dt / (float)num_substeps;

    Vec3 kindle_pos = (Vec3){0.0f, -2.6f, 0.0f};

    // Track frame total evaporation for audio steam hiss
    float frame_evap_rate = 0.0f;

    for (int step = 0; step < num_substeps; step++) {
        float cur_sim_t = g_sim_time - dt + (step + 1) * step_dt;
        float kindle_heat = 0.0f;
        if (cur_sim_t < 240.0f) {
            if (cur_sim_t < 60.0f) kindle_heat = 1.0f;
            else kindle_heat = fmaxf(0.0f, 1.0f - ((cur_sim_t - 60.0f) / 180.0f));
        }

        // ---------------------------------------------------------------------
        // HETEROGENEOUS SEGMENT THERMODYNAMICS & COMBUSTION
        // ---------------------------------------------------------------------
        for (int i = 0; i < g_num_logs; i++) {
            for (int s = 0; s < NUM_LOG_SEGS; s++) {
                float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                Vec3 seg_p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));

                if (g_logs[i].sag_amount > 0.01f) {
                    seg_p.y -= 4.0f * t_val * (1.0f - t_val) * g_logs[i].sag_amount;
                }

                float r_seg = sqrtf(seg_p.x * seg_p.x + seg_p.z * seg_p.z);
                float r_norm = r_seg / 3.6f;
                float eta_r = fmaxf(0.0f, 1.0f - r_norm * r_norm);

                // 1. Initial kindling nest heat
                if (kindle_heat > 0.02f) {
                    float d_k = vec3_len(vec3_sub(seg_p, kindle_pos));
                    float d_surf = fmaxf(0.0f, d_k - g_logs[i].radius - 1.5f);
                    if (d_surf < 2.8f) {
                        g_logs[i].segments[s].temp += 0.016f * (1.0f - d_surf / 2.8f) * kindle_heat * eta_r * step_dt;
                    }
                }

                // 2. Ash bed radiant ember heat
                if (g_ash_bed.heat > 0.15f && seg_p.y < 0.2f) {
                    float bed_dy = fmaxf(0.0f, seg_p.y - g_ash_bed.center.y);
                    if (bed_dy < 3.2f) {
                        float bed_fac = (1.0f - bed_dy / 3.2f) * eta_r;
                        g_logs[i].segments[s].temp += 0.0010f * g_ash_bed.heat * bed_fac * step_dt;
                    }
                }

                // 3. Cross-log fire radiation
                for (int j = 0; j < g_num_logs; j++) {
                    if (i == j) continue;
                    for (int sj = 0; sj < NUM_LOG_SEGS; sj++) {
                        if (g_logs[j].segments[sj].burn_progress > 0.03f && g_logs[j].segments[sj].temp > 0.30f && g_logs[j].segments[sj].structural_mass > 0.10f) {
                            float tj = (sj + 0.5f) / (float)NUM_LOG_SEGS;
                            Vec3 pj = vec3_add(g_logs[j].p1, vec3_scale(g_logs[j].axis, tj));
                            float d_cross = vec3_len(vec3_sub(seg_p, pj));
                            float d_cross_surf = fmaxf(0.0f, d_cross - g_logs[i].radius - g_logs[j].radius);
                            if (d_cross_surf < 1.6f) {
                                float rad_power = (1.0f - d_cross_surf / 1.6f) * (0.30f + 0.70f * eta_r);
                                g_logs[i].segments[s].temp += 0.0028f * rad_power * step_dt;
                            }
                        }
                    }
                }

                // 4. Moisture Evaporation & Latent Heat Clamping
                float evap = 0.0f;
                if (g_logs[i].segments[s].temp > 0.08f && g_logs[i].segments[s].moisture > 0.001f) {
                    evap = (0.0018f * (g_logs[i].segments[s].temp - 0.05f) + 0.0030f * kindle_heat * eta_r) * step_dt;
                    if (evap > g_logs[i].segments[s].moisture) evap = g_logs[i].segments[s].moisture;
                    g_logs[i].segments[s].moisture -= evap;
                    frame_evap_rate += evap * 15.0f;

                    // Emit fluffy white steam particle
                    if (rand_f() < (0.12f * step_dt * 40.0f)) {
                        Vec3 steam_v = (Vec3){
                            (rand_f() - 0.5f) * 0.6f + g_wind_turb * 0.5f,
                            rand_f() * 1.5f + 0.8f,
                            (rand_f() - 0.5f) * 0.6f
                        };
                        spawn_smoke_3d(seg_p, steam_v, 0.32f, 2.8f, 0);
                    }
                }
                if (g_logs[i].segments[s].moisture > 0.03f) {
                    if (g_logs[i].segments[s].temp > 0.28f) {
                        g_logs[i].segments[s].temp = 0.28f;
                    }
                }

                // 5. Conservative 1D thermal diffusion along wood grain
                float cur = g_logs[i].segments[s].temp;
                float prev_t = (s > 0) ? g_logs[i].segments[s-1].temp : cur;
                float next_t = (s < NUM_LOG_SEGS - 1) ? g_logs[i].segments[s+1].temp : cur;
                float laplacian = prev_t - 2.0f * cur + next_t;
                float k_eff = 0.0018f * (1.0f - 0.65f * g_logs[i].segments[s].burn_progress);
                g_logs[i].segments[s].temp += k_eff * laplacian * step_dt;

                // 6. Ambient convective cooling
                float cool_factor = 1.0f - 0.75f * eta_r;
                g_logs[i].segments[s].temp -= 0.00025f * cur * cool_factor * step_dt;

                // 7. Active combustion & Pyrolysis
                if (g_logs[i].segments[s].moisture <= 0.04f && g_logs[i].segments[s].temp > 0.30f) {
                    float burn_rate = 0.0f;
                    if (eta_r > 0.05f) {
                        float exo = 0.0016f * (g_logs[i].segments[s].temp - 0.28f) * (0.35f + 0.65f * eta_r) * g_logs[i].segments[s].structural_mass * step_dt;
                        g_logs[i].segments[s].temp = fminf(1.0f, g_logs[i].segments[s].temp + exo);
                        burn_rate = 0.00035f * (g_logs[i].segments[s].temp - 0.25f) * (0.25f + 0.75f * eta_r) * step_dt;
                        g_logs[i].segments[s].burn_progress = fminf(1.0f, g_logs[i].segments[s].burn_progress + burn_rate);
                    } else {
                        burn_rate = 0.00008f * (g_logs[i].segments[s].temp - 0.25f) * step_dt;
                        g_logs[i].segments[s].burn_progress = fminf(1.0f, g_logs[i].segments[s].burn_progress + burn_rate);
                    }

                    // Pyrolytic wood smoke particle
                    if (burn_rate > 0.00001f && rand_f() < (0.08f * step_dt * 40.0f)) {
                        Vec3 smoke_v = (Vec3){
                            (rand_f() - 0.5f) * 0.7f + g_wind_turb * 0.7f,
                            rand_f() * 2.0f + 1.2f,
                            (rand_f() - 0.5f) * 0.7f
                        };
                        spawn_smoke_3d(seg_p, smoke_v, 0.40f, 3.2f, 1);
                    }

                    // Update structural mass
                    g_logs[i].segments[s].structural_mass = fmaxf(0.0f, 1.0f - g_logs[i].segments[s].burn_progress * 1.25f);

                    // Shed ash flakes
                    if (g_logs[i].segments[s].burn_progress > 0.55f && rand_f() < (0.04f * step_dt * 40.0f)) {
                        float angle = rand_f() * 6.28318f;
                        Vec3 rad_dir = vec3_add(vec3_scale(g_logs[i].tangent, cosf(angle)),
                                                vec3_scale(g_logs[i].bitangent, sinf(angle)));
                        Vec3 ash_p = vec3_add(seg_p, vec3_scale(rad_dir, g_logs[i].radius * 0.95f));
                        Vec3 vel = (Vec3){
                            (rand_f() - 0.5f) * 0.3f + g_wind_turb * 0.3f,
                            -(rand_f() * 0.25f + 0.15f),
                            (rand_f() - 0.5f) * 0.3f
                        };
                        spawn_ash_3d(ash_p, vel, PALETTE_ASH[rand_range(2, 3)]);
                    }
                }

                // Fuel depletion
                if (g_logs[i].segments[s].burn_progress > 0.85f) {
                    g_logs[i].segments[s].temp = fmaxf(0.10f, g_logs[i].segments[s].temp - 0.00015f * step_dt);
                }
                if (g_logs[i].segments[s].temp < 0.0f) g_logs[i].segments[s].temp = 0.0f;

                // 8. Air Draft & Coal Incandescence
                float v_air = eta_r * (0.75f + 0.50f * g_logs[i].segments[s].temp) + 0.60f * fabsf(g_wind_turb);
                float glow = g_logs[i].segments[s].temp * fminf(1.0f, 0.35f + 0.65f * v_air);
                if (g_logs[i].segments[s].moisture > 0.05f) glow *= 0.15f;
                g_logs[i].segments[s].glow_intensity = glow;

                // 9. Sap Pocket Pops
                if (g_logs[i].segments[s].temp > 0.45f && g_logs[i].segments[s].burn_progress >= 0.15f && g_logs[i].segments[s].burn_progress <= 0.75f) {
                    if (rand_f() < (0.015f * step_dt * 40.0f)) {
                        g_audio_pop_trigger++;
                        int num_sp = rand_range(6, 10);
                        for (int sp = 0; sp < num_sp; sp++) {
                            float angle = rand_f() * 6.28318f;
                            Vec3 rad_dir = vec3_add(vec3_scale(g_logs[i].tangent, cosf(angle)),
                                                    vec3_scale(g_logs[i].bitangent, sinf(angle)));
                            Vec3 sp_p = vec3_add(seg_p, vec3_scale(rad_dir, g_logs[i].radius * 0.95f));
                            Vec3 pop_v = (Vec3){
                                rad_dir.x * (rand_f() * 3.5f + 2.0f) + g_wind_turb * 1.5f,
                                rand_f() * 4.5f + 3.0f,
                                rad_dir.z * (rand_f() * 3.5f + 2.0f)
                            };
                            spawn_spark_3d(sp_p, pop_v, rand_range(20, 55), PALETTE_EMBERS[rand_range(2, 4)]);
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------------
        // KINDLING LEAVES & TWIGS COMBUSTION
        // ---------------------------------------------------------------------
        for (int l = 0; l < g_num_leaves; l++) {
            if (!g_leaves[l].active) continue;
            if (kindle_heat > 0.02f || cur_sim_t < 60.0f) {
                g_leaves[l].temp += 0.055f * fmaxf(kindle_heat, 0.4f) * step_dt;
            }
            if (g_leaves[l].temp > 0.20f) {
                float leaf_burn = 0.038f * (g_leaves[l].temp - 0.15f) * step_dt;
                g_leaves[l].burn_progress += leaf_burn;
                g_leaves[l].temp = fminf(1.0f, g_leaves[l].temp + 0.065f * step_dt);
                if (rand_f() < 0.20f * step_dt * 40.0f) {
                    Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 1.8f, rand_f() * 3.5f + 1.5f, (rand_f() - 0.5f) * 1.8f};
                    spawn_spark_3d(g_leaves[l].pos, sp_v, rand_range(12, 28), PALETTE_EMBERS[rand_range(2, 4)]);
                }
            }
            if (g_leaves[l].burn_progress >= 0.95f) {
                g_leaves[l].active = false;
            }
        }

        for (int tw = 0; tw < g_num_twigs; tw++) {
            if (!g_twigs[tw].active) continue;
            if (kindle_heat > 0.02f) {
                g_twigs[tw].temp += 0.030f * kindle_heat * step_dt;
            }
            for (int l = 0; l < g_num_leaves; l++) {
                if (g_leaves[l].burn_progress > 0.1f && g_leaves[l].active) {
                    float d = vec3_len(vec3_sub(g_leaves[l].pos, g_twigs[tw].p1));
                    if (d < 1.4f) g_twigs[tw].temp += 0.020f * (1.0f - d / 1.4f) * step_dt;
                }
            }
            if (g_twigs[tw].temp > 0.08f && g_twigs[tw].moisture > 0.001f) {
                float evap = 0.0065f * (g_twigs[tw].temp - 0.05f) * step_dt;
                g_twigs[tw].moisture = fmaxf(0.0f, g_twigs[tw].moisture - evap);
                frame_evap_rate += evap * 20.0f;
            }
            if (g_twigs[tw].moisture <= 0.04f && g_twigs[tw].temp > 0.25f) {
                float exo = 0.0050f * (g_twigs[tw].temp - 0.20f) * step_dt;
                g_twigs[tw].temp = fminf(1.0f, g_twigs[tw].temp + exo);
                float burn = 0.0011f * (g_twigs[tw].temp - 0.20f) * step_dt;
                g_twigs[tw].burn_progress += burn;
            }
            if (g_twigs[tw].burn_progress >= 1.0f) {
                g_twigs[tw].active = false;
                g_twigs[tw].temp = 0.15f;
            }

            if (g_twigs[tw].temp > 0.30f) {
                Vec3 mid_tw = vec3_scale(vec3_add(g_twigs[tw].p1, g_twigs[tw].p2), 0.5f);
                for (int i = 0; i < g_num_logs; i++) {
                    for (int s = 0; s < NUM_LOG_SEGS; s++) {
                        float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                        Vec3 seg_p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));
                        float d = vec3_len(vec3_sub(seg_p, mid_tw));
                        if (d < 1.8f) {
                            g_logs[i].segments[s].temp += 0.0040f * (1.0f - d / 1.8f) * g_twigs[tw].temp * step_dt;
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------------
        // ASH BED & REACTION STATUS
        // ---------------------------------------------------------------------
        float total_mass = 0.0f;
        int total_segs = g_num_logs * NUM_LOG_SEGS;
        int active_burning_segs = 0;
        for (int i = 0; i < g_num_logs; i++) {
            for (int s = 0; s < NUM_LOG_SEGS; s++) {
                total_mass += g_logs[i].segments[s].structural_mass;
                if (g_logs[i].segments[s].temp > 0.30f && g_logs[i].segments[s].moisture <= 0.04f) active_burning_segs++;
            }
        }
        float avg_mass = total_mass / (float)total_segs;
        float burnt_mass = 1.0f - avg_mass;
        g_burnt_mass = burnt_mass;

        float target_height = fminf(1.4f, 0.35f + burnt_mass * 0.85f + g_ash_bed.volume);
        float smooth_rate = 1.0f - expf(-step_dt * 0.8f);
        g_ash_bed.height += (target_height - g_ash_bed.height) * smooth_rate;

        if (active_burning_segs > 0) {
            g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.0005f * step_dt * 40.0f);
        } else if (cur_sim_t > 180.0f) {
            g_ash_bed.heat = fmaxf(0.12f, g_ash_bed.heat - 0.0001f * step_dt * 40.0f);
        }

        // ---------------------------------------------------------------------
        // GRAVITY SAGGING, SNAP FRACTURE & COLLAPSE SETTLING
        // ---------------------------------------------------------------------
        for (int i = 0; i < g_num_logs; i++) {
            float support_integrity = 1.0f;
            if (g_logs[i].support_log1 >= 0 && g_logs[i].support_log1 < g_num_logs) {
                int s1 = g_logs[i].support_log1;
                float s1_m = (g_logs[s1].segments[3].structural_mass + g_logs[s1].segments[8].structural_mass) * 0.5f;
                float s1_eff = s1_m * (1.0f - g_logs[s1].collapse_cur * 0.65f);
                support_integrity = fminf(support_integrity, s1_eff);
            }
            if (g_logs[i].support_log2 >= 0 && g_logs[i].support_log2 < g_num_logs) {
                int s2 = g_logs[i].support_log2;
                float s2_m = (g_logs[s2].segments[3].structural_mass + g_logs[s2].segments[8].structural_mass) * 0.5f;
                float s2_eff = s2_m * (1.0f - g_logs[s2].collapse_cur * 0.65f);
                support_integrity = fminf(support_integrity, s2_eff);
            }

            float own_center_mass = (g_logs[i].segments[4].structural_mass + g_logs[i].segments[5].structural_mass + g_logs[i].segments[6].structural_mass) / 3.0f;
            float log_integrity = fminf(support_integrity, own_center_mass);

            if (own_center_mass < 0.65f) {
                g_logs[i].sag_amount = 0.55f * (1.0f - own_center_mass) * (1.0f - own_center_mass);
            }

            // Snap fracture when central mass is structurally exhausted
            if (own_center_mass < 0.22f && !g_logs[i].snapped) {
                g_logs[i].snapped = true;
                g_logs[i].fractured = true;
                g_audio_pop_trigger += 3;

                Vec3 ctr_p = vec3_scale(vec3_add(g_logs[i].p1, g_logs[i].p2), 0.5f);
                ctr_p.y -= g_logs[i].sag_amount;

                // Sparks burst
                for (int sp = 0; sp < 16; sp++) {
                    Vec3 snap_v = (Vec3){(rand_f() - 0.5f) * 2.5f, rand_f() * 3.5f + 1.5f, (rand_f() - 0.5f) * 2.5f};
                    spawn_spark_3d(ctr_p, snap_v, rand_range(25, 60), PALETTE_EMBERS[rand_range(2, 4)]);
                }
                // Soot smoke puff
                for (int sk = 0; sk < 8; sk++) {
                    Vec3 smk_v = (Vec3){(rand_f() - 0.5f) * 1.6f + g_wind_turb * 0.5f, rand_f() * 2.5f + 1.0f, (rand_f() - 0.5f) * 1.6f};
                    spawn_smoke_3d(ctr_p, smk_v, 0.48f, 3.5f, 2);
                }
            }

            float target_c = 0.0f;
            if (g_force_collapse || g_logs[i].snapped) {
                target_c = 1.0f;
            } else if (log_integrity < 0.45f) {
                target_c = fminf(1.0f, (0.45f - log_integrity) / 0.45f);
            }

            if (target_c > g_logs[i].collapse_cur) {
                g_logs[i].collapse_speed += (g_logs[i].snapped ? 0.0040f : 0.0018f) * step_dt * 40.0f;
                g_logs[i].collapse_cur += g_logs[i].collapse_speed;
                if (g_logs[i].collapse_cur >= target_c) {
                    g_logs[i].collapse_cur = target_c;
                    g_logs[i].collapse_speed = 0.0f;
                }
            }

            float c = g_logs[i].collapse_cur;
            g_logs[i].p1.x = g_logs[i].p1_orig.x * (1.0f - c) + g_logs[i].p1_collapsed.x * c;
            g_logs[i].p1.y = g_logs[i].p1_orig.y * (1.0f - c) + g_logs[i].p1_collapsed.y * c;
            g_logs[i].p1.z = g_logs[i].p1_orig.z * (1.0f - c) + g_logs[i].p1_collapsed.z * c;

            g_logs[i].p2.x = g_logs[i].p2_orig.x * (1.0f - c) + g_logs[i].p2_collapsed.x * c;
            g_logs[i].p2.y = g_logs[i].p2_orig.y * (1.0f - c) + g_logs[i].p2_collapsed.y * c;
            g_logs[i].p2.z = g_logs[i].p2_orig.z * (1.0f - c) + g_logs[i].p2_collapsed.z * c;

            if (g_logs[i].fractured) {
                if (g_logs[i].fracture_prog < 1.0f) {
                    g_logs[i].fracture_prog += 0.015f * step_dt * 40.0f;
                    if (g_logs[i].fracture_prog > 1.0f) g_logs[i].fracture_prog = 1.0f;
                }
                float fp = g_logs[i].fracture_prog;
                g_logs[i].break_p.x = g_logs[i].break_p_orig.x * (1.0f - fp) + g_logs[i].break_p_target.x * fp;
                g_logs[i].break_p.y = g_logs[i].break_p_orig.y * (1.0f - fp) + g_logs[i].break_p_target.y * fp;
                g_logs[i].break_p.z = g_logs[i].break_p_orig.z * (1.0f - fp) + g_logs[i].break_p_target.z * fp;
            }

            recompute_cylinder_axes(&g_logs[i]);
        }

        // Update audio variables
        float total_heat = (float)active_burning_segs / (float)(g_num_logs * NUM_LOG_SEGS) * 2.2f + g_ash_bed.heat * 0.35f;
        if (cur_sim_t < 60.0f) total_heat += kindle_heat * 0.45f;
        g_audio_fire_heat += (fminf(1.0f, total_heat) - g_audio_fire_heat) * 0.08f;
        g_audio_steam_rate = fmaxf(0.0f, g_audio_steam_rate - 0.005f) + frame_evap_rate;
        g_audio_rain_intensity += ((g_rain_active ? 1.0f : 0.0f) - g_audio_rain_intensity) * 0.1f;
    }

    // -------------------------------------------------------------------------
    // RAIN SIMULATION (Drizzle & Sizzle on Hearth)
    // -------------------------------------------------------------------------
    if (g_rain_active) {
        for (int r = 0; r < 4; r++) spawn_rain_drop();
    }
    for (int i = 0; i < MAX_RAIN; i++) {
        if (g_rain[i].active) {
            g_rain[i].pos.y -= g_rain[i].speed * 0.025f;
            g_rain[i].pos.x += g_wind_turb * 3.5f * 0.025f;
            g_rain[i].pos.z += (g_wind_turb * 0.5f) * 0.025f;

            if (g_rain[i].pos.y <= -4.2f) {
                float r_sq = g_rain[i].pos.x * g_rain[i].pos.x + g_rain[i].pos.z * g_rain[i].pos.z;
                if (r_sq < 22.0f && (g_ash_bed.heat > 0.2f || g_audio_fire_heat > 0.15f)) {
                    // Rain sizzling into steam
                    spawn_smoke_3d(g_rain[i].pos, (Vec3){(rand_f()-0.5f)*0.3f, rand_f()*1.2f+0.4f, (rand_f()-0.5f)*0.3f}, 0.24f, 1.4f, 0);
                    g_audio_steam_rate = fminf(1.0f, g_audio_steam_rate + 0.025f);
                }
                g_rain[i].active = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // VOLUMETRIC SMOKE & STEAM SIMULATION
    // -------------------------------------------------------------------------
    for (int i = 0; i < MAX_SMOKE; i++) {
        if (g_smoke[i].active) {
            g_smoke[i].pos.x += g_smoke[i].vel.x * 0.025f;
            g_smoke[i].pos.y += g_smoke[i].vel.y * 0.025f;
            g_smoke[i].pos.z += g_smoke[i].vel.z * 0.025f;

            g_smoke[i].vel.x += (g_wind_turb * 1.5f - g_smoke[i].vel.x * 0.4f) * 0.05f + sinf(g_smoke[i].pos.y * 1.4f + g_anim_time * 2.0f) * 0.012f;
            g_smoke[i].vel.y = fmaxf(0.35f, g_smoke[i].vel.y * 0.985f);
            g_smoke[i].vel.z += (rand_f() - 0.5f) * 0.02f;

            g_smoke[i].size += 0.22f * 0.025f;
            g_smoke[i].life -= 0.025f;
            if (g_smoke[i].life <= 0.0f || g_smoke[i].pos.y > 12.0f) {
                g_smoke[i].active = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // STRICT FIRE PROJECTION: Heat originates strictly from burning wood & kindling
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

    // Reset next fire frame and depth buffer
    memset(g_next_fire, 0, sizeof(g_next_fire));
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_z[y][x] = 1e9f;
        }
    }

    // 1. Kindling flames emitted directly from twigs and leaves on the hearth floor!
    float cur_kindle_heat = (g_sim_time < 240.0f) ? (g_sim_time < 60.0f ? 1.0f : fmaxf(0.0f, 1.0f - (g_sim_time - 60.0f) / 180.0f)) : 0.0f;
    for (int tw = 0; tw < g_num_twigs; tw++) {
        float tw_heat = (cur_kindle_heat > 0.05f) ? fmaxf(cur_kindle_heat * 0.85f, g_twigs[tw].temp) : (g_twigs[tw].active ? g_twigs[tw].temp : 0.0f);
        if (tw_heat > 0.25f) {
            Vec3 mid_tw = vec3_scale(vec3_add(g_twigs[tw].p1, g_twigs[tw].p2), 0.5f);
            Vec3 rel_k = vec3_sub(mid_tw, cam_pos);
            int kx = (int)(((vec3_dot(rel_k, right) / world_w) + 0.5f) * g_pixel_w);
            int ky = (int)((0.5f - (vec3_dot(rel_k, up) / world_h)) * g_pixel_h);
            float kz = vec3_dot(rel_k, fwd);
            float flame_z = kz - g_twigs[tw].radius * 0.9f;
            float flame_h = tw_heat * 0.92f;

            for (int dy = -2; dy <= 1; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    int px = kx + dx;
                    int py = ky + dy;
                    if (px >= 0 && px < g_pixel_w && py >= 0 && py < g_pixel_h) {
                        float d = sqrtf((dx * 1.0f)*(dx * 1.0f) + (dy * 1.5f)*(dy * 1.5f));
                        if (d < 2.5f) {
                            float val = flame_h * (1.0f - d / 2.5f);
                            g_fire_heat[py][px] = fmaxf(g_fire_heat[py][px], val);
                            if (flame_z < g_fire_z[py][px]) g_fire_z[py][px] = flame_z;
                        }
                    }
                }
            }
        }
    }

    // 2. Heat anchored exclusively to burning wood segments inside the core draft!
    for (int i = 0; i < g_num_logs; i++) {
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            float temp = g_logs[i].segments[s].temp;
            float moisture = g_logs[i].segments[s].moisture;
            if (temp > 0.30f && moisture <= 0.05f) {
                float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));
                if (g_logs[i].sag_amount > 0.01f) {
                    p.y -= 4.0f * t_val * (1.0f - t_val) * g_logs[i].sag_amount;
                }

                float r_seg = sqrtf(p.x * p.x + p.z * p.z);
                float r_norm = r_seg / 3.4f;
                float eta_r = fmaxf(0.0f, 1.0f - r_norm * r_norm);

                if (eta_r <= 0.08f) continue;

                Vec3 rel_p = vec3_sub(p, cam_pos);
                int px = (int)(((vec3_dot(rel_p, right) / world_w) + 0.5f) * g_pixel_w);
                int py = (int)((0.5f - (vec3_dot(rel_p, up) / world_h)) * g_pixel_h);
                float pz = vec3_dot(rel_p, fwd);
                float flame_z = pz - g_logs[i].radius * 0.85f;

                float flame_h = temp * (0.25f + 0.75f * eta_r) * 1.05f;
                for (int dy = -3; dy <= 1; dy++) {
                    for (int dx = -3; dx <= 3; dx++) {
                        int sx = px + dx;
                        int sy = py + dy;
                        if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                            float d = sqrtf((dx * 0.9f)*(dx * 0.9f) + (dy * 1.5f)*(dy * 1.5f));
                            if (d < 3.2f) {
                                float val = flame_h * (1.0f - d / 3.2f);
                                g_fire_heat[sy][sx] = fmaxf(g_fire_heat[sy][sx], val);
                                if (flame_z < g_fire_z[sy][sx]) g_fire_z[sy][sx] = flame_z;
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Convective flame propagation upwards with wind turbulence
    for (int y = g_pixel_h - 4; y >= 2; y--) {
        for (int x = 0; x < g_pixel_w; x++) {
            int src_x = x;
            int wind_step = (g_wind_turb > 0.30f) ? 1 : ((g_wind_turb < -0.30f) ? -1 : 0);
            int jitter = (xorshift32() % 3) - 1;
            src_x += (rand_f() < 0.45f) ? wind_step : jitter;
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
            if (val > 0.05f) {
                g_fire_z[y][x] = g_fire_z[y + 1][src_x];
            }
        }
    }

    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_heat[y][x] = g_next_fire[y][x];
        }
    }

    // 4. Sparks Ejection from burning wood
    if (rand_f() < 0.65f) {
        for (int i = 0; i < g_num_logs; i++) {
            int s = rand_range(0, NUM_LOG_SEGS - 1);
            if (g_logs[i].segments[s].temp > 0.50f) {
                float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
                Vec3 p = vec3_add(g_logs[i].p1, vec3_scale(g_logs[i].axis, t_val));
                if (g_logs[i].sag_amount > 0.01f) {
                    p.y -= 4.0f * t_val * (1.0f - t_val) * g_logs[i].sag_amount;
                }
                Vec3 spark_v = (Vec3){(rand_f() - 0.5f) * 1.5f + g_wind_turb * 1.4f, rand_f() * 3.2f + 2.0f, (rand_f() - 0.5f) * 1.5f};
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
            g_ash_flakes[i].pos.x += g_ash_flakes[i].vel.x * 0.04f + sinf(g_ash_flakes[i].pos.y * 1.5f + g_anim_time * 2.0f) * 0.015f;
            g_ash_flakes[i].pos.y += g_ash_flakes[i].vel.y * 0.04f;
            g_ash_flakes[i].pos.z += g_ash_flakes[i].vel.z * 0.04f;

            float r_sq = g_ash_flakes[i].pos.x * g_ash_flakes[i].pos.x + g_ash_flakes[i].pos.z * g_ash_flakes[i].pos.z;
            float r_bed = g_ash_bed.radius_xz;
            float bed_h = (r_sq < r_bed * r_bed) ? (g_ash_bed.height * sqrtf(1.0f - r_sq / (r_bed * r_bed))) : 0.0f;
            float floor_y = -4.2f + bed_h;

            if (g_ash_flakes[i].pos.y <= floor_y) {
                if (r_sq < r_bed * r_bed) {
                    g_ash_bed.volume += 0.002f;
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
    float flicker = 1.0f + 0.16f * sinf(g_anim_time * 8.0f) + 0.10f * cosf(g_anim_time * 13.0f);
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
                if (intersect_stone(&g_stones[i], ray_orig, ray_dir, &t, &pt, &norm)) {
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

            // Test 3D Twigs
            Twig3D *hit_twig = NULL;
            Vec3 twig_pt = {0,0,0}, twig_norm = {0,1,0};
            for (int i = 0; i < g_num_twigs; i++) {
                float t;
                Vec3 pt, norm;
                if (intersect_twig(&g_twigs[i], ray_orig, ray_dir, &t, &pt, &norm)) {
                    if (t < closest_t) {
                        closest_t = t;
                        twig_pt = pt;
                        twig_norm = norm;
                        hit_twig = &g_twigs[i];
                        hit_type = 4;
                    }
                }
            }

            // Test 3D Foliage Leaves
            Leaf3D *hit_leaf = NULL;
            Vec3 leaf_pt = {0,0,0}, leaf_norm = {0,1,0};
            for (int i = 0; i < g_num_leaves; i++) {
                float t;
                Vec3 pt, norm;
                if (intersect_leaf(&g_leaves[i], ray_orig, ray_dir, &t, &pt, &norm)) {
                    if (t < closest_t) {
                        closest_t = t;
                        leaf_pt = pt;
                        leaf_norm = norm;
                        hit_leaf = &g_leaves[i];
                        hit_type = 5;
                    }
                }
            }

            // Test ground/ash bed plane — flat at ground_y, inside stone ring radius
            // This is more realistic than a dome: ash lies on the floor between logs
            Vec3 ash_bed_pt = {0,0,0}, ash_bed_norm = {0,1,0};
            {
                float ground_plane_y = -4.2f;
                // Ray-plane intersection: (ray_orig.y + t * ray_dir.y) = ground_plane_y
                if (ray_dir.y < -0.001f) {  // Ray must point downward to hit ground
                    float gp_t = (ground_plane_y - ray_orig.y) / ray_dir.y;
                    if (gp_t > 0.1f && gp_t < closest_t) {
                        Vec3 gp_pt = vec3_add(ray_orig, vec3_scale(ray_dir, gp_t));
                        // Only show ash within the stone ring (~5.2 units from center)
                        float gp_r = sqrtf(gp_pt.x * gp_pt.x + gp_pt.z * gp_pt.z);
                        if (gp_r < 5.2f) {
                            closest_t = gp_t;
                            ash_bed_pt = gp_pt;
                            ash_bed_norm = (Vec3){0.0f, 1.0f, 0.0f};
                            hit_type = 3;
                        }
                    }
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
                int s_idx = (int)((s_val + rock_noise + hit_stone->shade_var) * 2.8f);
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
                float glow = hit_log->segments[hit_seg].glow_intensity;
                float moisture = hit_log->segments[hit_seg].moisture;

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
                            if (glow > 0.10f) {
                                int emb_idx = (int)(glow * 3.8f);
                                if (emb_idx < 0) emb_idx = 0;
                                if (emb_idx > 4) emb_idx = 4;
                                g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                            } else if (seg_temp > 0.25f) {
                                g_shade_buf[y][x] = PALETTE_EMBERS[0];
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
                        if (is_furrow && (glow > 0.12f || seg_temp > 0.38f)) {
                            float hotness = fmaxf(glow, seg_temp * 0.85f);
                            int emb_idx = (int)(hotness * 3.8f);
                            if (emb_idx < 0) emb_idx = 0;
                            if (emb_idx > 4) emb_idx = 4;
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
                            if (moisture > 0.08f && b_idx > 1) b_idx--; // Damp wood darkening
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
                float ambient = 0.20f + 0.14f * fmaxf(0.0f, ash_bed_norm.y);
                float s_val = ndotl * atten * light_intensity * 2.0f + ambient;

                float r_core = sqrtf(ash_bed_pt.x * ash_bed_pt.x + ash_bed_pt.z * ash_bed_pt.z);
                float core_heat = g_ash_bed.heat * fmaxf(0.0f, 1.0f - r_core / 4.2f);

                // Natural hearth floor: dirt/earth ground by default
                // Ash accumulates dynamically as wood is consumed (g_burnt_mass)
                float ash_radius = 1.2f + 3.6f * g_burnt_mass;
                float ash_coverage = 0.0f;
                if (r_core < ash_radius) {
                    ash_coverage = (1.0f - r_core / ash_radius) * (0.25f + 0.75f * g_burnt_mass);
                }

                if (ash_coverage < 0.15f) {
                    // NATURAL HEARTH DIRT / COMPACTED EARTH
                    float dirt_noise = (sinf(ash_bed_pt.x * 2.8f + 0.4f) * cosf(ash_bed_pt.z * 2.5f + 0.8f)) * 0.5f + 0.5f;
                    int d_idx = (int)((s_val * 0.95f + dirt_noise * 0.20f) * 2.4f);
                    if (d_idx < 0) d_idx = 0;
                    if (d_idx > 4) d_idx = 4;
                    g_shade_buf[y][x] = PALETTE_DIRT[d_idx];
                } else {
                    // ACCUMULATED ASH & COALS
                    float f1 = sinf(ash_bed_pt.x * 1.3f + ash_bed_pt.z * 0.7f);
                    float f2 = cosf(ash_bed_pt.z * 1.4f - ash_bed_pt.x * 0.6f);
                    float fissure = fabsf(f1 * f2);

                    if (fissure < 0.15f && core_heat > 0.22f && r_core < 2.5f) {
                        // Deep incandescent ember vein
                        float emb_heat = core_heat * (1.0f - fissure / 0.15f);
                        int emb_idx = (int)(emb_heat * 3.5f);
                        if (emb_idx < 0) emb_idx = 0;
                        if (emb_idx > 3) emb_idx = 3;
                        g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                    } else {
                        // Calcified ash progression
                        int ash_idx;
                        if (r_core < 1.8f) {
                            // Charcoal core with warm cast
                            ash_idx = (int)(s_val * 1.2f);
                            if (ash_idx < 0) ash_idx = 0;
                            if (ash_idx > 1) ash_idx = 1;
                            if (core_heat > 0.25f && ash_idx == 1) {
                                g_shade_buf[y][x] = (RGB){
                                    (uint8_t)fminf(255, 58 + (int)(core_heat * 50)),
                                    (uint8_t)(50 + (int)(core_heat * 20)),
                                    (uint8_t)(28 + (int)(core_heat * 5))
                                };
                                goto ash_done;
                            }
                        } else if (ash_coverage < 0.50f || g_burnt_mass < 0.40f) {
                            // Early mid-grey ash
                            ash_idx = (int)(s_val * 1.8f);
                            if (ash_idx < 1) ash_idx = 1;
                            if (ash_idx > 2) ash_idx = 2;
                        } else if (ash_coverage < 0.75f || g_burnt_mass < 0.75f) {
                            // Mid to light ash
                            ash_idx = (int)(s_val * 2.0f + 0.8f);
                            if (ash_idx < 2) ash_idx = 2;
                            if (ash_idx > 3) ash_idx = 3;
                        } else {
                            // Late-stage thick chalky white ash
                            ash_idx = (int)(s_val * 2.2f + 1.2f);
                            if (ash_idx < 3) ash_idx = 3;
                            if (ash_idx > 4) ash_idx = 4;
                        }
                        g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                        ash_done:;
                    }
                }

            } else if (hit_type == 4 && hit_twig != NULL) {
                g_id_buf[y][x] = hit_twig->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(twig_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, twig_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(twig_norm, l_dir));
                float ambient = 0.25f + 0.12f * fmaxf(0.0f, twig_norm.y);
                float light_val = ndotl * atten * light_intensity * 2.2f + ambient;

                if (hit_twig->burn_progress > 0.65f) {
                    if (hit_twig->temp > 0.35f) {
                        g_shade_buf[y][x] = PALETTE_EMBERS[1];
                    } else {
                        g_shade_buf[y][x] = PALETTE_CHARRED[1];
                    }
                } else if (hit_twig->burn_progress > 0.25f) {
                    if (hit_twig->temp > 0.35f) {
                        g_shade_buf[y][x] = PALETTE_EMBERS[0];
                    } else {
                        g_shade_buf[y][x] = PALETTE_CHARRED[2];
                    }
                } else {
                    int b_idx = (int)(light_val * 2.4f + 1.2f);
                    if (b_idx < 1) b_idx = 1;
                    if (b_idx > 4) b_idx = 4;
                    g_shade_buf[y][x] = PALETTE_WOOD[b_idx];
                }

            } else if (hit_type == 5 && hit_leaf != NULL) {
                g_id_buf[y][x] = hit_leaf->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(leaf_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, leaf_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, fabsf(vec3_dot(leaf_norm, l_dir)));
                float ambient = 0.30f + 0.15f * fmaxf(0.0f, leaf_norm.y);
                float light_val = ndotl * atten * light_intensity * 2.0f + ambient;

                float leaf_burn = hit_leaf->burn_progress;
                if (leaf_burn > 0.60f) {
                    if (hit_leaf->temp > 0.30f) {
                        g_shade_buf[y][x] = PALETTE_EMBERS[2];
                    } else {
                        g_shade_buf[y][x] = PALETTE_LEAF[5];
                    }
                } else if (leaf_burn > 0.25f) {
                    int c_idx = (int)(light_val * 1.5f + 3.0f);
                    if (c_idx < 3) c_idx = 3;
                    if (c_idx > 4) c_idx = 4;
                    g_shade_buf[y][x] = PALETTE_LEAF[c_idx];
                } else {
                    int g_idx = (int)(light_val * 1.8f + 1.0f);
                    if (g_idx < 1) g_idx = 1;
                    if (g_idx > 2) g_idx = 2;
                    g_shade_buf[y][x] = PALETTE_LEAF[g_idx];
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
                if (curr_id >= OBJ_LEAF_BASE) {
                    g_frame[y][x].color = g_shade_buf[y][x];
                } else if (curr_id == OBJ_ASH_BED) {
                    g_frame[y][x].color = PALETTE_DIRT[0];
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
                    if (g_id_buf[y][x] == OBJ_ASH_BED || g_fire_z[y][x] < g_depth_buf[y][x] - 0.06f) {
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

    // 4. Volumetric Smoke & Steam (3D billboard spheres with Bayer dithering and depth test)
    for (int i = 0; i < MAX_SMOKE; i++) {
        if (!g_smoke[i].active) continue;
        Vec3 p_rel = vec3_sub(g_smoke[i].pos, cam_pos);
        float sz = vec3_dot(p_rel, fwd);
        if (sz < 0.5f) continue;

        int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
        int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);

        float screen_r = (g_smoke[i].size / world_w) * g_pixel_w * (cam_dist / sz);
        int rad = (int)ceilf(screen_r);
        if (rad < 1) rad = 1;
        if (rad > 14) rad = 14;

        float alpha_fac = (g_smoke[i].life / g_smoke[i].max_life);

        for (int dy = -rad; dy <= rad; dy++) {
            int py = sy + dy;
            if (py < 0 || py >= g_pixel_h) continue;
            for (int dx = -rad; dx <= rad; dx++) {
                int px = sx + dx;
                if (px < 0 || px >= g_pixel_w) continue;

                float dist_sq = (dx * 1.0f) * (dx * 1.0f) + (dy * 1.8f) * (dy * 1.8f);
                float max_r_sq = (float)(rad * rad);
                if (dist_sq > max_r_sq) continue;

                float density = (1.0f - dist_sq / max_r_sq) * alpha_fac;
                if (density < 0.08f) continue;

                if (sz > g_depth_buf[py][px] + 0.25f) continue;

                RGB smk_col;
                int c_level = (density > 0.65f) ? 2 : ((density > 0.35f) ? 1 : 0);
                if (g_smoke[i].type == 0) smk_col = PALETTE_STEAM[c_level];
                else if (g_smoke[i].type == 1) smk_col = PALETTE_SMOKE[c_level];
                else smk_col = PALETTE_SOOT_PUFF[c_level];

                static const int bayer2[2][2] = {
                    {0, 2},
                    {3, 1}
                };
                float dither_threshold = (bayer2[py & 1][px & 1] + 0.5f) / 4.0f;

                if (density > dither_threshold || g_frame[py][px].is_sky) {
                    if (g_frame[py][px].is_sky) {
                        g_frame[py][px].color = smk_col;
                        g_frame[py][px].is_sky = false;
                    } else {
                        float a = density * 0.65f;
                        g_frame[py][px].color.r = (uint8_t)(g_frame[py][px].color.r * (1.0f - a) + smk_col.r * a);
                        g_frame[py][px].color.g = (uint8_t)(g_frame[py][px].color.g * (1.0f - a) + smk_col.g * a);
                        g_frame[py][px].color.b = (uint8_t)(g_frame[py][px].color.b * (1.0f - a) + smk_col.b * a);
                    }
                }
            }
        }
    }

    // 5. Rain Drops Streaks
    if (g_rain_active) {
        for (int i = 0; i < MAX_RAIN; i++) {
            if (!g_rain[i].active) continue;
            Vec3 p_rel = vec3_sub(g_rain[i].pos, cam_pos);
            float rz = vec3_dot(p_rel, fwd);
            if (rz < 0.5f) continue;

            int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
            int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
            if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                if (rz < g_depth_buf[sy][sx] + 0.15f) {
                    g_frame[sy][sx].color = COLOR_RAIN;
                    g_frame[sy][sx].is_sky = false;
                    if (sy - 1 >= 0 && rz < g_depth_buf[sy - 1][sx] + 0.15f) {
                        g_frame[sy - 1][sx].color = COLOR_RAIN;
                        g_frame[sy - 1][sx].is_sky = false;
                    }
                }
            }
        }
    }

    // 6. Ash Flakes Falling in 3D
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

    // 7. Sparks Rising
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (g_sparks[i].active) {
            Vec3 p_rel = vec3_sub(g_sparks[i].pos, cam_pos);
            int sx = (int)(((vec3_dot(p_rel, right) / world_w) + 0.5f) * g_pixel_w);
            int sy = (int)((0.5f - (vec3_dot(p_rel, up) / world_h)) * g_pixel_h);
            float spark_z = vec3_dot(p_rel, fwd);
            if (sx >= 0 && sx < g_pixel_w && sy >= 0 && sy < g_pixel_h) {
                if (g_id_buf[sy][sx] == OBJ_ASH_BED || spark_z < g_depth_buf[sy][sx] - 0.06f) {
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

            if (top.is_sky && bot.is_sky) {
                if (!prev_bg_transp) {
                    buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, "\033[49m");
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len, " ");
                continue;
            }

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

    int total_sec = (int)g_sim_time;
    int hrs = total_sec / 3600;
    int mins = (total_sec % 3600) / 60;
    int secs = total_sec % 60;

    const char *stage_name = "1/7: Gravetos e Secagem";
    if (g_sim_time > 2700.0f) stage_name = "7/7: Cinzas e Resfriamento";
    else if (g_sim_time > 2100.0f) stage_name = "6/7: Leito de Brasas";
    else if (g_sim_time > 1600.0f) stage_name = "5/7: Fratura Estrutural e Colapso";
    else if (g_sim_time > 1100.0f) stage_name = "4/7: Incandescência e Deformação";
    else if (g_sim_time > 360.0f) stage_name = "3/7: Fogueira Roaring";
    else if (g_sim_time > 120.0f) stage_name = "2/7: Ignição e Pirólise";

    const char *mode_name = "Fogueira Quadrada";
    if (g_stack_mode == 1) mode_name = "Tenda Cônica";
    else if (g_stack_mode == 2) mode_name = "Pirâmide";

    const char *audio_str = g_audio_mute ? "\033[1;31m[MUTED]\033[0m" : "\033[1;32m[ON]\033[0m";
    const char *weather_str = g_rain_active ? "\033[1;36mChuva 🌧\033[0m" : "\033[1;33mSeco ☀\033[0m";

    if (g_show_hud) {
        buf_len += snprintf(buf + buf_len, sizeof(buf) - buf_len,
            "\033[1;33m[3D Bonfire]\033[0m %02d:%02d:%02d [%s %.1fx] | Pilha: \033[1;32m%s\033[0m | Fase: \033[1;37m%s\033[0m | Som: %s | Clima: %s | [F] Lenha [P] Chuva [M] Áudio [L] Pilha [+/-] Vel [C] Colapso [Q] Sair ",
            hrs, mins, secs, g_realtime_mode ? "Realtime" : "Fast", g_time_scale, mode_name, stage_name, audio_str, weather_str);
    }

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
        } else if (ch == 'f' || ch == 'F') {
            stoke_fire_add_wood();
        } else if (ch == 'p' || ch == 'P') {
            g_rain_active = !g_rain_active;
        } else if (ch == 'm' || ch == 'M') {
            g_audio_mute = !g_audio_mute;
        } else if (ch == 'l' || ch == 'L') {
            g_stack_mode = (g_stack_mode + 1) % 3;
            init_scene();
        } else if (ch == 'x' || ch == 'X') {
            g_realtime_mode = !g_realtime_mode;
            g_time_scale = g_realtime_mode ? 1.0f : 30.0f;
        } else if (ch == 'i' || ch == 'I') {
            g_show_hud = !g_show_hud;
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
            g_time_scale += (g_time_scale < 5.0f) ? 1.0f : 5.0f;
            if (g_time_scale > 60.0f) g_time_scale = 60.0f;
            g_realtime_mode = (fabsf(g_time_scale - 1.0f) < 0.1f);
        } else if (ch == '-' || ch == '_') {
            g_time_scale -= (g_time_scale <= 5.0f) ? 1.0f : 5.0f;
            if (g_time_scale < 0.2f) g_time_scale = 0.2f;
            g_realtime_mode = (fabsf(g_time_scale - 1.0f) < 0.1f);
        }
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--realtime") == 0) {
            g_realtime_mode = true;
            g_time_scale = 1.0f;
        } else if (strcmp(argv[i], "--fast") == 0) {
            g_realtime_mode = false;
            g_time_scale = 30.0f;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_time_scale = (float)atof(argv[++i]);
            g_realtime_mode = (fabsf(g_time_scale - 1.0f) < 0.1f);
        }
    }

    if (argc > 2 && strcmp(argv[1], "--snapshot") == 0) {
        g_pixel_w = 120;
        g_pixel_h = 70;
        if (argc > 6) g_stack_mode = atoi(argv[6]);
        init_scene();
        float target_sim = (argc > 3) ? atof(argv[3]) : 0.0f;
        if (argc > 4) g_cam_yaw = atof(argv[4]) * (float)M_PI / 180.0f;
        if (argc > 5) g_cam_pitch = atof(argv[5]) * (float)M_PI / 180.0f;
        if (argc > 7 && strcmp(argv[7], "rain") == 0) g_rain_active = true;
        g_time_scale = 30.0f; // Fast advance for headless snapshot rendering
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
    start_audio_engine();

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

    stop_audio_engine();
    return 0;
}
