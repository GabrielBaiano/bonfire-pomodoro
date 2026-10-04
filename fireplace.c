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
#include <sys/stat.h>
#include <strings.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "sound_bonfire.h"
#include "sound_ds_ambient.h"
#include "sound_wood_ambient.h"
#include "sound_alert.h"

static bool g_sound_enabled = true;
static int g_sound_volume = 50; // 0-100% volume
static int *g_shared_volume = NULL;

static void play_alert_sound(void);
static void send_system_notification(const char *title, const char *msg);
static void sync_volume_config(void);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_COLS 260
#define MAX_PIXEL_ROWS 180
#define MAX_SPARKS 256
#define MAX_ASH_FLAKES 128
#define MAX_SMOKE 80
#define MAX_LOGS 32
#define MAX_STONES 24
#define NUM_LOG_SEGS 10

typedef struct {
    uint8_t r, g, b;
} RGB;

static const RGB COLOR_BLACK = {0, 0, 0};

typedef struct {
    const char *name;
    RGB palette_wood[7];
    RGB palette_endcap[5];
    float crackle_mult;
    float burn_rate_mult;
} WoodSpecies;

static const WoodSpecies WOOD_SPECIES[4] = {
    {
        "Oak",
        {
            {20, 12, 8}, {42, 27, 18}, {72, 48, 32}, {105, 72, 48}, {138, 96, 64}, {168, 120, 82}, {200, 145, 102}
        },
        {
            {20, 12, 8}, {92, 60, 38}, {130, 88, 56}, {168, 116, 78}, {205, 150, 105}
        },
        0.85f, 1.0f
    },
    {
        "Pine",
        {
            {24, 14, 6}, {52, 32, 16}, {88, 58, 28}, {124, 84, 42}, {162, 114, 58}, {198, 145, 82}, {228, 175, 110}
        },
        {
            {24, 14, 6}, {110, 72, 34}, {152, 104, 52}, {192, 138, 76}, {230, 178, 115}
        },
        2.4f, 1.15f
    },
    {
        "Birch",
        {
            {24, 24, 22}, {60, 60, 56}, {105, 102, 96}, {150, 148, 142}, {192, 190, 184}, {225, 224, 218}, {248, 246, 242}
        },
        {
            {24, 24, 22}, {102, 78, 52}, {145, 114, 78}, {188, 152, 110}, {225, 190, 145}
        },
        1.2f, 1.05f
    },
    {
        "Cherry",
        {
            {24, 10, 10}, {50, 22, 20}, {84, 38, 32}, {120, 56, 48}, {158, 80, 68}, {192, 108, 92}, {225, 140, 120}
        },
        {
            {24, 10, 10}, {104, 48, 40}, {148, 72, 60}, {190, 102, 85}, {228, 145, 122}
        },
        0.9f, 0.95f
    }
};

static int g_wood_type = 0;
static bool g_wood_type_forced = false;
#define PALETTE_WOOD (WOOD_SPECIES[(g_wood_type >= 0 && g_wood_type < 4) ? g_wood_type : 0].palette_wood)
#define PALETTE_ENDCAP (WOOD_SPECIES[(g_wood_type >= 0 && g_wood_type < 4) ? g_wood_type : 0].palette_endcap)

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

// Dark Souls Coiled Sword & Burned Bronze/Iron Alloy Palette
// Aged forged bronze with copper-burnt undertones, reddish-orange heat patina, and fire sheen
static const RGB PALETTE_IRON[] = {
    {38, 20, 14},     // 0: Deep burnt copper/bronze shadow
    {78, 40, 24},     // 1: Dark oxidized bronze
    {138, 72, 36},    // 2: Warm burnt reddish-bronze
    {196, 114, 52},   // 3: Highlighted fire-bronze edge
    {245, 168, 88}    // 4: Warm copper-golden specular gleam
};

// Weathered Pale Bone Palette (Ancient calcified human remains)
static const RGB PALETTE_BONE[] = {
    {28, 22, 18},     // 0: Deep black eye-socket void & nasal cavity
    {95, 84, 72},     // 1: Charred crevice & shadow
    {168, 156, 140},  // 2: Weathered ancient bone
    {218, 210, 194},  // 3: Bleached bone ivory
    {252, 248, 238}   // 4: Chalky bone highlight
};

typedef struct {
    float x, y, z;
} Vec3;

#define NUM_SWORD_BLADE_SEGS 26
#define OBJ_SWORD 800
#define MAX_BONES 80
#define OBJ_BONE_BASE 900

typedef struct {
    Vec3 p1, p2;
    float radius;
    float heat;
} SwordBladeSegment;

typedef struct {
    int obj_id;
    bool active;
    Vec3 root_pos;
    Vec3 axis;
    Vec3 guard_pos;
    Vec3 guard_block_p1, guard_block_p2;
    Vec3 guard_p1, guard_p2;
    Vec3 quillon_p1, quillon_p2;
    Vec3 pommel_pos;
    Vec3 pommel_tip;
    SwordBladeSegment blade_segs[NUM_SWORD_BLADE_SEGS];
} Sword3D;

typedef struct {
    int obj_id;
    Vec3 p1, p2;
    float radius_shaft;
    float radius_joint;
    Vec3 dir;
    float char_amount;
    float heat;
    bool is_skull;
    Vec3 skull_pos;
    float skull_radius;
    Vec3 eye_left;
    Vec3 eye_right;
    float eye_radius;
    Vec3 jaw_pos;
    float jaw_radius;
} Bone3D;

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
    float break_t;                     // Fracture split ratio along the log [0.25..0.75]
    Vec3 break_p1, break_p1_orig, break_p1_target; // Piece 1 broken tip
    Vec3 break_p2, break_p2_orig, break_p2_target; // Piece 2 broken tip
    Vec3 break_v1, break_v2;           // Ragdoll velocities of fractured log pieces
    int break_bounces1, break_bounces2;// Ground impact bounces for each piece
    bool is_falling;                   // Dynamic ragdoll drop state
    Vec3 rest_p1, rest_p2;             // Target settled resting endpoints
    float fall_vy;                     // Vertical velocity (gravity/bounce)
    float fall_rot_y;                  // Rotational tumble angle
    float fall_rot_vy;                 // Angular velocity
    int fall_bounces;                  // Count of impact bounces
    float fall_timer;                  // Fall duration
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

// 3D Logs
static Cylinder3D g_logs[MAX_LOGS];
static int g_num_logs = 5;

typedef enum {
    FIRE_STATE_UNLIT = 0,
    FIRE_STATE_LIT_FOCUS = 1,
    FIRE_STATE_SMOLDERING_REST = 2,
    FIRE_STATE_EXTINGUISHED = 3
} FireState;

typedef enum {
    BANNER_NONE = 0,
    BANNER_LIT = 1,
    BANNER_REST = 2,
    BANNER_LONG_REST = 3,
    BANNER_EXTINGUISHED = 4
} BannerType;

// Dark Souls Coiled Sword (Espada Espiral) & Bone Pile
static bool g_is_dark_souls = false;
static FireState g_fire_state = FIRE_STATE_UNLIT;
static BannerType g_banner_type = BANNER_NONE;
static bool g_bonfire_lit = false;
static float g_ignition_timer = 0.0f;
static float g_banner_timer = 0.0f;
static Sword3D g_sword;
static Bone3D g_bones[MAX_BONES];
static int g_num_bones = 0;

// Pomodoro Focus & Rest cycle management
static float g_focus_duration = 1500.0f;        // 25 min default
static float g_short_break_duration = 300.0f;   // 5 min default
static float g_long_break_duration = 900.0f;    // 15 min default
static int g_sessions_before_long_break = 4;    // 4 sessions default
static float g_rest_duration = 300.0f;          // Active rest tolerance
static bool g_is_long_break = false;            // Current break is long break
static float g_pomodoro_elapsed = 0.0f;         // Seconds elapsed in current state
static int g_pomodoro_cycles_done = 0;          // Completed focus cycles
static bool g_pomodoro_paused = false;
static float g_auto_wood_check_timer = 0.0f;

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
static float g_cycle_duration = 1500.0f; // Sync with focus duration (25 min default)
static bool g_cycle_logged = false;   // Prevents duplicate history logging
static float g_time_scale = 1.0f;     // 1.0 = Realtime, 30.0 = Fast Demo
static bool g_realtime_mode = true;   // Realtime 1.0x vs Fast Demo 30.0x
static float g_wind = 0.0f;
static float g_wind_target = 0.0f;
static float g_wind_turb = 0.0f;
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

static inline Vec3 get_log_segment_pos(const Cylinder3D *c, int s) {
    float t_val = (s + 0.5f) / (float)NUM_LOG_SEGS;
    if (!c->fractured) {
        Vec3 p = vec3_add(c->p1, vec3_scale(c->axis, t_val));
        if (c->sag_amount > 0.01f) {
            p.y -= 4.0f * t_val * (1.0f - t_val) * c->sag_amount;
        }
        return p;
    } else {
        float break_t = (c->break_t > 0.05f) ? c->break_t : 0.5f;
        if (t_val <= break_t) {
            float sub_t = t_val / break_t;
            Vec3 sub_axis = vec3_sub(c->break_p1, c->p1);
            return vec3_add(c->p1, vec3_scale(sub_axis, sub_t));
        } else {
            float sub_t = (t_val - break_t) / (1.0f - break_t);
            Vec3 sub_axis = vec3_sub(c->p2, c->break_p2);
            return vec3_add(c->break_p2, vec3_scale(sub_axis, sub_t));
        }
    }
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
    c->break_t = 0.5f;
    c->is_falling = false;
    c->rest_p1 = p1;
    c->rest_p2 = p2;
    c->fall_vy = 0.0f;
    c->fall_rot_y = 0.0f;
    c->fall_rot_vy = 0.0f;
    c->fall_bounces = 0;
    c->fall_timer = 0.0f;
    c->break_v1 = (Vec3){0.0f, 0.0f, 0.0f};
    c->break_v2 = (Vec3){0.0f, 0.0f, 0.0f};
    c->break_bounces1 = 0;
    c->break_bounces2 = 0;
    Vec3 mid = vec3_scale(vec3_add(p1, p2), 0.5f);
    c->break_p1 = mid;
    c->break_p1_orig = mid;
    c->break_p1_target = (Vec3){ mid.x, -4.2f + radius, mid.z };
    c->break_p2 = mid;
    c->break_p2_orig = mid;
    c->break_p2_target = (Vec3){ mid.x, -4.2f + radius, mid.z };
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

static void get_history_file_path(char *path, size_t max_len) {
    const char *home = getenv("HOME");
    if (home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.config", home);
        mkdir(dir, 0755);
        snprintf(dir, sizeof(dir), "%s/.config/bonfire", home);
        mkdir(dir, 0755);
        snprintf(path, max_len, "%s/.config/bonfire/history.log", home);
    } else {
        snprintf(path, max_len, "bonfires.log");
    }
}

static void log_bonfire_history(int minutes_target, int minutes_burned, const char *mode, bool completed) {
    char path[512];
    get_history_file_path(path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f) f = fopen("bonfires.log", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char date_str[64];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M", t);
        fprintf(f, "[%s] %d min (Planejado: %d min) | Pilha: %s | %s\n",
                date_str, minutes_burned, minutes_target, mode,
                completed ? "Concluída 🔥" : "Interrompida 💨");
        fclose(f);
    }
}

static void print_bonfire_history(void) {
    char path[512];
    get_history_file_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) f = fopen("bonfires.log", "r");
    if (!f) {
        printf("Nenhuma fogueira registrada no histórico ainda.\n");
        return;
    }
    printf("\033[1;33m=== Histórico de Fogueiras ===\033[0m\n\n");
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        printf("  %s", line);
        count++;
    }
    printf("\nTotal: %d fogueiras salvas no histórico.\n", count);
    fclose(f);
}

static void safe_write(int fd, const void *buf, size_t count) {
    ssize_t ret = write(fd, buf, count);
    (void)ret;
}

static pid_t g_ambient_pgid = -1;

static void stop_ambient_sound(void) {
    if (g_ambient_pgid > 0) {
        kill(-g_ambient_pgid, SIGTERM);
        int status;
        waitpid(g_ambient_pgid, &status, WNOHANG);
        g_ambient_pgid = -1;
    }
}

static void start_ambient_sound(bool is_dark_souls, int volume_pct) {
    if (!g_sound_enabled || volume_pct <= 0) return;
    stop_ambient_sound();

    if (g_shared_volume) {
        *g_shared_volume = volume_pct;
    }

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTERM, SIG_DFL);

        int audio_pipe[2];
        if (pipe(audio_pipe) != 0) _exit(1);

        pid_t player = fork();
        if (player == 0) {
            close(audio_pipe[1]);
            if (dup2(audio_pipe[0], STDIN_FILENO) < 0) _exit(1);
            close(audio_pipe[0]);

            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            execlp("pw-play", "pw-play", "--raw", "--rate=22050", "--channels=1", "--format=s16", "-", (char *)NULL);
            execlp("aplay", "aplay", "-q", "-t", "raw", "-r", "22050", "-f", "S16_LE", "-c", "1", "-", (char *)NULL);
            execlp("paplay", "paplay", "--raw", "--rate=22050", "--channels=1", "--format=s16le", "/dev/stdin", (char *)NULL);
            _exit(1);
        }

        if (player < 0) {
            close(audio_pipe[0]);
            close(audio_pipe[1]);
            _exit(1);
        }

        close(audio_pipe[0]);

        const int16_t *src = (const int16_t *)(is_dark_souls ? assets_ds_fire_ambient_pcm : assets_wood_fire_ambient_pcm);
        size_t total_samples = (is_dark_souls ? assets_ds_fire_ambient_pcm_len : assets_wood_fire_ambient_pcm_len) / sizeof(int16_t);

        int16_t chunk[1024];
        while (1) {
            size_t sample_pos = 0;
            while (sample_pos < total_samples) {
                int cur_vol = (g_shared_volume != NULL) ? *g_shared_volume : volume_pct;
                float vol_factor = (float)cur_vol / 100.0f;
                size_t batch = total_samples - sample_pos;
                if (batch > 1024) batch = 1024;
                for (size_t i = 0; i < batch; i++) {
                    chunk[i] = (int16_t)((float)src[sample_pos + i] * vol_factor);
                }
                ssize_t written = write(audio_pipe[1], chunk, batch * sizeof(int16_t));
                if (written <= 0) {
                    close(audio_pipe[1]);
                    waitpid(player, NULL, 0);
                    _exit(0);
                }
                sample_pos += batch;
            }
        }
        _exit(0);
    }

    g_ambient_pgid = pid;
}

static void update_ambient_audio(void) {
    if (!g_sound_enabled || g_sound_volume <= 0 || g_pomodoro_paused || g_fire_state != FIRE_STATE_LIT_FOCUS) {
        if (g_shared_volume) *g_shared_volume = 0;
        stop_ambient_sound();
    } else {
        if (g_shared_volume) *g_shared_volume = g_sound_volume;
        if (g_ambient_pgid <= 0) {
            start_ambient_sound(g_is_dark_souls, g_sound_volume);
        }
    }
}

static void reset_terminal(void) {
    stop_ambient_sound();
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
// STACK GENERATOR: Tenda Cônica (Teepee)
// -----------------------------------------------------------------------------
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

static void init_bone(Bone3D *b, int id, Vec3 p1, Vec3 p2, float r_shaft, float r_joint, float char_amt, float heat) {
    b->obj_id = id;
    b->p1 = p1;
    b->p2 = p2;
    b->radius_shaft = r_shaft;
    b->radius_joint = r_joint;
    Vec3 axis = vec3_sub(p2, p1);
    float len = vec3_len(axis);
    b->dir = (len > 1e-4f) ? vec3_scale(axis, 1.0f / len) : (Vec3){0.0f, 1.0f, 0.0f};
    b->char_amount = char_amt;
    b->heat = heat;
    b->is_skull = false;
    b->skull_pos = (Vec3){0.0f, 0.0f, 0.0f};
    b->skull_radius = 0.0f;
    b->eye_left = (Vec3){0.0f, 0.0f, 0.0f};
    b->eye_right = (Vec3){0.0f, 0.0f, 0.0f};
    b->eye_radius = 0.0f;
    b->jaw_pos = (Vec3){0.0f, 0.0f, 0.0f};
    b->jaw_radius = 0.0f;
}

static void init_skull(Bone3D *b, int id, Vec3 pos, float radius, float yaw, float pitch, float char_amt, float heat) {
    b->obj_id = id;
    b->p1 = pos;
    b->p2 = pos;
    b->radius_shaft = 0.0f;
    b->radius_joint = 0.0f;
    b->char_amount = char_amt;
    b->heat = heat;
    b->is_skull = true;
    b->skull_pos = pos;
    b->skull_radius = radius;

    // Face forward towards camera (-Z in world space) with subtle rotation
    Vec3 fwd = vec3_norm((Vec3){ sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) });
    Vec3 right = vec3_norm(vec3_cross(fwd, (Vec3){0.0f, 1.0f, 0.0f}));
    Vec3 up = vec3_cross(right, fwd);
    b->dir = fwd;

    // Carve recessed dark eye sockets on the frontal bone facing camera
    float eye_spacing = radius * 0.35f;
    float eye_forward = radius * 0.82f;
    float eye_up = radius * 0.10f;

    b->eye_radius = radius * 0.24f;
    b->eye_left = vec3_add(pos, vec3_add(vec3_scale(fwd, eye_forward),
                                 vec3_add(vec3_scale(right, -eye_spacing), vec3_scale(up, eye_up))));
    b->eye_right = vec3_add(pos, vec3_add(vec3_scale(fwd, eye_forward),
                                  vec3_add(vec3_scale(right, eye_spacing), vec3_scale(up, eye_up))));

    // Maxilla and jaw positioned forward and lower (proportional anatomically)
    b->jaw_pos = vec3_add(pos, vec3_add(vec3_scale(fwd, radius * 0.22f), vec3_scale(up, -radius * 0.44f)));
    b->jaw_radius = radius * 0.36f;
}

static void build_dark_souls_scene(void) {
    g_num_stones = 0; // NO stone ring in Dark Souls mode!
    g_num_twigs = 0;
    g_num_leaves = 0;

    // 1. Natural Conical Ash Mound (Base de cinzas e poeira de ossos calcificados)
    g_ash_bed.center = (Vec3){0.0f, -4.20f, 0.0f};
    g_ash_bed.radius_xz = 4.1f;
    g_ash_bed.height = 1.62f; // Summit at y = -2.58f
    g_ash_bed.heat = g_bonfire_lit ? 1.0f : 0.0f;
    g_ash_bed.volume = 0.92f;
    g_burnt_mass = 0.95f;

    // 2. Coiled Sword (Espada Espiral fincada no monte)
    g_sword.obj_id = OBJ_SWORD;
    g_sword.active = true;
    g_sword.root_pos = (Vec3){0.04f, -2.85f, -0.04f}; // Embedded deep into ash core
    g_sword.axis = vec3_norm((Vec3){0.14f, 0.98f, -0.07f}); // Iconic slight Dark Souls tilt

    Vec3 u = g_sword.axis;
    Vec3 w1 = vec3_norm(vec3_cross(u, (Vec3){0.0f, 0.0f, 1.0f}));
    Vec3 w2 = vec3_cross(u, w1);

    float blade_len = 7.6f;
    Vec3 prev_p = g_sword.root_pos;
    for (int i = 0; i < NUM_SWORD_BLADE_SEGS; i++) {
        float s = blade_len * (float)(i + 1) / (float)NUM_SWORD_BLADE_SEGS;
        float radius = 0.40f - 0.12f * (s / blade_len);
        float phase = s * 2.4f + 0.35f;
        float amp = 0.38f * (1.0f - 0.12f * (s / blade_len));
        Vec3 spiral_offset = vec3_add(vec3_scale(w1, amp * sinf(phase)), vec3_scale(w2, amp * cosf(phase)));
        Vec3 curr_p = vec3_add(g_sword.root_pos, vec3_add(vec3_scale(u, s), spiral_offset));

        g_sword.blade_segs[i].p1 = prev_p;
        g_sword.blade_segs[i].p2 = curr_p;
        g_sword.blade_segs[i].radius = radius;
        // Thermal incandescence climbs the coiled sword (intense ember gradient)
        g_sword.blade_segs[i].heat = g_bonfire_lit ? fmaxf(0.0f, fminf(1.0f, (6.6f - s) / 5.2f)) : 0.0f;
        prev_p = curr_p;
    }

    g_sword.guard_pos = vec3_add(g_sword.root_pos, vec3_scale(u, blade_len));
    g_sword.guard_block_p1 = vec3_sub(g_sword.guard_pos, vec3_scale(u, 0.25f));
    g_sword.guard_block_p2 = vec3_add(g_sword.guard_pos, vec3_scale(u, 0.25f));

    g_sword.guard_p1 = vec3_sub(g_sword.guard_pos, vec3_scale(w1, 1.40f));
    g_sword.guard_p2 = vec3_add(g_sword.guard_pos, vec3_scale(w1, 1.40f));

    g_sword.quillon_p1 = vec3_add(g_sword.guard_p1, vec3_add(vec3_scale(u, -0.45f), vec3_scale(w1, -0.22f)));
    g_sword.quillon_p2 = vec3_add(g_sword.guard_p2, vec3_add(vec3_scale(u, -0.45f), vec3_scale(w1, 0.22f)));

    // Long authentic 2-handed greatsword hilt / grip and crown pommel
    g_sword.pommel_pos = vec3_add(g_sword.guard_pos, vec3_scale(u, 2.75f));
    g_sword.pommel_tip = vec3_add(g_sword.pommel_pos, vec3_scale(u, 0.55f));

    // 3. Charred Bonfire Logs/Branches (Galhos carbonizados salientes)
    g_num_logs = 4;
    // Iconic right-slanting charred branch jutting forward-right from reference photo
    init_cylinder(&g_logs[0], 1, (Vec3){0.22f, -2.55f, -0.15f}, (Vec3){2.95f, -4.15f, -1.55f},
                  (Vec3){0.22f, -2.55f, -0.15f}, (Vec3){2.95f, -4.15f, -1.55f}, 0.26f, 0.45f);
    // Left-rear charred branch
    init_cylinder(&g_logs[1], 2, (Vec3){-0.22f, -2.55f, 0.15f}, (Vec3){-2.65f, -4.10f, 1.45f},
                  (Vec3){-0.22f, -2.55f, 0.15f}, (Vec3){-2.65f, -4.10f, 1.45f}, 0.23f, 0.45f);
    // Right-rear branch
    init_cylinder(&g_logs[2], 3, (Vec3){0.18f, -2.60f, 0.25f}, (Vec3){2.35f, -4.10f, 1.60f},
                  (Vec3){0.18f, -2.60f, 0.25f}, (Vec3){2.35f, -4.10f, 1.60f}, 0.21f, 0.45f);
    // Left-front branch
    init_cylinder(&g_logs[3], 4, (Vec3){-0.28f, -2.60f, -0.22f}, (Vec3){-2.25f, -4.12f, -1.50f},
                  (Vec3){-0.28f, -2.60f, -0.22f}, (Vec3){-2.25f, -4.12f, -1.50f}, 0.20f, 0.45f);

    for (int i = 0; i < g_num_logs; i++) {
        g_logs[i].wood_health = 0.0f;
        g_logs[i].charred = 1.0f;
        g_logs[i].ash_amount = 0.45f;
        g_logs[i].snapped = false;
        g_logs[i].fractured = false;
        g_logs[i].is_falling = false;
        g_logs[i].collapse_cur = 1.0f;
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            g_logs[i].segments[s].burn_progress = 0.85f;
            g_logs[i].segments[s].temp = (s < 2) ? 0.70f : 0.20f;
            g_logs[i].segments[s].structural_mass = 0.85f;
            g_logs[i].segments[s].moisture = 0.0f;
            g_logs[i].segments[s].glow_intensity = (s < 2) ? 0.80f : 0.0f;
        }
    }

    // 4. Detailed Bone Pile embedded in and resting prominently on Ash Mound
    // The single iconic Dark Souls human skull, scaled proportionally and resting on the bone pyre
    init_skull(&g_bones[0], OBJ_BONE_BASE + 0, (Vec3){ -0.65f, -2.48f, -1.95f }, 0.82f, 0.12f, -0.04f, 0.12f, 0.20f);

    // Criss-crossing femurs, ribs, and limb bones forming a massive dense bone pyre
    struct { Vec3 p1, p2; float r_s, r_j, chr, ht; } bone_specs[] = {
        // Crossed large femurs cradling the foreground skull from underneath
        { {-2.15f, -3.35f, -1.90f}, {-0.10f, -3.15f, -1.55f}, 0.22f, 0.36f, 0.15f, 0.18f },
        { {-0.15f, -3.15f, -1.90f}, {-2.05f, -3.40f, -1.45f}, 0.22f, 0.36f, 0.15f, 0.18f },

        // Dense limb bones across mound flanks and summit (replacing former extra skulls)
        { { 1.35f, -2.65f, -1.45f}, { 2.25f, -3.35f, -0.95f}, 0.21f, 0.34f, 0.20f, 0.25f },
        { { 0.35f, -2.55f, -0.95f}, { 1.15f, -2.85f, -0.45f}, 0.20f, 0.32f, 0.40f, 0.60f },
        { {-0.75f, -2.45f,  0.85f}, {-1.55f, -3.15f,  1.25f}, 0.20f, 0.32f, 0.45f, 0.40f },
        { { 0.85f, -2.45f,  0.80f}, { 1.65f, -3.15f,  1.20f}, 0.20f, 0.32f, 0.40f, 0.40f },

        // Crossed femurs on the right slope
        { { 2.35f, -3.35f, -1.75f}, { 0.65f, -3.15f, -1.25f}, 0.22f, 0.38f, 0.18f, 0.22f },
        { { 0.75f, -3.15f, -1.80f}, { 2.45f, -3.40f, -1.15f}, 0.22f, 0.38f, 0.18f, 0.22f },

        // Curved ribcage arches wrapping out of the ash bed around the sword base
        { {-1.05f, -2.15f, -0.65f}, { 1.05f, -2.15f, -0.70f}, 0.20f, 0.32f, 0.60f, 0.85f },
        { {-1.15f, -2.18f, -0.25f}, { 1.15f, -2.18f, -0.20f}, 0.20f, 0.32f, 0.65f, 0.85f },
        { {-1.05f, -2.15f,  0.35f}, { 1.05f, -2.15f,  0.40f}, 0.20f, 0.32f, 0.70f, 0.85f },
        { {-0.90f, -2.25f,  0.75f}, { 0.90f, -2.25f,  0.80f}, 0.20f, 0.32f, 0.55f, 0.75f },

        // Outer radiating long bones down the front mound slope
        { {-0.45f, -2.40f, -1.25f}, {-1.75f, -3.45f, -2.35f}, 0.22f, 0.36f, 0.18f, 0.18f },
        { { 0.45f, -2.40f, -1.15f}, { 1.65f, -3.45f, -2.25f}, 0.22f, 0.36f, 0.18f, 0.18f },
        { {-0.30f, -2.55f, -1.60f}, { 0.35f, -2.80f, -2.10f}, 0.21f, 0.34f, 0.15f, 0.15f },
        { {-1.45f, -3.25f, -1.85f}, {-0.25f, -3.60f, -2.55f}, 0.20f, 0.32f, 0.15f, 0.15f },
        { { 0.25f, -3.60f, -2.55f}, { 1.45f, -3.25f, -1.85f}, 0.20f, 0.32f, 0.15f, 0.15f },
        { {-0.85f, -3.35f, -2.20f}, {-1.85f, -3.65f, -2.80f}, 0.20f, 0.32f, 0.14f, 0.14f },
        { { 0.85f, -3.35f, -2.20f}, { 1.85f, -3.65f, -2.80f}, 0.20f, 0.32f, 0.14f, 0.14f },
        { {-0.20f, -2.85f, -2.05f}, { 0.15f, -3.45f, -2.75f}, 0.21f, 0.34f, 0.15f, 0.15f },

        // Lateral and flank mound collar bones
        { { 2.10f, -3.35f, -0.45f}, { 3.15f, -3.85f,  0.35f}, 0.21f, 0.34f, 0.20f, 0.20f },
        { {-2.10f, -3.35f, -0.45f}, {-3.15f, -3.85f,  0.35f}, 0.21f, 0.34f, 0.20f, 0.20f },
        { { 2.50f, -3.45f, -1.15f}, { 3.35f, -3.90f, -0.35f}, 0.20f, 0.32f, 0.18f, 0.18f },
        { {-2.50f, -3.45f, -1.15f}, {-3.35f, -3.90f, -0.35f}, 0.20f, 0.32f, 0.18f, 0.18f },
        { { 1.85f, -2.75f,  0.15f}, { 2.85f, -3.55f,  0.95f}, 0.21f, 0.34f, 0.25f, 0.25f },
        { {-1.85f, -2.75f,  0.15f}, {-2.85f, -3.55f,  0.95f}, 0.21f, 0.34f, 0.25f, 0.25f },

        // Rear slope and summit long bones
        { {-1.25f, -2.35f,  0.75f}, { 0.15f, -2.15f,  1.10f}, 0.22f, 0.36f, 0.35f, 0.45f },
        { { 1.25f, -2.35f,  0.65f}, {-0.15f, -2.15f,  1.05f}, 0.22f, 0.36f, 0.35f, 0.45f },
        { {-0.35f, -2.30f,  0.85f}, {-1.85f, -3.35f,  1.55f}, 0.21f, 0.34f, 0.30f, 0.30f },
        { { 0.35f, -2.30f,  0.85f}, { 1.85f, -3.35f,  1.55f}, 0.21f, 0.34f, 0.30f, 0.30f },
        { {-1.65f, -3.45f,  1.15f}, {-2.45f, -3.80f,  1.95f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { { 1.65f, -3.45f,  1.15f}, { 2.45f, -3.80f,  1.95f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { {-0.65f, -3.65f,  2.15f}, { 0.65f, -3.65f,  2.25f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { {-1.35f, -3.20f,  1.85f}, {-0.25f, -3.75f,  2.65f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { { 1.35f, -3.20f,  1.85f}, { 0.25f, -3.75f,  2.65f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { {-2.25f, -3.65f, -0.65f}, {-2.85f, -3.82f,  0.55f}, 0.19f, 0.30f, 0.18f, 0.18f },
        { { 2.25f, -3.65f, -0.65f}, { 2.85f, -3.82f,  0.55f}, 0.19f, 0.30f, 0.18f, 0.18f },
        { {-0.95f, -3.05f, -1.45f}, { 0.05f, -3.15f, -1.85f}, 0.20f, 0.32f, 0.20f, 0.20f },
        { { 0.95f, -3.05f, -1.35f}, {-0.05f, -3.15f, -1.85f}, 0.20f, 0.32f, 0.20f, 0.20f },
        { {-0.55f, -2.65f, -0.95f}, { 0.55f, -2.65f, -0.90f}, 0.21f, 0.34f, 0.35f, 0.40f },
        { {-1.15f, -2.75f,  0.45f}, {-0.25f, -3.05f,  0.85f}, 0.20f, 0.32f, 0.25f, 0.25f },
        { { 1.15f, -2.75f,  0.45f}, { 0.25f, -3.05f,  0.85f}, 0.20f, 0.32f, 0.25f, 0.25f },
        { {-0.65f, -2.85f,  1.25f}, { 0.65f, -2.85f,  1.30f}, 0.20f, 0.32f, 0.25f, 0.25f },
        { {-1.85f, -3.05f,  0.65f}, {-0.95f, -2.45f,  0.15f}, 0.20f, 0.32f, 0.25f, 0.25f },
        { { 1.85f, -3.05f,  0.65f}, { 0.95f, -2.45f,  0.15f}, 0.20f, 0.32f, 0.25f, 0.25f },
        { {-2.65f, -3.75f, -0.15f}, {-1.85f, -3.35f, -0.95f}, 0.19f, 0.30f, 0.18f, 0.18f },
        { { 2.65f, -3.75f, -0.15f}, { 1.85f, -3.35f, -0.95f}, 0.19f, 0.30f, 0.18f, 0.18f },
        { {-1.15f, -3.45f, -1.45f}, {-2.15f, -3.85f, -1.05f}, 0.19f, 0.30f, 0.16f, 0.16f },
        { { 1.15f, -3.45f, -1.45f}, { 2.15f, -3.85f, -1.05f}, 0.19f, 0.30f, 0.16f, 0.16f },
        { {-0.15f, -2.45f, -0.75f}, {-0.45f, -2.95f, -1.25f}, 0.21f, 0.34f, 0.30f, 0.40f },
        { { 0.15f, -2.45f, -0.75f}, { 0.45f, -2.95f, -1.25f}, 0.21f, 0.34f, 0.30f, 0.40f },
        { {-0.85f, -3.15f, -0.35f}, {-1.45f, -3.55f, -0.85f}, 0.20f, 0.32f, 0.20f, 0.20f },
        { { 0.85f, -3.15f, -0.35f}, { 1.45f, -3.55f, -0.85f}, 0.20f, 0.32f, 0.20f, 0.20f },
        { {-0.45f, -3.35f,  0.45f}, {-1.15f, -3.65f,  1.05f}, 0.19f, 0.30f, 0.20f, 0.20f },
        { { 0.45f, -3.35f,  0.45f}, { 1.15f, -3.65f,  1.05f}, 0.19f, 0.30f, 0.20f, 0.20f }
    };
    int num_specs = (int)(sizeof(bone_specs) / sizeof(bone_specs[0]));
    g_num_bones = 1 + num_specs;
    if (g_num_bones > MAX_BONES) g_num_bones = MAX_BONES;

    for (int i = 0; i < num_specs && (1 + i) < MAX_BONES; i++) {
        init_bone(&g_bones[1 + i], OBJ_BONE_BASE + 1 + i,
                  bone_specs[i].p1, bone_specs[i].p2,
                  bone_specs[i].r_s, bone_specs[i].r_j,
                  bone_specs[i].chr, bone_specs[i].ht);
    }
}

static void init_scene(void) {
    memset(g_fire_heat, 0, sizeof(g_fire_heat));
    memset(g_next_fire, 0, sizeof(g_next_fire));
    memset(g_fire_z, 0, sizeof(g_fire_z));
    memset(g_sparks, 0, sizeof(g_sparks));
    memset(g_ash_flakes, 0, sizeof(g_ash_flakes));
    memset(g_smoke, 0, sizeof(g_smoke));

    g_bonfire_lit = (g_fire_state == FIRE_STATE_LIT_FOCUS || g_fire_state == FIRE_STATE_SMOLDERING_REST);
    g_ash_bed.center = (Vec3){0.0f, -4.2f, 0.0f};
    g_ash_bed.radius_xz = 4.8f;
    g_ash_bed.height = 0.35f;
    g_ash_bed.heat = g_bonfire_lit ? (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 0.35f : 1.0f) : 0.0f;
    g_ash_bed.volume = 0.0f;
    g_burnt_mass = 0.0f;

    g_sim_time = 0.0f;
    g_anim_time = 0.0f;
    g_collapse_progress = 0.0f;
    g_force_collapse = false;

    if (!g_wood_type_forced) {
        g_wood_type = rand_range(0, 3);
    }

    if (g_is_dark_souls) {
        build_dark_souls_scene();
    } else {
        g_num_bones = 0;
        build_stone_ring();
        build_kindling();
        build_stack_teepee();
    }
}

static bool intersect_capsule(Vec3 p1, Vec3 p2, float radius, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm) {
    Vec3 d = vec3_sub(p2, p1);
    float len = vec3_len(d);
    if (len < 1e-4f) {
        Vec3 oc = vec3_sub(ro, p1);
        float b = vec3_dot(oc, rd);
        float c = vec3_dot(oc, oc) - radius * radius;
        float disc = b * b - c;
        if (disc < 0.0f) return false;
        float sdisc = sqrtf(disc);
        float t = -b - sdisc;
        if (t < 0.1f) t = -b + sdisc;
        if (t < 0.1f) return false;
        *out_t = t;
        *out_pt = vec3_add(ro, vec3_scale(rd, t));
        *out_norm = vec3_norm(vec3_sub(*out_pt, p1));
        return true;
    }
    Vec3 dir = vec3_scale(d, 1.0f / len);
    Vec3 rc = vec3_sub(ro, p1);
    Vec3 d_proj = vec3_sub(rd, vec3_scale(dir, vec3_dot(rd, dir)));
    Vec3 rc_proj = vec3_sub(rc, vec3_scale(dir, vec3_dot(rc, dir)));
    float a = vec3_dot(d_proj, d_proj);
    float b = 2.0f * vec3_dot(d_proj, rc_proj);
    float c = vec3_dot(rc_proj, rc_proj) - radius * radius;
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};

    if (a > 1e-6f) {
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            float sdisc = sqrtf(disc);
            float t0 = (-b - sdisc) / (2.0f * a);
            float t1 = (-b + sdisc) / (2.0f * a);
            float t = (t0 > 0.1f) ? t0 : t1;
            if (t > 0.1f) {
                Vec3 pt = vec3_add(ro, vec3_scale(rd, t));
                float h = vec3_dot(vec3_sub(pt, p1), dir);
                if (h >= 0.0f && h <= len) {
                    best_t = t;
                    best_pt = pt;
                    Vec3 ax = vec3_add(p1, vec3_scale(dir, h));
                    best_norm = vec3_norm(vec3_sub(pt, ax));
                    hit = true;
                }
            }
        }
    }
    for (int cap = 0; cap < 2; cap++) {
        Vec3 cp = cap ? p2 : p1;
        Vec3 oc = vec3_sub(ro, cp);
        float b2 = vec3_dot(oc, rd);
        float c2 = vec3_dot(oc, oc) - radius * radius;
        float disc2 = b2 * b2 - c2;
        if (disc2 >= 0.0f) {
            float sdisc2 = sqrtf(disc2);
            float t0 = -b2 - sdisc2;
            float t1 = -b2 + sdisc2;
            float t = (t0 > 0.1f) ? t0 : t1;
            if (t > 0.1f && t < best_t) {
                best_t = t;
                best_pt = vec3_add(ro, vec3_scale(rd, t));
                best_norm = vec3_norm(vec3_sub(best_pt, cp));
                hit = true;
            }
        }
    }
    if (hit) {
        *out_t = best_t;
        *out_pt = best_pt;
        *out_norm = best_norm;
        return true;
    }
    return false;
}

static bool intersect_bone(const Bone3D *b, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm, float *out_char, float *out_heat) {
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float char_val = b->char_amount;

    if (b->is_skull) {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(b->skull_pos, vec3_add(b->skull_pos, (Vec3){0.0f, 0.12f, 0.0f}), b->skull_radius, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) {
                best_t = t;
                best_pt = pt;
                best_norm = norm;
                hit = true;
            }
        }
        if (b->jaw_radius > 0.01f) {
            if (intersect_capsule(b->jaw_pos, vec3_add(b->jaw_pos, (Vec3){0.0f, 0.08f, 0.0f}), b->jaw_radius, ro, rd, &t, &pt, &norm)) {
                if (t < best_t) {
                    best_t = t;
                    best_pt = pt;
                    best_norm = norm;
                    hit = true;
                }
            }
        }
        // Deep sunken eye sockets & nasal cavity on the frontal face
        if (hit && b->skull_radius > 0.01f) {
            Vec3 delta = vec3_sub(best_pt, b->skull_pos);
            float pfwd = vec3_dot(delta, b->dir);
            if (pfwd > b->skull_radius * 0.15f) {
                Vec3 s_up = (Vec3){0.0f, 1.0f, 0.0f};
                Vec3 s_right = vec3_norm(vec3_cross(b->dir, s_up));
                Vec3 s_true_up = vec3_cross(s_right, b->dir);
                float pright = vec3_dot(delta, s_right);
                float pup = vec3_dot(delta, s_true_up);

                // Left and right eye orbits (prominent Dark Souls hollow sockets with clean separation)
                float dy_eye = pup - b->skull_radius * 0.12f;
                float dx_l = pright - (-b->skull_radius * 0.36f);
                float dx_r = pright - (b->skull_radius * 0.36f);
                float eye_r_sq = (b->skull_radius * 0.21f) * (b->skull_radius * 0.21f);

                // Nasal aperture (pyriform cavity centered below eye orbits)
                float dy_nose = pup - (-b->skull_radius * 0.14f);
                float dx_nose = pright;
                float nose_metric = dx_nose * dx_nose * 3.6f + dy_nose * dy_nose;
                float nose_r_sq = (b->skull_radius * 0.15f) * (b->skull_radius * 0.15f);

                if (dx_l * dx_l + dy_eye * dy_eye < eye_r_sq ||
                    dx_r * dx_r + dy_eye * dy_eye < eye_r_sq ||
                    nose_metric < nose_r_sq) {
                    best_norm = vec3_scale(b->dir, -1.0f); // inward recessed void
                    char_val = 1.0f; // eye socket / nasal cavity void shadow
                } else {
                    // Maxillary dental arch and tooth gaps
                    float dy_mouth = pup - (-b->skull_radius * 0.38f);
                    if (fabsf(dy_mouth) < b->skull_radius * 0.10f && fabsf(pright) < b->skull_radius * 0.34f) {
                        float tooth_phase = cosf(pright / (b->skull_radius * 0.065f) * (float)M_PI);
                        if (fabsf(dy_mouth) < b->skull_radius * 0.025f || tooth_phase < -0.30f) {
                            best_norm = vec3_scale(b->dir, -1.0f);
                            char_val = 0.95f; // dental fissure shadow
                        }
                    }
                }
            }
        }
    } else {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(b->p1, b->p2, b->radius_shaft, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) {
                best_t = t;
                best_pt = pt;
                best_norm = norm;
                hit = true;
            }
        }
        // Double condyle joints at p1
        Vec3 side = vec3_norm(vec3_cross(b->dir, (Vec3){0.0f, 1.0f, 0.0f}));
        Vec3 j1a = vec3_add(b->p1, vec3_scale(side, b->radius_joint * 0.50f));
        Vec3 j1b = vec3_sub(b->p1, vec3_scale(side, b->radius_joint * 0.50f));
        if (intersect_capsule(j1a, b->p1, b->radius_joint * 0.72f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; hit = true; }
        }
        if (intersect_capsule(j1b, b->p1, b->radius_joint * 0.72f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; hit = true; }
        }

        // Double condyle joints at p2
        Vec3 j2a = vec3_add(b->p2, vec3_scale(side, b->radius_joint * 0.50f));
        Vec3 j2b = vec3_sub(b->p2, vec3_scale(side, b->radius_joint * 0.50f));
        if (intersect_capsule(j2a, b->p2, b->radius_joint * 0.72f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; hit = true; }
        }
        if (intersect_capsule(j2b, b->p2, b->radius_joint * 0.72f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; hit = true; }
        }
    }

    if (hit) {
        *out_t = best_t;
        *out_pt = best_pt;
        *out_norm = best_norm;
        *out_char = char_val;
        *out_heat = b->heat;
        return true;
    }
    return false;
}

static bool intersect_sword(const Sword3D *sw, Vec3 ro, Vec3 rd, float *out_t, Vec3 *out_pt, Vec3 *out_norm, float *out_heat, int *out_part) {
    if (!sw->active) return false;

    // Fast bounding capsule test enclosing entire sword (blade, guard, quillons, grip, pommel)
    // Radius 1.85f strictly covers the widest quillons and spiral offsets.
    float b_t;
    Vec3 b_pt, b_norm;
    if (!intersect_capsule(sw->root_pos, sw->pommel_tip, 1.85f, ro, rd, &b_t, &b_pt, &b_norm)) {
        return false;
    }

    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float best_heat = 0.0f;
    int best_part = 0;

    // 1. Undulating blade segments
    for (int i = 0; i < NUM_SWORD_BLADE_SEGS; i++) {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(sw->blade_segs[i].p1, sw->blade_segs[i].p2, sw->blade_segs[i].radius, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) {
                best_t = t;
                best_pt = pt;
                best_norm = norm;
                best_heat = sw->blade_segs[i].heat;
                best_part = 0;
                hit = true;
            }
        }
    }

    // 2. Crossguard (Central hub block + Wide crossbar + Angled quillons)
    {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(sw->guard_block_p1, sw->guard_block_p2, 0.38f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; best_heat = 0.0f; best_part = 1; hit = true; }
        }
        if (intersect_capsule(sw->guard_p1, sw->guard_p2, 0.28f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; best_heat = 0.0f; best_part = 1; hit = true; }
        }
        if (intersect_capsule(sw->guard_p1, sw->quillon_p1, 0.22f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; best_heat = 0.0f; best_part = 1; hit = true; }
        }
        if (intersect_capsule(sw->guard_p2, sw->quillon_p2, 0.22f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) { best_t = t; best_pt = pt; best_norm = norm; best_heat = 0.0f; best_part = 1; hit = true; }
        }
    }

    // 3. 2-Handed Grip / Hilt
    {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(sw->guard_pos, sw->pommel_pos, 0.22f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) {
                best_t = t;
                best_pt = pt;
                best_norm = norm;
                best_heat = 0.0f;
                best_part = 2;
                hit = true;
            }
        }
    }

    // 4. Crown / Diamond Pommel
    {
        float t;
        Vec3 pt, norm;
        if (intersect_capsule(sw->pommel_pos, sw->pommel_tip, 0.38f, ro, rd, &t, &pt, &norm)) {
            if (t < best_t) {
                best_t = t;
                best_pt = pt;
                best_norm = norm;
                best_heat = 0.0f;
                best_part = 3;
                hit = true;
            }
        }
    }

    if (hit) {
        *out_t = best_t;
        *out_pt = best_pt;
        *out_norm = best_norm;
        *out_heat = best_heat;
        *out_part = best_part;
        return true;
    }
    return false;
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

    // Fractured cylinder: test both independent sub-halves
    float best_t = 1e9f;
    bool hit = false;
    Vec3 best_pt = {0,0,0}, best_norm = {0,1,0};
    float best_u = 0.0f, best_v = 0.0f, best_rf = 1.0f;
    bool best_cap = false;
    int best_seg = 0;

    float break_t = (c->break_t > 0.05f) ? c->break_t : 0.5f;
    int break_seg_split = (int)(break_t * NUM_LOG_SEGS);
    if (break_seg_split < 1) break_seg_split = 1;
    if (break_seg_split >= NUM_LOG_SEGS - 1) break_seg_split = NUM_LOG_SEGS - 2;

    // Sub A: p1 -> break_p1
    float tA, uA, vA, rfA;
    Vec3 ptA, normA;
    bool capA;
    if (intersect_sub_cylinder(c->p1, c->break_p1, c->radius * 0.95f, 0.0f, ray_orig, ray_dir,
                               &tA, &ptA, &normA, &uA, &vA, &capA, &rfA, true, true)) {
        if (tA < best_t) {
            best_t = tA; best_pt = ptA; best_norm = normA;
            best_u = uA; best_v = vA * break_t; best_cap = capA; best_rf = rfA;
            best_seg = (int)(vA * break_seg_split);
            if (best_seg >= break_seg_split) best_seg = break_seg_split - 1;
            hit = true;
        }
    }

    // Sub B: break_p2 -> p2
    float tB, uB, vB, rfB;
    Vec3 ptB, normB;
    bool capB;
    if (intersect_sub_cylinder(c->break_p2, c->p2, c->radius * 0.95f, 0.0f, ray_orig, ray_dir,
                               &tB, &ptB, &normB, &uB, &vB, &capB, &rfB, true, true)) {
        if (tB < best_t) {
            best_t = tB; best_pt = ptB; best_norm = normB;
            best_u = uB; best_v = break_t + vB * (1.0f - break_t); best_cap = capB; best_rf = rfB;
            best_seg = break_seg_split + (int)(vB * (NUM_LOG_SEGS - break_seg_split));
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

static bool stoke_fire_add_wood(void) {
    // 1. Cannot throw a new log while another is still falling in the air
    for (int i = 0; i < g_num_logs; i++) {
        if (g_logs[i].is_falling) return false;
    }

    // 2. Assess current fire intensity and active unburnt wood capacity
    float fire_heat = g_ash_bed.heat;
    int unburnt_logs = 0;
    for (int i = 0; i < g_num_logs; i++) {
        float mass = g_logs[i].segments[4].structural_mass;
        float burn = g_logs[i].segments[4].burn_progress;
        if (mass > 0.35f && burn < 0.65f) {
            unburnt_logs++;
        }
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            if (g_logs[i].segments[s].temp > 0.25f) {
                fire_heat = fmaxf(fire_heat, g_logs[i].segments[s].temp);
            }
        }
    }

    // Dynamic capacity: roaring fire can take 7 logs, moderate fire 5-6, dying fire 4
    int max_capacity = 4;
    if (fire_heat > 0.60f) max_capacity = 7;
    else if (fire_heat > 0.30f) max_capacity = 6;
    else if (fire_heat > 0.15f) max_capacity = 5;

    // If completely dead cold, cannot catch new log
    if (fire_heat < 0.08f && g_sim_time > 100.0f) {
        return false;
    }

    if (unburnt_logs >= max_capacity) {
        // Fire is full! Just gently stoke the existing ember bed without adding more wood
        g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.15f);
        Vec3 hearth_c = (Vec3){0.0f, -3.5f, 0.0f};
        for (int sp = 0; sp < 15; sp++) {
            Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.0f, rand_f() * 3.5f + 1.5f, (rand_f() - 0.5f) * 2.0f};
            spawn_spark_3d(hearth_c, sp_v, rand_range(15, 35), PALETTE_EMBERS[rand_range(2, 4)]);
        }
        return false;
    }

    // Revive kindling/embers if low
    g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.30f);
    for (int i = 0; i < g_num_twigs; i++) {
        if (!g_twigs[i].active || g_twigs[i].burn_progress > 0.85f) {
            g_twigs[i].active = true;
            g_twigs[i].burn_progress = 0.0f;
            g_twigs[i].moisture = 0.05f;
            g_twigs[i].temp = 0.45f;
        }
    }

    // 3. Find the best slot for the new log
    int target_slot = -1;
    if (g_num_logs < MAX_LOGS) {
        target_slot = g_num_logs;
        g_num_logs++;
    } else {
        float min_mass = 999.0f;
        int min_idx = -1;
        for (int i = 0; i < g_num_logs; i++) {
            float m = 0.0f;
            for (int s = 0; s < NUM_LOG_SEGS; s++) m += g_logs[i].segments[s].structural_mass;
            if (m < min_mass) {
                min_mass = m;
                min_idx = i;
            }
        }
        if (min_idx >= 0) target_slot = min_idx;
    }

    if (target_slot < 0) return false;

    // 4. Calculate non-overlapping Teepee angle (furthest angular gap from standing logs)
    float best_angle = rand_f() * 6.2831853f;
    float max_min_dist = -1.0f;
    for (int cand = 0; cand < 16; cand++) {
        float test_angle = cand * (6.2831853f / 16.0f) + (rand_f() - 0.5f) * 0.15f;
        float min_d = 999.0f;
        for (int i = 0; i < g_num_logs; i++) {
            if (i == target_slot) continue;
            if (g_logs[i].segments[4].structural_mass < 0.20f) continue;
            float log_ang = atan2f(g_logs[i].p1_orig.z, g_logs[i].p1_orig.x);
            float diff = fabsf(test_angle - log_ang);
            if (diff > (float)M_PI) diff = 2.0f * (float)M_PI - diff;
            if (diff < min_d) min_d = diff;
        }
        if (min_d > max_min_dist) {
            max_min_dist = min_d;
            best_angle = test_angle;
        }
    }

    float ground_y = -4.2f;
    float radius = 0.95f + rand_f() * 0.22f;
    float contact_r = 4.25f + 0.20f * (rand_f() - 0.5f);
    float apex_r = 0.65f + 0.10f * (rand_f() - 0.5f);
    float apex_y = 2.30f + 0.20f * (rand_f() - 0.5f);

    Vec3 rest_p1 = (Vec3){ contact_r * cosf(best_angle), ground_y - 0.40f, contact_r * sinf(best_angle) };
    Vec3 rest_p2 = (Vec3){ apex_r * cosf(best_angle), apex_y, apex_r * sinf(best_angle) };
    Vec3 p2_coll = (Vec3){ apex_r * 0.35f * cosf(best_angle), ground_y + radius * 0.70f, apex_r * 0.35f * sinf(best_angle) };

    // Initial Ragdoll Drop Position high above the hearth
    float spawn_drop_h = 7.2f + rand_f() * 1.5f;
    Vec3 spawn_p1 = (Vec3){ rest_p1.x + (rand_f() - 0.5f) * 0.8f, rest_p1.y + spawn_drop_h, rest_p1.z + (rand_f() - 0.5f) * 0.8f };
    Vec3 spawn_p2 = (Vec3){ rest_p2.x + (rand_f() - 0.5f) * 0.8f, rest_p2.y + spawn_drop_h + 1.2f, rest_p2.z + (rand_f() - 0.5f) * 0.8f };

    init_cylinder(&g_logs[target_slot], 100 + target_slot, spawn_p1, spawn_p2, rest_p1, p2_coll, radius, 0.0f);
    g_logs[target_slot].p1_orig = rest_p1;
    g_logs[target_slot].p2_orig = rest_p2;
    g_logs[target_slot].rest_p1 = rest_p1;
    g_logs[target_slot].rest_p2 = rest_p2;
    g_logs[target_slot].is_falling = true;
    g_logs[target_slot].fall_vy = -3.8f;
    g_logs[target_slot].fall_rot_y = (rand_f() - 0.5f) * 0.5f;
    g_logs[target_slot].fall_rot_vy = (rand_f() - 0.5f) * 2.5f;
    g_logs[target_slot].fall_bounces = 0;
    g_logs[target_slot].fall_timer = 0.0f;

    for (int s = 0; s < NUM_LOG_SEGS; s++) {
        g_logs[target_slot].segments[s].moisture = 0.16f;
        g_logs[target_slot].segments[s].temp = 0.05f;
    }

    return true;
}

static void update_simulation(void) {
    float real_dt = 0.025f * g_time_scale;

    // Pomodoro lifecycle progression
    if (!g_pomodoro_paused) {
        if (g_fire_state == FIRE_STATE_LIT_FOCUS) {
            g_pomodoro_elapsed += real_dt;
            if (g_pomodoro_elapsed >= g_focus_duration) {
                // Focus time ended -> Enter Smoldering Rest (brasa curta)
                g_fire_state = FIRE_STATE_SMOLDERING_REST;
                g_pomodoro_elapsed = 0.0f;
                g_pomodoro_cycles_done++;
                if (g_sessions_before_long_break > 0 && (g_pomodoro_cycles_done % g_sessions_before_long_break == 0)) {
                    g_is_long_break = true;
                    g_rest_duration = g_long_break_duration;
                    g_banner_type = BANNER_LONG_REST;
                } else {
                    g_is_long_break = false;
                    g_rest_duration = g_short_break_duration;
                    g_banner_type = BANNER_REST;
                }
                g_banner_timer = 4.0f;
                play_alert_sound();
                send_system_notification("Bonfire Pomodoro", g_is_long_break ? "Focus complete! Time for a long rest." : "Focus complete! Rest at the bonfire.");
                if (!g_cycle_logged) {
                    g_cycle_logged = true;
                    int target_min = (int)(g_focus_duration / 60.0f);
                    char mode_desc[64];
                    if (g_is_dark_souls) {
                        snprintf(mode_desc, sizeof(mode_desc), "Dark Souls Bonfire (Session %02d)", g_pomodoro_cycles_done);
                    } else {
                        snprintf(mode_desc, sizeof(mode_desc), "Teepee %s (Session %02d)", WOOD_SPECIES[g_wood_type].name, g_pomodoro_cycles_done);
                    }
                    log_bonfire_history(target_min, target_min, mode_desc, true);
                }
            }
        } else if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
            g_pomodoro_elapsed += real_dt;
            if (g_pomodoro_elapsed >= g_rest_duration) {
                // User neglected to stoke/rekindle embers in time -> Fire dies completely!
                g_fire_state = FIRE_STATE_EXTINGUISHED;
                g_bonfire_lit = false;
                g_ash_bed.heat = 0.0f;
                g_banner_type = BANNER_EXTINGUISHED;
                g_banner_timer = 4.5f;
                play_alert_sound();
                send_system_notification("Bonfire Pomodoro", "Rest ended! Kindle the flame to begin your next focus session.");
            }
        }
    }

    // Auto-replenish wood during active focus in standard fireplace mode
    if (!g_is_dark_souls && g_fire_state == FIRE_STATE_LIT_FOCUS && !g_pomodoro_paused) {
        g_auto_wood_check_timer += real_dt;
        if (g_auto_wood_check_timer >= 2.0f) {
            g_auto_wood_check_timer = 0.0f;
            float total_wood_mass = 0.0f;
            for (int i = 0; i < g_num_logs; i++) {
                total_wood_mass += g_logs[i].segments[4].structural_mass;
            }
            float rem_focus = fmaxf(0.0f, g_focus_duration - g_pomodoro_elapsed);
            if (total_wood_mass < 2.0f && rem_focus > 45.0f) {
                stoke_fire_add_wood();
            }
        }
    }

    // Physical time dimensionalization:
    // Full lifecycle (0 to 3000s) maps directly to g_focus_duration real seconds
    float rate_mult = (g_focus_duration > 0.0f) ? (3000.0f / g_focus_duration) : 1.0f;
    float dt = 0.025f * g_time_scale * rate_mult;
    if (g_fire_state == FIRE_STATE_LIT_FOCUS || g_fire_state == FIRE_STATE_SMOLDERING_REST) {
        g_sim_time += dt;
    }
    g_anim_time += 0.025f;

    if (g_banner_timer > 0.0f) {
        g_banner_timer -= 0.025f * g_time_scale;
        if (g_banner_timer < 0.0f) g_banner_timer = 0.0f;
    }
    if (g_ignition_timer > 0.0f) {
        g_ignition_timer += 0.025f * g_time_scale;
        if (g_ignition_timer > 3.0f) g_ignition_timer = 3.0f;
    }

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

    if (!g_is_dark_souls) for (int step = 0; step < num_substeps; step++) {
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
                Vec3 seg_p = get_log_segment_pos(&g_logs[i], s);

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
                            Vec3 pj = get_log_segment_pos(&g_logs[j], sj);
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
                        burn_rate = 0.00035f * (g_logs[i].segments[s].temp - 0.25f) * (0.25f + 0.75f * eta_r) * WOOD_SPECIES[g_wood_type].burn_rate_mult * step_dt;
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

                // Fuel depletion & terminal cooling
                if (g_logs[i].segments[s].burn_progress > 0.85f || cur_sim_t > 2400.0f) {
                    float cool_rate = (cur_sim_t > 2700.0f) ? 0.0030f : 0.0008f;
                    g_logs[i].segments[s].temp = fmaxf(0.0f, g_logs[i].segments[s].temp - cool_rate * step_dt);
                }
                if (g_logs[i].segments[s].structural_mass <= 0.02f) {
                    g_logs[i].segments[s].temp = fmaxf(0.0f, g_logs[i].segments[s].temp - 0.0020f * step_dt);
                }
                if (cur_sim_t >= 3000.0f) {
                    g_logs[i].segments[s].temp = 0.0f;
                    g_logs[i].segments[s].glow_intensity = 0.0f;
                }
                if (g_logs[i].segments[s].temp < 0.0f) g_logs[i].segments[s].temp = 0.0f;

                // 8. Air Draft & Coal Incandescence
                float v_air = eta_r * (0.75f + 0.50f * g_logs[i].segments[s].temp) + 0.60f * fabsf(g_wind_turb);
                float glow = g_logs[i].segments[s].temp * fminf(1.0f, 0.35f + 0.65f * v_air);
                if (g_logs[i].segments[s].moisture > 0.05f) glow *= 0.15f;
                g_logs[i].segments[s].glow_intensity = glow;

                // 9. Sap Pocket Pops
                if (g_logs[i].segments[s].temp > 0.45f && g_logs[i].segments[s].burn_progress >= 0.15f && g_logs[i].segments[s].burn_progress <= 0.75f) {
                    if (rand_f() < (0.015f * WOOD_SPECIES[g_wood_type].crackle_mult * step_dt * 40.0f)) {
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
                        Vec3 seg_p = get_log_segment_pos(&g_logs[i], s);
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

        if (!g_is_dark_souls) {
            float target_height = fminf(1.4f, 0.35f + burnt_mass * 0.85f + g_ash_bed.volume);
            float smooth_rate = 1.0f - expf(-step_dt * 0.8f);
            g_ash_bed.height += (target_height - g_ash_bed.height) * smooth_rate;
        }

        if (active_burning_segs > 0) {
            g_ash_bed.heat = fminf(1.0f, g_ash_bed.heat + 0.0005f * step_dt * 40.0f);
        } else if (cur_sim_t > 180.0f) {
            float cool_floor = (cur_sim_t > 2700.0f) ? fmaxf(0.0f, 0.12f * (1.0f - (cur_sim_t - 2700.0f) / 300.0f)) : 0.12f;
            g_ash_bed.heat = fmaxf(cool_floor, g_ash_bed.heat - 0.0002f * step_dt * 40.0f);
        }
        if (cur_sim_t >= 3000.0f) {
            g_ash_bed.heat = 0.0f;
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
            float snap_threshold = 0.22f + 0.05f * sinf(i * 3.7f + 0.8f);
            if (own_center_mass < snap_threshold && !g_logs[i].snapped) {
                g_logs[i].snapped = true;
                g_logs[i].fractured = true;

                // 1. Asymmetric fracture position along wood grain
                int min_s = 3;
                float min_m = 999.0f;
                for (int s = 2; s <= 7; s++) {
                    if (g_logs[i].segments[s].structural_mass < min_m) {
                        min_m = g_logs[i].segments[s].structural_mass;
                        min_s = s;
                    }
                }
                float r_jitter = (rand_f() - 0.5f) * 0.35f;
                float break_t = (min_s + 0.5f + r_jitter) / (float)NUM_LOG_SEGS;
                if (break_t < 0.28f) break_t = 0.28f;
                if (break_t > 0.72f) break_t = 0.72f;
                g_logs[i].break_t = break_t;

                Vec3 axis_cur = vec3_sub(g_logs[i].p2, g_logs[i].p1);
                Vec3 break_orig = vec3_add(g_logs[i].p1, vec3_scale(axis_cur, break_t));
                break_orig.y -= g_logs[i].sag_amount;

                g_logs[i].break_p1 = break_orig;
                g_logs[i].break_p1_orig = break_orig;
                g_logs[i].break_p2 = break_orig;
                g_logs[i].break_p2_orig = break_orig;
                g_logs[i].break_bounces1 = 0;
                g_logs[i].break_bounces2 = 0;

                // 2. Lateral kick vector perpendicular to log axis in XZ plane
                Vec3 lat_kick = (Vec3){ -g_logs[i].dir.z, 0.0f, g_logs[i].dir.x };
                float kick_dir = (rand_f() > 0.5f) ? 1.0f : -1.0f;
                float kick_mag1 = 0.7f + rand_f() * 1.3f;
                float kick_mag2 = 0.6f + rand_f() * 1.2f;

                float p1_ground_y = -4.2f + g_logs[i].radius * 0.85f + rand_f() * 0.30f;
                float p2_ground_y = -4.2f + g_logs[i].radius * 0.85f + rand_f() * 0.30f;

                g_logs[i].break_p1_target = (Vec3){
                    break_orig.x + lat_kick.x * kick_mag1 * kick_dir + (rand_f() - 0.5f) * 0.5f,
                    p1_ground_y,
                    break_orig.z + lat_kick.z * kick_mag1 * kick_dir + (rand_f() - 0.5f) * 0.5f
                };

                g_logs[i].break_p2_target = (Vec3){
                    break_orig.x - lat_kick.x * kick_mag2 * kick_dir + (rand_f() - 0.5f) * 0.5f,
                    p2_ground_y,
                    break_orig.z - lat_kick.z * kick_mag2 * kick_dir + (rand_f() - 0.5f) * 0.5f
                };

                g_logs[i].break_v1 = (Vec3){
                    lat_kick.x * kick_mag1 * kick_dir * 1.6f + (rand_f() - 0.5f) * 0.8f,
                    rand_f() * 1.6f + 0.4f,
                    lat_kick.z * kick_mag1 * kick_dir * 1.6f + (rand_f() - 0.5f) * 0.8f
                };
                g_logs[i].break_v2 = (Vec3){
                    -lat_kick.x * kick_mag2 * kick_dir * 1.6f + (rand_f() - 0.5f) * 0.8f,
                    rand_f() * 1.6f + 0.4f,
                    -lat_kick.z * kick_mag2 * kick_dir * 1.6f + (rand_f() - 0.5f) * 0.8f
                };

                // Asymmetric tilt & shift on the outer ends upon snapping
                g_logs[i].p1_collapsed.x += (rand_f() - 0.5f) * 0.9f;
                g_logs[i].p1_collapsed.z += (rand_f() - 0.5f) * 0.9f;
                g_logs[i].p1_collapsed.y = fmaxf(-4.2f + g_logs[i].radius, g_logs[i].p1_collapsed.y + (rand_f() - 0.5f) * 0.3f);

                g_logs[i].p2_collapsed.x += (rand_f() - 0.5f) * 0.9f;
                g_logs[i].p2_collapsed.z += (rand_f() - 0.5f) * 0.9f;
                g_logs[i].p2_collapsed.y = fmaxf(-4.2f + g_logs[i].radius, g_logs[i].p2_collapsed.y + (rand_f() - 0.5f) * 0.3f);

                // Sparks burst
                for (int sp = 0; sp < 22; sp++) {
                    Vec3 snap_v = (Vec3){(rand_f() - 0.5f) * 3.0f, rand_f() * 4.0f + 1.8f, (rand_f() - 0.5f) * 3.0f};
                    spawn_spark_3d(break_orig, snap_v, rand_range(25, 60), PALETTE_EMBERS[rand_range(2, 4)]);
                }
                // Soot smoke puff
                for (int sk = 0; sk < 10; sk++) {
                    Vec3 smk_v = (Vec3){(rand_f() - 0.5f) * 1.8f + g_wind_turb * 0.5f, rand_f() * 2.8f + 1.2f, (rand_f() - 0.5f) * 1.8f};
                    spawn_smoke_3d(break_orig, smk_v, 0.52f, 3.8f, 2);
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

            if (g_logs[i].is_falling) {
                g_logs[i].fall_timer += step_dt;

                // Gravity acceleration (downward)
                g_logs[i].fall_vy -= 26.0f * step_dt;
                float dy = g_logs[i].fall_vy * step_dt;

                // Tumble rotation
                g_logs[i].fall_rot_y += g_logs[i].fall_rot_vy * step_dt;
                g_logs[i].fall_rot_vy *= 0.97f; // rotational air resistance

                g_logs[i].p1.y += dy;
                g_logs[i].p2.y += dy;

                // Dynamic tilt jitter
                float rot_offset_x = sinf(g_logs[i].fall_rot_y) * 0.35f;
                float rot_offset_z = cosf(g_logs[i].fall_rot_y) * 0.35f;

                g_logs[i].p2.x = g_logs[i].rest_p2.x + rot_offset_x;
                g_logs[i].p2.z = g_logs[i].rest_p2.z + rot_offset_z;

                // Check impact threshold against rest position
                if (g_logs[i].p1.y <= g_logs[i].rest_p1.y) {
                    g_logs[i].fall_bounces++;

                    // Impact spark burst and soot puff
                    Vec3 impact_pt = vec3_scale(vec3_add(g_logs[i].p1, g_logs[i].p2), 0.5f);
                    for (int sp = 0; sp < 22; sp++) {
                        Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 3.6f, rand_f() * 4.2f + 2.0f, (rand_f() - 0.5f) * 3.6f};
                        spawn_spark_3d(impact_pt, sp_v, rand_range(20, 50), PALETTE_EMBERS[rand_range(2, 4)]);
                    }
                    for (int sk = 0; sk < 4; sk++) {
                        Vec3 smk_v = (Vec3){(rand_f() - 0.5f) * 1.5f, rand_f() * 1.8f + 0.8f, (rand_f() - 0.5f) * 1.5f};
                        spawn_smoke_3d(impact_pt, smk_v, 0.45f, 2.5f, 2);
                    }

                    if (g_logs[i].fall_bounces >= 2 || fabsf(g_logs[i].fall_vy) < 2.5f || g_logs[i].fall_timer > 0.65f) {
                        // Settle and lock in resting pose
                        g_logs[i].p1 = g_logs[i].rest_p1;
                        g_logs[i].p2 = g_logs[i].rest_p2;
                        g_logs[i].p1_orig = g_logs[i].rest_p1;
                        g_logs[i].p2_orig = g_logs[i].rest_p2;
                        g_logs[i].is_falling = false;
                        g_logs[i].fall_vy = 0.0f;
                    } else {
                        // Inelastic bounce
                        g_logs[i].p1.y = g_logs[i].rest_p1.y + 0.08f;
                        g_logs[i].p2.y = g_logs[i].rest_p2.y + 0.18f;
                        g_logs[i].fall_vy = -g_logs[i].fall_vy * 0.28f;
                        g_logs[i].fall_rot_vy = (rand_f() - 0.5f) * 3.0f;
                    }
                }
            } else {
                float c = g_logs[i].collapse_cur;
                g_logs[i].p1.x = g_logs[i].p1_orig.x * (1.0f - c) + g_logs[i].p1_collapsed.x * c;
                g_logs[i].p1.y = g_logs[i].p1_orig.y * (1.0f - c) + g_logs[i].p1_collapsed.y * c;
                g_logs[i].p1.z = g_logs[i].p1_orig.z * (1.0f - c) + g_logs[i].p1_collapsed.z * c;

                g_logs[i].p2.x = g_logs[i].p2_orig.x * (1.0f - c) + g_logs[i].p2_collapsed.x * c;
                g_logs[i].p2.y = g_logs[i].p2_orig.y * (1.0f - c) + g_logs[i].p2_collapsed.y * c;
                g_logs[i].p2.z = g_logs[i].p2_orig.z * (1.0f - c) + g_logs[i].p2_collapsed.z * c;
            }

            if (g_logs[i].fractured) {
                // Ragdoll physics on fracture piece 1
                g_logs[i].break_v1.y -= 22.0f * step_dt;
                g_logs[i].break_p1.x += g_logs[i].break_v1.x * step_dt;
                g_logs[i].break_p1.y += g_logs[i].break_v1.y * step_dt;
                g_logs[i].break_p1.z += g_logs[i].break_v1.z * step_dt;
                g_logs[i].break_v1.x *= 0.94f;
                g_logs[i].break_v1.z *= 0.94f;

                float ground1 = -4.2f + g_logs[i].radius * 0.85f;
                if (g_logs[i].break_p1.y <= ground1) {
                    g_logs[i].break_p1.y = ground1;
                    if (g_logs[i].break_bounces1 < 2 && fabsf(g_logs[i].break_v1.y) > 1.2f) {
                        g_logs[i].break_v1.y = -g_logs[i].break_v1.y * 0.28f;
                        g_logs[i].break_bounces1++;
                        for (int sp = 0; sp < 6; sp++) {
                            Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.0f, rand_f() * 2.5f + 1.0f, (rand_f() - 0.5f) * 2.0f};
                            spawn_spark_3d(g_logs[i].break_p1, sp_v, rand_range(15, 35), PALETTE_EMBERS[rand_range(2, 4)]);
                        }
                    } else {
                        g_logs[i].break_v1 = (Vec3){0, 0, 0};
                    }
                }

                // Ragdoll physics on fracture piece 2
                g_logs[i].break_v2.y -= 22.0f * step_dt;
                g_logs[i].break_p2.x += g_logs[i].break_v2.x * step_dt;
                g_logs[i].break_p2.y += g_logs[i].break_v2.y * step_dt;
                g_logs[i].break_p2.z += g_logs[i].break_v2.z * step_dt;
                g_logs[i].break_v2.x *= 0.94f;
                g_logs[i].break_v2.z *= 0.94f;

                float ground2 = -4.2f + g_logs[i].radius * 0.85f;
                if (g_logs[i].break_p2.y <= ground2) {
                    g_logs[i].break_p2.y = ground2;
                    if (g_logs[i].break_bounces2 < 2 && fabsf(g_logs[i].break_v2.y) > 1.2f) {
                        g_logs[i].break_v2.y = -g_logs[i].break_v2.y * 0.28f;
                        g_logs[i].break_bounces2++;
                        for (int sp = 0; sp < 6; sp++) {
                            Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.0f, rand_f() * 2.5f + 1.0f, (rand_f() - 0.5f) * 2.0f};
                            spawn_spark_3d(g_logs[i].break_p2, sp_v, rand_range(15, 35), PALETTE_EMBERS[rand_range(2, 4)]);
                        }
                    } else {
                        g_logs[i].break_v2 = (Vec3){0, 0, 0};
                    }
                }
            }

            recompute_cylinder_axes(&g_logs[i]);
        }
    }

    if (g_is_dark_souls && g_sword.active) {
        // Sustain bonfire flame activity based on state
        if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
            g_ash_bed.heat = 0.35f;
        } else if (g_fire_state == FIRE_STATE_LIT_FOCUS) {
            g_ash_bed.heat = 1.0f;
        } else {
            g_ash_bed.heat = 0.0f;
        }

        // Update blade heat along the taller twisted blade
        float blade_len = 8.0f;
        for (int i = 0; i < NUM_SWORD_BLADE_SEGS; i++) {
            float s = blade_len * (float)(i + 1) / (float)NUM_SWORD_BLADE_SEGS;
            g_sword.blade_segs[i].heat = fmaxf(0.0f, fminf(1.0f, (6.6f - s) / 5.2f)) * g_ash_bed.heat;
        }

        // Swirling embers & sparks climbing up the coiled bronze sword blade
        Vec3 u = g_sword.axis;
        Vec3 w1 = vec3_norm(vec3_cross(u, (Vec3){0.0f, 0.0f, 1.0f}));
        Vec3 w2 = vec3_cross(u, w1);

        for (int e = 0; e < 3; e++) {
            if (rand_f() < (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 0.30f : 0.75f)) {
                float s = rand_f() * (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 2.5f : 7.4f);
                float phase = s * 2.4f + g_anim_time * 6.0f + (float)e * 2.1f;
                float spiral_r = 0.38f * (1.0f - 0.12f * (s / 7.4f));
                Vec3 offset = vec3_add(vec3_scale(w1, spiral_r * sinf(phase)), vec3_scale(w2, spiral_r * cosf(phase)));
                Vec3 sp_p = vec3_add(g_sword.root_pos, vec3_add(vec3_scale(u, s), offset));
                Vec3 sp_v = (Vec3){
                    (rand_f() - 0.5f) * 0.45f + offset.x * 0.5f,
                    rand_f() * 2.8f + 1.8f,
                    (rand_f() - 0.5f) * 0.45f + offset.z * 0.5f
                };
                RGB ember_c;
                float c_choice = rand_f();
                if (c_choice > 0.65f) ember_c = PALETTE_EMBERS[4]; // White-gold ember
                else if (c_choice > 0.30f) ember_c = PALETTE_EMBERS[3]; // Bright yellow-orange fire spark
                else ember_c = PALETTE_EMBERS[2]; // Deep fiery orange-red ember
                spawn_spark_3d(sp_p, sp_v, rand_range(25, 65), ember_c);
            }
        }
    }

    // Post-extinction thin delicate wispy smoke drifting from cool ash bed
    if (!g_is_dark_souls && g_sim_time >= 3000.0f && g_sim_time < 3350.0f) {
        if (rand_f() < 0.12f) {
            Vec3 ash_c = (Vec3){(rand_f() - 0.5f) * 1.5f, -4.1f, (rand_f() - 0.5f) * 1.5f};
            Vec3 smk_v = (Vec3){
                (rand_f() - 0.5f) * 0.25f + g_wind_turb * 0.35f,
                rand_f() * 0.8f + 0.5f,
                (rand_f() - 0.5f) * 0.25f
            };
            spawn_smoke_3d(ash_c, smk_v, 0.22f, 3.2f, 1);
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
    Vec3 target = (Vec3){0.0f, g_is_dark_souls ? 1.25f : -1.2f, 0.0f};
    float cam_dist = g_is_dark_souls ? 25.2f : 28.0f;
    Vec3 cam_pos = (Vec3){
        cam_dist * cosf(g_cam_pitch) * sinf(g_cam_yaw),
        target.y + cam_dist * sinf(g_cam_pitch),
        -cam_dist * cosf(g_cam_pitch) * cosf(g_cam_yaw)
    };
    Vec3 fwd = vec3_norm(vec3_sub(target, cam_pos));
    Vec3 up_w = (Vec3){0.0f, 1.0f, 0.0f};
    Vec3 right = vec3_norm(vec3_cross(fwd, up_w));
    Vec3 up = vec3_cross(right, fwd);

    float world_w = g_is_dark_souls ? 21.6f : 22.0f;
    float world_h = g_is_dark_souls ? 16.2f : 14.0f;

    // Reset next fire frame and depth buffer
    memset(g_next_fire, 0, sizeof(g_next_fire));
    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_z[y][x] = 1e9f;
        }
    }

    if (g_is_dark_souls && g_sword.active) {
        // Update sword blade thermal state
        for (int i = 0; i < NUM_SWORD_BLADE_SEGS; i++) {
            float s = 8.0f * (float)(i + 1) / (float)NUM_SWORD_BLADE_SEGS;
            if (!g_bonfire_lit) {
                g_sword.blade_segs[i].heat = 0.0f;
            } else if (g_ignition_timer > 0.0f && g_ignition_timer < 2.0f) {
                float climb = (g_ignition_timer / 2.0f) * 8.0f;
                g_sword.blade_segs[i].heat = fmaxf(0.0f, fminf(1.0f, (climb - s + 1.0f) / 1.8f));
            } else if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
                g_sword.blade_segs[i].heat = fmaxf(0.0f, fminf(0.40f, (2.6f - s) / 2.2f));
            } else {
                g_sword.blade_segs[i].heat = fmaxf(0.0f, fminf(1.0f, (6.6f - s) / 5.2f));
            }
        }

        if (g_bonfire_lit) {
            // Base mound glowing ember core - compact hot heart at sword entry
            for (float r = 0.0f; r <= 0.95f; r += 0.22f) {
                for (float a = 0.0f; a < 6.28f; a += 0.85f) {
                    Vec3 p_emb = (Vec3){ r * cosf(a) + 0.04f, -2.80f, r * sinf(a) - 0.04f };
                    Vec3 rel_k = vec3_sub(p_emb, cam_pos);
                    int kx = (int)(((vec3_dot(rel_k, right) / world_w) + 0.5f) * g_pixel_w);
                    int ky = (int)((0.5f - (vec3_dot(rel_k, up) / world_h)) * g_pixel_h);
                    float kz = vec3_dot(rel_k, fwd);
                    float val = (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 0.45f : 0.96f) * (1.0f - r / 1.15f);
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int px = kx + dx, py = ky + dy;
                            if (px >= 0 && px < g_pixel_w && py >= 0 && py < g_pixel_h) {
                                float d = sqrtf(dx * dx * 1.0f + dy * dy * 1.5f);
                                if (d < 1.8f) {
                                    float h = val * (1.0f - d / 1.8f);
                                    g_fire_heat[py][px] = fmaxf(g_fire_heat[py][px], h);
                                    if (kz - 0.25f < g_fire_z[py][px]) g_fire_z[py][px] = kz - 0.25f;
                                }
                            }
                        }
                    }
                }
            }

            // Helical wrapping flame ribbons climbing the coiled blade all the way to the crossguard
            Vec3 u = g_sword.axis;
            Vec3 w1 = vec3_norm(vec3_cross(u, (Vec3){0.0f, 0.0f, 1.0f}));
            Vec3 w2 = vec3_cross(u, w1);
            float flame_reach = (g_fire_state == FIRE_STATE_SMOLDERING_REST) ? 2.2f : 7.6f;

            for (float s = 0.06f; s <= flame_reach; s += 0.08f) {
                float t_norm = s / flame_reach;
                float base_heat = (0.95f - t_norm * 0.40f);
                float spiral_r = 0.32f * (1.0f - t_norm * 0.15f);

                // Two intertwined spiraling ribbons hugging the blade twists
                for (int t = 0; t < 2; t++) {
                    float phase = s * 2.6f + g_anim_time * 6.5f + (t * 3.14159f);
                    Vec3 radial_disp = vec3_add(vec3_scale(w1, spiral_r * sinf(phase)),
                                                vec3_scale(w2, spiral_r * cosf(phase)));
                    Vec3 p_tongue = vec3_add(g_sword.root_pos, vec3_add(vec3_scale(u, s), radial_disp));
                    Vec3 rel = vec3_sub(p_tongue, cam_pos);
                    int kx = (int)(((vec3_dot(rel, right) / world_w) + 0.5f) * g_pixel_w);
                    int ky = (int)((0.5f - (vec3_dot(rel, up) / world_h)) * g_pixel_h);
                    float kz = vec3_dot(rel, fwd);

                    float flicker = 0.88f + 0.22f * sinf(g_anim_time * 12.0f + s * 3.5f + t * 2.5f);
                    float val = fminf(1.0f, base_heat * flicker * 1.10f);

                    int rad = (t_norm < 0.25f) ? 2 : 1;

                    // Flame depth relative to camera
                    float f_depth = kz + (radial_disp.z < 0.0f ? -0.22f : 0.22f);

                    for (int dy = -rad; dy <= rad; dy++) {
                        for (int dx = -rad; dx <= rad; dx++) {
                            int px = kx + dx, py = ky + dy;
                            if (px >= 0 && px < g_pixel_w && py >= 0 && py < g_pixel_h) {
                                float d = sqrtf(dx * dx * 1.0f + dy * dy * 1.4f);
                                if (d <= (float)rad) {
                                    float h = val * (1.0f - d / ((float)rad + 0.4f));
                                    g_fire_heat[py][px] = fmaxf(g_fire_heat[py][px], h);
                                    if (f_depth < g_fire_z[py][px]) g_fire_z[py][px] = f_depth;
                                }
                            }
                        }
                    }
                }
            }
        }
    } else {
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
                    Vec3 p = get_log_segment_pos(&g_logs[i], s);

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
    }

    // 3. Convective flame propagation upwards
    for (int y = g_pixel_h - 4; y >= 2; y--) {
        for (int x = 0; x < g_pixel_w; x++) {
            float below;
            if (g_is_dark_souls) {
                int left = (x > 0) ? x - 1 : 0;
                int right = (x < g_pixel_w - 1) ? x + 1 : g_pixel_w - 1;
                float drift = sinf(g_anim_time * 5.0f + y * 0.30f);
                int off = (drift > 0.45f) ? 1 : ((drift < -0.45f) ? -1 : 0);
                int sx = x + off;
                if (sx < 0) sx = 0;
                if (sx >= g_pixel_w) sx = g_pixel_w - 1;
                below = g_fire_heat[y + 1][x] * 0.50f + g_fire_heat[y + 1][sx] * 0.30f +
                        (g_fire_heat[y + 1][left] + g_fire_heat[y + 1][right]) * 0.10f;
            } else {
                int src_x = x;
                int wind_step = (g_wind_turb > 0.30f) ? 1 : ((g_wind_turb < -0.30f) ? -1 : 0);
                int jitter = (xorshift32() % 3) - 1;
                src_x += (rand_f() < 0.45f) ? wind_step : jitter;
                if (src_x < 0) src_x = 0;
                if (src_x >= g_pixel_w) src_x = g_pixel_w - 1;
                below = g_fire_heat[y + 1][src_x];
            }

            if (below <= 0.04f) {
                g_next_fire[y][x] = 0.0f;
                continue;
            }

            float decay = g_is_dark_souls ? (0.018f + 0.014f * rand_f()) : (0.032f + 0.030f * rand_f());
            float val = fmaxf(0.0f, below - decay);
            g_next_fire[y][x] = val;
            if (val > 0.05f) {
                g_fire_z[y][x] = g_fire_z[y + 1][x];
            }
        }
    }

    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            g_fire_heat[y][x] = g_next_fire[y][x];
        }
    }

    if (g_is_dark_souls) {
        if (g_bonfire_lit) {
            float ember_prob = (g_fire_state == FIRE_STATE_SMOLDERING_REST) ? 0.35f : 0.88f;
            // Continuous organic embers rising from the ash mound and climbing up the coiled sword blade
            if (rand_f() < ember_prob) {
                float s_ember = rand_f() * (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 2.5f : 5.8f);
                Vec3 u_sw = g_sword.axis;
                Vec3 w1_sw = vec3_norm(vec3_cross(u_sw, (Vec3){0.0f, 0.0f, 1.0f}));
                Vec3 w2_sw = vec3_cross(u_sw, w1_sw);
                float a_emb = rand_f() * 6.28f;
                float r_emb = 0.15f + rand_f() * 0.35f;
                Vec3 disp = vec3_add(vec3_scale(w1_sw, r_emb * sinf(a_emb)), vec3_scale(w2_sw, r_emb * cosf(a_emb)));
                Vec3 p_emb = vec3_add(g_sword.root_pos, vec3_add(vec3_scale(u_sw, s_ember), disp));

                Vec3 v_emb = (Vec3){
                    (rand_f() - 0.5f) * 0.6f + sinf(p_emb.y * 2.0f + g_anim_time * 3.0f) * 0.4f,
                    rand_f() * (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 1.4f : 2.2f) + 1.2f,
                    (rand_f() - 0.5f) * 0.6f
                };
                RGB emb_col = (rand_f() > 0.40f) ? PALETTE_EMBERS[3] : PALETTE_EMBERS[2];
                spawn_spark_3d(p_emb, v_emb, rand_range(20, 45), emb_col);
            }
            // Base mound hot sparks
            float mound_prob = (g_fire_state == FIRE_STATE_SMOLDERING_REST) ? 0.15f : 0.45f;
            if (rand_f() < mound_prob) {
                float r_ash = rand_f() * 1.5f;
                float a_ash = rand_f() * 6.28f;
                Vec3 p_ash = (Vec3){ r_ash * cosf(a_ash), -2.65f, r_ash * sinf(a_ash) };
                Vec3 v_ash = (Vec3){ (rand_f() - 0.5f) * 0.8f, rand_f() * 2.2f + 1.2f, (rand_f() - 0.5f) * 0.8f };
                spawn_spark_3d(p_ash, v_ash, rand_range(18, 40), PALETTE_EMBERS[rand_range(2, 4)]);
            }
        }
    } else if (g_sim_time < 2900.0f && rand_f() < (g_fire_state == FIRE_STATE_SMOLDERING_REST ? 0.20f : 0.65f)) {
        for (int i = 0; i < g_num_logs; i++) {
            int s = rand_range(0, NUM_LOG_SEGS - 1);
            if (g_logs[i].segments[s].temp > 0.50f) {
                Vec3 p = get_log_segment_pos(&g_logs[i], s);
                Vec3 spark_v = (Vec3){(rand_f() - 0.5f) * 1.5f + g_wind_turb * 1.4f, rand_f() * 3.2f + 2.0f, (rand_f() - 0.5f) * 1.5f};
                RGB spark_col = PALETTE_EMBERS[rand_range(2, 4)];
                spawn_spark_3d(p, spark_v, rand_range(16, 42), spark_col);
                break;
            }
        }
    }

    if (g_fire_state == FIRE_STATE_EXTINGUISHED || (!g_is_dark_souls && g_sim_time >= 3000.0f)) {
        memset(g_fire_heat, 0, sizeof(g_fire_heat));
        for (int i = 0; i < MAX_SPARKS; i++) g_sparks[i].active = false;
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
    Vec3 target = (Vec3){0.0f, g_is_dark_souls ? 1.25f : -1.2f, 0.0f};
    float cam_dist = g_is_dark_souls ? 25.2f : 28.0f;
    Vec3 cam_pos = (Vec3){
        cam_dist * cosf(g_cam_pitch) * sinf(g_cam_yaw),
        target.y + cam_dist * sinf(g_cam_pitch),
        -cam_dist * cosf(g_cam_pitch) * cosf(g_cam_yaw)
    };
    Vec3 fwd = vec3_norm(vec3_sub(target, cam_pos));
    Vec3 up_w = (Vec3){0.0f, 1.0f, 0.0f};
    Vec3 right = vec3_norm(vec3_cross(fwd, up_w));
    Vec3 up = vec3_cross(right, fwd);

    float world_w = g_is_dark_souls ? 21.6f : 22.0f;
    float world_h = g_is_dark_souls ? 16.2f : 14.0f;

    // Fire point light positioned in the hearth core with organic flicker
    float fire_activity = fmaxf(g_ash_bed.heat, 0.0f);
    for (int i = 0; i < g_num_logs; i++) {
        for (int s = 0; s < NUM_LOG_SEGS; s++) {
            if (g_logs[i].segments[s].temp > 0.25f && g_logs[i].segments[s].moisture <= 0.05f) {
                fire_activity = fmaxf(fire_activity, g_logs[i].segments[s].temp);
            }
        }
    }
    float flicker = (1.0f + 0.22f * sinf(g_anim_time * 8.5f + sinf(g_anim_time * 17.0f)) + 0.14f * cosf(g_anim_time * 12.0f)) * fire_activity;
    Vec3 light_pos = (Vec3){0.0f, g_is_dark_souls ? -1.5f : -1.8f, 0.0f};
    float light_intensity = 2.6f * flicker;
    if (g_is_dark_souls && !g_bonfire_lit) {
        light_intensity = 0.40f;
    }

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
            int hit_type = 0; // 0: none, 1: stone, 2: log, 3: ash/ground, 4: twig, 5: leaf, 6: sword, 7: bone

            // Test 3D Stone Fire Ring (Only in standard mode)
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

            // Test 3D Coiled Sword (Dark Souls Mode)
            Vec3 sword_pt = {0,0,0}, sword_norm = {0,1,0};
            float sword_heat = 0.0f;
            int sword_part = 0;
            if (g_is_dark_souls && g_sword.active) {
                float t;
                Vec3 pt, norm;
                float heat;
                int part;
                if (intersect_sword(&g_sword, ray_orig, ray_dir, &t, &pt, &norm, &heat, &part)) {
                    if (t < closest_t) {
                        closest_t = t;
                        sword_pt = pt;
                        sword_norm = norm;
                        sword_heat = heat;
                        sword_part = part;
                        hit_type = 6;
                    }
                }
            }

            // Test 3D Bones (Dark Souls Mode)
            Bone3D *hit_bone = NULL;
            Vec3 bone_pt = {0,0,0}, bone_norm = {0,1,0};
            float bone_char = 0.0f, bone_heat = 0.0f;
            if (g_is_dark_souls) {
                // Fast bounding sphere test for bone pile (all 55 bones/skulls lie inside radius 4.40f)
                Vec3 b_c = (Vec3){0.0f, -2.85f, -0.10f};
                Vec3 ro_b = vec3_sub(ray_orig, b_c);
                float b_dot_d = vec3_dot(ro_b, ray_dir);
                float c_b = vec3_dot(ro_b, ro_b) - (4.40f * 4.40f);
                float disc_b = b_dot_d * b_dot_d - c_b;
                if (disc_b >= 0.0f) {
                    for (int i = 0; i < g_num_bones; i++) {
                        float t, chr, ht;
                        Vec3 pt, norm;
                        if (intersect_bone(&g_bones[i], ray_orig, ray_dir, &t, &pt, &norm, &chr, &ht)) {
                            if (t < closest_t) {
                                closest_t = t;
                                bone_pt = pt;
                                bone_norm = norm;
                                bone_char = chr;
                                bone_heat = ht;
                                hit_bone = &g_bones[i];
                                hit_type = 7;
                            }
                        }
                    }
                }
            }

            // Test Ash Bed / Ground Plane
            Vec3 ash_bed_pt = {0,0,0}, ash_bed_norm = {0,1,0};
            if (g_is_dark_souls) {
                // Curved Conical Ash Mound: ellipsoid dome
                float r_bed = 4.1f;
                float h_bed = 1.62f;
                Vec3 c_bed = (Vec3){0.0f, -4.20f, 0.0f};
                Vec3 ro_s = (Vec3){ (ray_orig.x - c_bed.x) / r_bed, (ray_orig.y - c_bed.y) / h_bed, (ray_orig.z - c_bed.z) / r_bed };
                Vec3 rd_s = (Vec3){ ray_dir.x / r_bed, ray_dir.y / h_bed, ray_dir.z / r_bed };
                float a_s = vec3_dot(rd_s, rd_s);
                float b_s = 2.0f * vec3_dot(ro_s, rd_s);
                float c_s = vec3_dot(ro_s, ro_s) - 1.0f;
                float disc_s = b_s * b_s - 4.0f * a_s * c_s;
                if (disc_s >= 0.0f) {
                    float sdisc_s = sqrtf(disc_s);
                    float t0 = (-b_s - sdisc_s) / (2.0f * a_s);
                    float t1 = (-b_s + sdisc_s) / (2.0f * a_s);
                    float t = (t0 > 0.1f) ? t0 : t1;
                    if (t > 0.1f && t < closest_t) {
                        Vec3 pt = vec3_add(ray_orig, vec3_scale(ray_dir, t));
                        if (pt.y >= -4.20f) {
                            closest_t = t;
                            ash_bed_pt = pt;
                            ash_bed_norm = vec3_norm((Vec3){
                                (pt.x - c_bed.x) / (r_bed * r_bed),
                                (pt.y - c_bed.y) / (h_bed * h_bed),
                                (pt.z - c_bed.z) / (r_bed * r_bed)
                            });
                            hit_type = 3;
                        }
                    }
                }
                // Ground floor around the mound
                if (ray_dir.y < -0.001f) {
                    float gp_t = (-4.20f - ray_orig.y) / ray_dir.y;
                    if (gp_t > 0.1f && gp_t < closest_t) {
                        Vec3 gp_pt = vec3_add(ray_orig, vec3_scale(ray_dir, gp_t));
                        float gp_r = sqrtf(gp_pt.x * gp_pt.x + gp_pt.z * gp_pt.z);
                        if (gp_r < 7.8f) {
                            closest_t = gp_t;
                            ash_bed_pt = gp_pt;
                            ash_bed_norm = (Vec3){0.0f, 1.0f, 0.0f};
                            hit_type = 3;
                        }
                    }
                }
            } else {
                float ground_plane_y = -4.2f;
                if (ray_dir.y < -0.001f) {
                    float gp_t = (ground_plane_y - ray_orig.y) / ray_dir.y;
                    if (gp_t > 0.1f && gp_t < closest_t) {
                        Vec3 gp_pt = vec3_add(ray_orig, vec3_scale(ray_dir, gp_t));
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
                    // Concentric growth rings with organic radial jitter
                    float ring_phase = hit_rf * 10.0f + sinf(hit_u * 6.28318f) * 0.35f;
                    int ring_band = ((int)(ring_phase * 2.0f)) % 2;

                    // Radial wood shrinkage/fire crack fissures radiating from the pith
                    float fissure = fabsf(sinf(hit_u * 6.0f * (float)M_PI + hit_rf * 1.5f));
                    bool is_radial_fissure = (fissure < 0.16f && hit_rf > 0.12f);
                    bool is_sapwood_edge = (hit_rf > 0.82f);

                    if (burn > 0.70f) {
                        // STAGE: Calcified Ash & Exposed Ember Fissures
                        if (is_radial_fissure && (glow > 0.08f || seg_temp > 0.22f)) {
                            float hot = fmaxf(glow, seg_temp * 0.85f);
                            int emb_idx = (int)(hot * 3.8f);
                            if (emb_idx < 0) emb_idx = 0;
                            if (emb_idx > 4) emb_idx = 4;
                            g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                        } else {
                            // Calcified ash mantle
                            int ash_idx = (int)(light_val * 1.8f + (is_sapwood_edge ? 0.5f : 1.5f));
                            if (ash_idx < 1) ash_idx = 1;
                            if (ash_idx > 4) ash_idx = 4;
                            g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                        }
                    } else if (burn > 0.40f) {
                        // STAGE: Charred Carbon Endcap with Incandescent Crack Veins
                        if (is_radial_fissure && (glow > 0.10f || seg_temp > 0.35f)) {
                            float hot = fmaxf(glow, seg_temp * 0.85f);
                            int emb_idx = (int)(hot * 3.8f);
                            if (emb_idx < 0) emb_idx = 0;
                            if (emb_idx > 4) emb_idx = 4;
                            g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                        } else {
                            if (burn > 0.55f && log_norm.y > 0.30f) {
                                // Light dusting of ash on upper edge
                                g_shade_buf[y][x] = PALETTE_ASH[1];
                            } else {
                                int c_idx = (int)(light_val * 1.8f + (ring_band ? 0 : 1));
                                if (is_sapwood_edge) c_idx = 0; // Outer edge chars first
                                if (c_idx < 0) c_idx = 0;
                                if (c_idx > 4) c_idx = 4;
                                g_shade_buf[y][x] = PALETTE_CHARRED[c_idx];
                            }
                        }
                    } else if (burn > 0.15f) {
                        // STAGE: Scorched Sapwood & Darkened Growth Rings
                        if (is_sapwood_edge || (is_radial_fissure && burn > 0.25f)) {
                            g_shade_buf[y][x] = PALETTE_CHARRED[0];
                        } else {
                            int b_idx = 1 + ring_band + ((light_val > 0.75f) ? 1 : 0);
                            if (b_idx < 1) b_idx = 1;
                            if (b_idx > 4) b_idx = 4;
                            g_shade_buf[y][x] = PALETTE_WOOD[b_idx];
                        }
                    } else {
                        // STAGE: Fresh Cut Wood with Annual Growth Rings & Pith
                        int col_idx = 1 + ring_band + (is_sapwood_edge ? 1 : 0) + ((light_val > 0.80f) ? 1 : 0);
                        if (moisture > 0.08f && col_idx > 1) col_idx--; // Damp darkening
                        if (col_idx < 1) col_idx = 1;
                        if (col_idx > 4) col_idx = 4;
                        g_shade_buf[y][x] = PALETTE_ENDCAP[col_idx];
                    }
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

                if (g_is_dark_souls) {
                    if (ash_bed_pt.y > -4.18f) {
                        // On the sacred Ash Mound
                        if (r_core < 1.35f && ash_bed_pt.y > -3.35f && g_bonfire_lit) {
                            // Glowing embers and coals at the sword entry core
                            float h_core = fmaxf(0.0f, 1.0f - r_core / 1.35f);
                            int emb_idx = (int)(h_core * 3.8f);
                            if (emb_idx < 1) emb_idx = 1;
                            if (emb_idx > 3) emb_idx = 3;
                            g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                        } else {
                            // Dark carbonized ash and calcified soot dust (high contrast under white bones)
                            float noise = (sinf(ash_bed_pt.x * 4.0f) * cosf(ash_bed_pt.z * 4.0f)) * 0.20f;
                            int ash_idx = (int)((s_val + noise) * 1.8f + 0.6f);
                            if (ash_idx < 0) ash_idx = 0;
                            if (ash_idx > 2) ash_idx = 2;
                            g_shade_buf[y][x] = PALETTE_ASH[ash_idx];
                        }
                    } else {
                        // Ground floor around the mound: dark charred stone/ash with subtle fire halo
                        float halo = fmaxf(0.0f, 1.0f - (r_core - 4.2f) / 3.4f);
                        if (halo > 0.05f) {
                            int d_idx = (int)(halo * 2.2f + s_val * 0.8f);
                            if (d_idx < 0) d_idx = 0;
                            if (d_idx > 2) d_idx = 2;
                            g_shade_buf[y][x] = PALETTE_CHARRED[d_idx];
                        } else {
                            g_shade_buf[y][x] = PALETTE_CHARRED[0];
                        }
                    }
                } else {
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
                    g_shade_buf[y][x] = g_is_dark_souls ? PALETTE_BONE[b_idx] : PALETTE_WOOD[b_idx];
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

            } else if (hit_type == 6) {
                g_id_buf[y][x] = OBJ_SWORD;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(sword_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, sword_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.06f * l_dist + 0.012f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(sword_norm, l_dir));

                // Specular reflection of fire light off metal
                Vec3 v_dir = vec3_norm(vec3_sub(cam_pos, sword_pt));
                Vec3 h_dir = vec3_norm(vec3_add(l_dir, v_dir));
                float ndoth = fmaxf(0.0f, vec3_dot(sword_norm, h_dir));
                float spec = powf(ndoth, 8.0f) * 1.8f * flicker;

                float ambient = 0.28f + 0.14f * fmaxf(0.0f, sword_norm.y);
                float light_val = ndotl * atten * light_intensity * 2.2f + ambient + spec;

                if (sword_part == 0) {
                    // Coiled Blade: Burnt reddish-orange bronze alloy with incandescence along twists
                    int b_idx = (int)(light_val * 1.8f);
                    if (b_idx < 0) b_idx = 0;
                    if (b_idx > 4) b_idx = 4;
                    RGB bronze_col = PALETTE_IRON[b_idx];

                    if (spec > 0.38f) {
                        bronze_col = PALETTE_IRON[4]; // Copper/bronze specular gleam
                    }

                    if (sword_heat > 0.05f) {
                        float h = sword_heat * (0.68f + 0.32f * flicker);
                        RGB heat_col;
                        if (h > 0.82f) heat_col = (RGB){255, 242, 195};   // White-hot incandescent core
                        else if (h > 0.55f) heat_col = PALETTE_EMBERS[3]; // Golden yellow flame
                        else if (h > 0.32f) heat_col = PALETTE_EMBERS[2]; // Fiery orange-red
                        else if (h > 0.14f) heat_col = PALETTE_EMBERS[1]; // Glowing cherry red
                        else heat_col = (RGB){172, 54, 22};               // Burnt red-orange heat patina

                        // Blend heat into bronze blade so the metallic character and edge highlights are preserved
                        float heat_blend = fminf(0.85f, h * 1.15f);
                        g_shade_buf[y][x] = (RGB){
                            (uint8_t)fminf(255.0f, bronze_col.r * (1.0f - heat_blend) + heat_col.r * heat_blend),
                            (uint8_t)fminf(255.0f, bronze_col.g * (1.0f - heat_blend) + heat_col.g * heat_blend),
                            (uint8_t)fminf(255.0f, bronze_col.b * (1.0f - heat_blend) + heat_col.b * heat_blend)
                        };
                    } else {
                        // Unheated upper blade: burnt reddish-orange bronze
                        g_shade_buf[y][x] = bronze_col;
                    }
                } else if (sword_part == 2) {
                    // Dark leather-bound grip with aged bronze wire wrap
                    int grip_band = ((int)(sword_pt.y * 14.0f)) % 2;
                    int c_idx = (int)(light_val * 1.4f) + (grip_band ? 1 : 0);
                    if (c_idx < 0) c_idx = 0;
                    if (c_idx > 3) c_idx = 3;
                    g_shade_buf[y][x] = PALETTE_IRON[c_idx];
                } else {
                    // Guard, quillons & pommel: forged burned bronze with specular edge
                    int c_idx = (int)(light_val * 1.8f);
                    if (spec > 0.40f) c_idx = 4;
                    if (c_idx < 0) c_idx = 0;
                    if (c_idx > 4) c_idx = 4;
                    g_shade_buf[y][x] = PALETTE_IRON[c_idx];
                }

            } else if (hit_type == 7 && hit_bone != NULL) {
                g_id_buf[y][x] = hit_bone->obj_id;
                g_depth_buf[y][x] = vec3_dot(vec3_sub(bone_pt, cam_pos), fwd);

                Vec3 l_vec = vec3_sub(light_pos, bone_pt);
                float l_dist = vec3_len(l_vec);
                Vec3 l_dir = vec3_norm(l_vec);
                float atten = 1.0f / (1.0f + 0.08f * l_dist + 0.015f * l_dist * l_dist);
                float ndotl = fmaxf(0.0f, vec3_dot(bone_norm, l_dir));
                // Bones catch both warm core flame light and bright ambient illumination
                float front_amb = 0.90f + 0.35f * fmaxf(0.0f, bone_norm.y) - 0.30f * bone_norm.z;
                float light_val = ndotl * atten * light_intensity * 2.2f + front_amb;

                if (bone_char > 0.85f) {
                    // Deep dark socket void or nasal cavity
                    g_shade_buf[y][x] = PALETTE_BONE[0];
                } else if (bone_heat > 0.50f && g_bonfire_lit) {
                    // Glowing charred bone near embers
                    int emb_idx = (int)(bone_heat * 3.5f);
                    if (emb_idx < 0) emb_idx = 0;
                    if (emb_idx > 3) emb_idx = 3;
                    g_shade_buf[y][x] = PALETTE_EMBERS[emb_idx];
                } else if (bone_char > 0.65f) {
                    // Heavily charred calcified bone
                    g_shade_buf[y][x] = PALETTE_BONE[1];
                } else {
                    // Ancient weathered bleached bone white with rich volumetric depth
                    int b_idx = (int)(light_val * 1.55f);
                    if (bone_char > 0.30f && b_idx > 1) b_idx--; // soot dusting
                    if (b_idx < 1) b_idx = 1;
                    if (b_idx > 4) b_idx = 4;
                    g_shade_buf[y][x] = PALETTE_BONE[b_idx];
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
                        if (curr_id >= OBJ_BONE_BASE && nid >= OBJ_BONE_BASE) {
                            // Clear silhouette for the single foreground humanoid skull
                            float thresh = (curr_id == OBJ_BONE_BASE || nid == OBJ_BONE_BASE) ? 0.18f : 0.38f;
                            if (fabsf(g_depth_buf[y][x] - g_depth_buf[ny][nx]) > thresh) {
                                is_edge = true;
                                break;
                            }
                        } else if (curr_id >= OBJ_BONE_BASE && nid == OBJ_ASH_BED) {
                            // Let bones rest smoothly into the ash mound without artificial black rings
                        } else if (nid == 0 || g_depth_buf[y][x] < g_depth_buf[ny][nx] - 0.20f) {
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
                if (curr_id >= OBJ_BONE_BASE) {
                    g_frame[y][x].color = g_shade_buf[y][x]; // Keep rich bone shading and eye socket voids intact
                } else if (curr_id >= OBJ_LEAF_BASE) {
                    g_frame[y][x].color = g_shade_buf[y][x];
                } else if (curr_id == OBJ_ASH_BED) {
                    g_frame[y][x].color = g_is_dark_souls ? g_shade_buf[y][x] : PALETTE_DIRT[0];
                } else if (curr_id == OBJ_SWORD) {
                    g_frame[y][x].color = PALETTE_IRON[0];
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
                    if (g_is_dark_souls) {
                        if (g_id_buf[y][x] == OBJ_ASH_BED) {
                            g_frame[y][x].color = fire_col;
                            g_frame[y][x].is_sky = false;
                        } else if (g_id_buf[y][x] >= OBJ_BONE_BASE) {
                            // Bones in foreground: keep bone visible with warm flame lighting tint
                            if (g_fire_z[y][x] < g_depth_buf[y][x] - 0.25f) {
                                g_frame[y][x].color = fire_col; // flame in front of bone
                            } else {
                                // Additive fire glow over the bone surface
                                RGB base_c = g_frame[y][x].color;
                                float fire_alpha = heat * 0.45f;
                                g_frame[y][x].color = (RGB){
                                    (uint8_t)fminf(255.0f, base_c.r * (1.0f - fire_alpha) + fire_col.r * fire_alpha),
                                    (uint8_t)fminf(255.0f, base_c.g * (1.0f - fire_alpha) + fire_col.g * fire_alpha),
                                    (uint8_t)fminf(255.0f, base_c.b * (1.0f - fire_alpha) + fire_col.b * fire_alpha)
                                };
                            }
                            g_frame[y][x].is_sky = false;
                        } else if (g_id_buf[y][x] == OBJ_SWORD) {
                            // Sword: flame licks around the coiled bronze sword with warm translucency
                            RGB base_c = g_frame[y][x].color;
                            float fire_alpha = (g_fire_z[y][x] < g_depth_buf[y][x] - 0.25f) ? (heat * 0.60f) : (heat * 0.32f);
                            g_frame[y][x].color = (RGB){
                                (uint8_t)fminf(255.0f, base_c.r * (1.0f - fire_alpha) + fire_col.r * fire_alpha),
                                (uint8_t)fminf(255.0f, base_c.g * (1.0f - fire_alpha) + fire_col.g * fire_alpha),
                                (uint8_t)fminf(255.0f, base_c.b * (1.0f - fire_alpha) + fire_col.b * fire_alpha)
                            };
                            g_frame[y][x].is_sky = false;
                        } else if (g_fire_z[y][x] < g_depth_buf[y][x] - 0.08f) {
                            g_frame[y][x].color = fire_col;
                            g_frame[y][x].is_sky = false;
                        }
                    } else if (g_id_buf[y][x] == OBJ_ASH_BED || g_fire_z[y][x] < g_depth_buf[y][x] - 0.06f) {
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

    // 5. Ash Flakes Falling in 3D
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

static int format_frame_buffer(char *buf, int buf_cap) {
    if (!buf || buf_cap <= 0) return 0;
    int buf_len = 0;

    int n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[H");
    if (n > 0) buf_len += n;

    int prev_fg_r = -1, prev_fg_g = -1, prev_fg_b = -1;
    int prev_bg_r = -1, prev_bg_g = -1, prev_bg_b = -1;
    bool prev_bg_transp = true;

    int text_rows = g_pixel_h / 2;
    for (int r = 0; r < text_rows; r++) {
        int y_top = r * 2;
        int y_bot = r * 2 + 1;

        for (int x = 0; x < g_pixel_w; x++) {
            if (buf_len >= buf_cap - 64) break;
            Pixel top = g_frame[y_top][x];
            Pixel bot = g_frame[y_bot][x];

            if (top.is_sky && bot.is_sky) {
                if (!prev_bg_transp) {
                    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[49m");
                    if (n > 0) buf_len += n;
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                buf[buf_len++] = ' ';
                continue;
            }

            if (top.is_sky && !bot.is_sky) {
                if (bot.color.r != prev_fg_r || bot.color.g != prev_fg_g || bot.color.b != prev_fg_b) {
                    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[38;2;%d;%d;%dm",
                                 bot.color.r, bot.color.g, bot.color.b);
                    if (n > 0) buf_len += n;
                    prev_fg_r = bot.color.r; prev_fg_g = bot.color.g; prev_fg_b = bot.color.b;
                }
                if (!prev_bg_transp) {
                    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[49m");
                    if (n > 0) buf_len += n;
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                n = snprintf(buf + buf_len, buf_cap - buf_len, "▄");
                if (n > 0) buf_len += n;
                continue;
            }

            if (!top.is_sky && bot.is_sky) {
                if (top.color.r != prev_fg_r || top.color.g != prev_fg_g || top.color.b != prev_fg_b) {
                    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[38;2;%d;%d;%dm",
                                 top.color.r, top.color.g, top.color.b);
                    if (n > 0) buf_len += n;
                    prev_fg_r = top.color.r; prev_fg_g = top.color.g; prev_fg_b = top.color.b;
                }
                if (!prev_bg_transp) {
                    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[49m");
                    if (n > 0) buf_len += n;
                    prev_bg_transp = true;
                    prev_bg_r = prev_bg_g = prev_bg_b = -1;
                }
                n = snprintf(buf + buf_len, buf_cap - buf_len, "▀");
                if (n > 0) buf_len += n;
                continue;
            }

            if (top.color.r != prev_fg_r || top.color.g != prev_fg_g || top.color.b != prev_fg_b) {
                n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[38;2;%d;%d;%dm",
                             top.color.r, top.color.g, top.color.b);
                if (n > 0) buf_len += n;
                prev_fg_r = top.color.r; prev_fg_g = top.color.g; prev_fg_b = top.color.b;
            }
            if (bot.color.r != prev_bg_r || bot.color.g != prev_bg_g || bot.color.b != prev_bg_b) {
                n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[48;2;%d;%d;%dm",
                             bot.color.r, bot.color.g, bot.color.b);
                if (n > 0) buf_len += n;
                prev_bg_r = bot.color.r; prev_bg_g = bot.color.g; prev_bg_b = bot.color.b;
                prev_bg_transp = false;
            }
            n = snprintf(buf + buf_len, buf_cap - buf_len, "▀");
            if (n > 0) buf_len += n;
        }
        if (buf_len < buf_cap - 32) {
            n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[0m\n");
            if (n > 0) buf_len += n;
        }
        prev_fg_r = prev_fg_g = prev_fg_b = -1;
        prev_bg_r = prev_bg_g = prev_bg_b = -1;
        prev_bg_transp = true;
    }

    // -------------------------------------------------------------------------
    // TOP AESTHETIC POMODORO HUD (Dark Souls themed)
    // -------------------------------------------------------------------------
    if (g_pixel_w >= 65 && text_rows >= 6) {
        char hud_buf[256];
        int hud_len = 0;

        if (g_fire_state == FIRE_STATE_UNLIT) {
            hud_len = snprintf(hud_buf, sizeof(hud_buf),
                "\033[1;30m[ BONFIRE UNLIT ]\033[0m  \033[1;38;2;220;180;90mFocus: %02d:00\033[0m  |  \033[1;37m[E]\033[0m \033[38;2;255;215;100mKindle Bonfire\033[0m",
                (int)(g_focus_duration / 60.0f));
        } else if (g_fire_state == FIRE_STATE_LIT_FOCUS) {
            float el_foc = fminf(g_focus_duration, g_pomodoro_elapsed);
            float rem_foc = fmaxf(0.0f, g_focus_duration - el_foc);
            int e_m = (int)(el_foc / 60.0f), e_s = (int)fmodf(el_foc, 60.0f);
            int t_m = (int)(g_focus_duration / 60.0f), t_s = (int)fmodf(g_focus_duration, 60.0f);
            int r_m = (int)(rem_foc / 60.0f), r_s = (int)fmodf(rem_foc, 60.0f);

            float pct = (g_focus_duration > 0.0f) ? (el_foc / g_focus_duration) : 0.0f;
            int bar_w = 16;
            int filled = (int)(pct * bar_w);
            if (filled > bar_w) filled = bar_w;

            char bar[64];
            int bpos = 0;
            for (int b = 0; b < filled; b++) bpos += snprintf(bar + bpos, sizeof(bar) - bpos, "█");
            for (int b = filled; b < bar_w; b++) bpos += snprintf(bar + bpos, sizeof(bar) - bpos, "░");

            int curr_session = (g_sessions_before_long_break > 0) ? ((g_pomodoro_cycles_done % g_sessions_before_long_break) + 1) : 1;
            hud_len = snprintf(hud_buf, sizeof(hud_buf),
                "\033[1;38;2;255;160;40mFOCUS [%02d/%02d]\033[0m  \033[1;31m🔥\033[0m \033[1;37m%02d:%02d / %02d:%02d\033[0m  \033[38;2;255;190;60m[%s]\033[0m \033[1;33m%2d%%\033[0m  \033[38;2;180;180;180m(Left %02d:%02d)\033[0m%s",
                curr_session, g_sessions_before_long_break, e_m, e_s, t_m, t_s, bar, (int)(pct * 100.0f), r_m, r_s,
                g_pomodoro_paused ? "  \033[1;33m[PAUSED]\033[0m" : "");
        } else if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
            float el_rst = fminf(g_rest_duration, g_pomodoro_elapsed);
            float rem_rst = fmaxf(0.0f, g_rest_duration - el_rst);
            int r_m = (int)(rem_rst / 60.0f), r_s = (int)fmodf(rem_rst, 60.0f);
            float pct = (g_rest_duration > 0.0f) ? (1.0f - el_rst / g_rest_duration) : 0.0f;
            int bar_w = 14;
            int filled = (int)(pct * bar_w);
            if (filled > bar_w) filled = bar_w;

            char bar[64];
            int bpos = 0;
            for (int b = 0; b < filled; b++) bpos += snprintf(bar + bpos, sizeof(bar) - bpos, "█");
            for (int b = filled; b < bar_w; b++) bpos += snprintf(bar + bpos, sizeof(bar) - bpos, "░");

            const char *rest_title = g_is_long_break ? "\033[1;38;2;120;210;255mLONG BREAK\033[0m" : "\033[1;38;2;255;110;30mSHORT BREAK\033[0m";
            hud_len = snprintf(hud_buf, sizeof(hud_buf),
                "%s  \033[1;33m⏳ %02d:%02d\033[0m  \033[38;2;200;120;40m[%s]\033[0m  \033[1;37m[E]\033[0m \033[1;38;2;255;215;100mStoke Embers to Rekindle\033[0m%s",
                rest_title, r_m, r_s, bar,
                g_pomodoro_paused ? "  \033[1;33m[PAUSED]\033[0m" : "");
        } else {
            hud_len = snprintf(hud_buf, sizeof(hud_buf),
                "\033[1;30m[ EXTINGUISHED / COLD ASHES ]\033[0m  \033[1;37m[E]\033[0m \033[38;2;255;215;100mKindle New Cycle\033[0m");
        }

        if (hud_len > 0) {
            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[1;4H\033[48;2;14;10;8m %s \033[0m", hud_buf);
            if (n > 0) buf_len += n;
        }
    }

    // -------------------------------------------------------------------------
    // BOTTOM SHORTCUTS FOOTER LINE (Clean & minimal, no redundant status)
    // -------------------------------------------------------------------------
    n = snprintf(buf + buf_len, buf_cap - buf_len, "\033[%d;1H\033[2K", text_rows);
    if (n > 0) buf_len += n;

    char footer_buf[256];
    const char *snd_mode = (!g_sound_enabled || g_sound_volume == 0) ? "Muted" : "Sound";
    int snd_vol = (!g_sound_enabled) ? 0 : g_sound_volume;

    if (g_fire_state == FIRE_STATE_UNLIT) {
        snprintf(footer_buf, sizeof(footer_buf),
            "\033[1;37m[E]\033[0m \033[38;2;255;200;90mKindle\033[0m   "
            "\033[38;2;160;160;160m[Space] 360°   [M] %s   [-/+] %d%%   [Q] Quit\033[0m",
            snd_mode, snd_vol);
    } else if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
        snprintf(footer_buf, sizeof(footer_buf),
            "\033[1;37m[E]\033[0m \033[38;2;255;200;90mRekindle\033[0m   "
            "\033[38;2;160;160;160m[Space] 360°   [P] %s   [M] %s   [-/+] %d%%   [S] Skip   [Q] Quit\033[0m",
            g_pomodoro_paused ? "Resume" : "Pause",
            snd_mode, snd_vol);
    } else if (g_fire_state == FIRE_STATE_EXTINGUISHED) {
        snprintf(footer_buf, sizeof(footer_buf),
            "\033[1;37m[E]\033[0m \033[38;2;255;200;90mKindle New Cycle\033[0m   "
            "\033[38;2;160;160;160m[Space] 360°   [M] %s   [-/+] %d%%   [Q] Quit\033[0m",
            snd_mode, snd_vol);
    } else {
        // Active LIT FOCUS
        if (g_is_dark_souls) {
            snprintf(footer_buf, sizeof(footer_buf),
                "\033[1;37m[E]\033[0m \033[38;2;255;200;90mStoke\033[0m   "
                "\033[38;2;160;160;160m[Space] 360°   [P] %s   [M] %s   [-/+] %d%%   [S] Skip   [Q] Quit\033[0m",
                g_pomodoro_paused ? "Resume" : "Pause",
                snd_mode, snd_vol);
        } else {
            snprintf(footer_buf, sizeof(footer_buf),
                "\033[1;37m[F]\033[0m \033[38;2;255;200;90mWood\033[0m   "
                "\033[38;2;160;160;160m[Space] 360°   [P] %s   [M] %s   [-/+] %d%%   [S] Skip   [Q] Quit\033[0m",
                g_pomodoro_paused ? "Resume" : "Pause",
                snd_mode, snd_vol);
        }
    }

    n = snprintf(buf + buf_len, buf_cap - buf_len, " %s ", footer_buf);
    if (n > 0) buf_len += n;

    // -------------------------------------------------------------------------
    // CINEMATIC BANNER OVERLAYS (Lit, Rest, Extinguished)
    // -------------------------------------------------------------------------
    if (g_banner_timer > 0.0f && g_banner_type == BANNER_LIT) {
        float alpha = 1.0f;
        if (g_banner_timer > 3.3f) {
            alpha = (4.0f - g_banner_timer) / 0.7f;
        } else if (g_banner_timer < 0.8f) {
            alpha = g_banner_timer / 0.8f;
        }
        if (alpha < 0.05f) alpha = 0.05f;
        if (alpha > 1.0f) alpha = 1.0f;

        int center_row = text_rows / 2;
        int banner_row = center_row - 2;
        if (banner_row < 2) banner_row = 2;

        int r_acc = (int)(185.0f * alpha), g_acc = (int)(135.0f * alpha), b_acc = (int)(55.0f * alpha);
        if (g_pixel_w >= 67) {
            static const char *s_bonfire_lit_font[3] = {
                "█▀▀█  █▀▀█  █▄  █  █▀▀  ▀█▀  █▀▀█  █▀▀      █    ▀█▀  ▀█▀",
                "█▀▀▄  █  █  █ ▀▄█  █▀▀   █   █▄▄▀  █▀▀      █     █    █ ",
                "▀▀▀   ▀▀▀▀  ▀   ▀  ▀    ▀▀▀  ▀  ▀  ▀▀▀      ▀▀▀  ▀▀▀   ▀ "
            };
            int banner_w = 63;
            int start_col = (g_pixel_w - banner_w) / 2 + 1;

            int r0 = (int)(255.0f * alpha), g0 = (int)(245.0f * alpha), b0 = (int)(185.0f * alpha);
            int r1 = (int)(250.0f * alpha), g1 = (int)(200.0f * alpha), b1 = (int)(75.0f * alpha);
            int r2 = (int)(215.0f * alpha), g2 = (int)(135.0f * alpha), b2 = (int)(35.0f * alpha);

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm  ─── ── ───────────────────────────────────────────── ── ───  \033[0m",
                banner_row, start_col, r_acc, g_acc, b_acc);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm   %s   \033[0m",
                banner_row + 1, start_col, r0, g0, b0, s_bonfire_lit_font[0]);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm   %s   \033[0m",
                banner_row + 2, start_col, r1, g1, b1, s_bonfire_lit_font[1]);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm   %s   \033[0m",
                banner_row + 3, start_col, r2, g2, b2, s_bonfire_lit_font[2]);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm  ─── ── ───────────────────────────────────────────── ── ───  \033[0m",
                banner_row + 4, start_col, r_acc, g_acc, b_acc);
            if (n > 0) buf_len += n;
        } else {
            const char *title = "B O N F I R E   L I T";
            int title_len = 21;
            int banner_w = 44;
            if (banner_w > g_pixel_w - 4) banner_w = g_pixel_w - 4;
            int start_col = (g_pixel_w - banner_w) / 2 + 1;
            int text_col = (g_pixel_w - title_len) / 2 + 1;
            int r_text = (int)(255.0f * alpha), g_text = (int)(225.0f * alpha), b_text = (int)(130.0f * alpha);

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm── ─── ─────────────────────────────── ─── ──\033[0m",
                banner_row, start_col, r_acc, g_acc, b_acc);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm%s\033[0m",
                banner_row + 1, text_col, r_text, g_text, b_text, title);
            if (n > 0) buf_len += n;

            n = snprintf(buf + buf_len, buf_cap - buf_len,
                "\033[%d;%dH\033[48;2;12;8;6m\033[1;38;2;%d;%d;%dm── ─── ─────────────────────────────── ─── ──\033[0m",
                banner_row + 2, start_col, r_acc, g_acc, b_acc);
            if (n > 0) buf_len += n;
        }
    }

    if (buf_len >= buf_cap) buf_len = buf_cap - 1;
    buf[buf_len] = '\0';
    return buf_len;
}

static void present_frame(void) {
    static char s_present_buf[524288];
    int len = format_frame_buffer(s_present_buf, sizeof(s_present_buf));
    if (len > 0) {
        safe_write(STDOUT_FILENO, s_present_buf, len);
    }
}

static void play_oneshot_sound_volume(const unsigned char *data, size_t total, float volume_scale) {
    if (!data || total == 0 || volume_scale <= 0.001f) return;

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid > 0) {
        // Parent: wait for intermediate child (which exits immediately)
        waitpid(pid, NULL, 0);
        return;
    }

    // Intermediate child: fork grandchild and exit so parent is never blocked
    pid_t pid2 = fork();
    if (pid2 > 0) {
        _exit(0);
    }
    if (pid2 < 0) {
        _exit(1);
    }

    // Grandchild: completely detached from main terminal process
    int audio_pipe[2];
    if (pipe(audio_pipe) != 0) {
        _exit(1);
    }

    pid_t player_pid = fork();
    if (player_pid == 0) {
        // Audio player process
        close(audio_pipe[1]);
        if (dup2(audio_pipe[0], STDIN_FILENO) < 0) {
            _exit(1);
        }
        close(audio_pipe[0]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char vol_str[32];
        snprintf(vol_str, sizeof(vol_str), "%.2f", volume_scale);

        char pa_vol_str[32];
        int pa_vol = (int)(volume_scale * 65536.0f);
        if (pa_vol > 65536) pa_vol = 65536;
        if (pa_vol < 1) pa_vol = 1;
        snprintf(pa_vol_str, sizeof(pa_vol_str), "--volume=%d", pa_vol);

        // Try available audio players in order
        execlp("pw-play", "pw-play", "--volume", vol_str, "-", (char *)NULL);
        execlp("paplay", "paplay", pa_vol_str, "/dev/stdin", (char *)NULL);
        execlp("aplay", "aplay", "-q", "-", (char *)NULL);
        _exit(1);
    }

    if (player_pid < 0) {
        close(audio_pipe[0]);
        close(audio_pipe[1]);
        _exit(1);
    }

    // Audio feeder process: stream embedded 16-bit WAV data into player pipe
    close(audio_pipe[0]);
    signal(SIGPIPE, SIG_IGN);

    size_t sent = 0;
    while (sent < total) {
        ssize_t n = write(audio_pipe[1], data + sent, total - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(audio_pipe[1]);

    waitpid(player_pid, NULL, 0);
    _exit(0);
}

static void play_bonfire_sound(void) {
    if (!g_sound_enabled || g_sound_volume <= 0) return;
    float vol = (float)g_sound_volume / 100.0f;
    play_oneshot_sound_volume(assets_bonfire_original_16bit_wav, assets_bonfire_original_16bit_wav_len, vol);
}

static void play_alert_sound(void) {
    // When muted or sound volume is 0, do NOT silence the phase completion alert!
    // Keep it at a subtle minimum level (~15% volume) so the user knows time ended.
    float vol = 0.15f;
    if (g_sound_enabled && g_sound_volume > 0) {
        vol = (float)g_sound_volume / 100.0f;
        if (vol < 0.15f) vol = 0.15f;
    }
    play_oneshot_sound_volume(assets_item_discovery_wav, assets_item_discovery_wav_len, vol);
}

static void send_system_notification(const char *title, const char *msg) {
    // 1. Ring terminal bell to trigger window manager urgency / tab highlight
    safe_write(STDOUT_FILENO, "\a", 1);

    // 2. Dispatch desktop notification via detached grandchild
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid > 0) {
        waitpid(pid, NULL, 0);
        return;
    }

    pid_t pid2 = fork();
    if (pid2 > 0) {
        _exit(0);
    }
    if (pid2 < 0) {
        _exit(1);
    }

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    execlp("notify-send", "notify-send",
           "-a", "Bonfire Pomodoro",
           "-u", "normal",
           "-t", "8000",
           "-i", "preferences-system-time",
           title, msg, (char *)NULL);
    _exit(0);
}

static void ignite_fireplace(bool is_rekindle) {
    if (g_fire_state == FIRE_STATE_LIT_FOCUS && !is_rekindle) return;

    if (g_is_dark_souls) {
        play_bonfire_sound();
    }

    g_fire_state = FIRE_STATE_LIT_FOCUS;
    g_bonfire_lit = true;
    g_ignition_timer = 0.01f;
    g_banner_type = BANNER_LIT;
    g_banner_timer = 4.0f;
    g_pomodoro_elapsed = 0.0f;
    g_pomodoro_paused = false;
    g_cycle_logged = false;

    // Mini-explosão radial de fagulhas 3D e brasas estilo Dark Souls para todas as fogueiras
    for (int i = 0; i < 180; i++) {
        float angle = rand_f() * 2.0f * (float)M_PI;
        float r = 0.10f + 0.90f * rand_f();
        Vec3 sp_p = (Vec3){
            (g_is_dark_souls ? 0.04f : 0.0f) + r * cosf(angle),
            -2.85f + rand_f() * 0.95f,
            (g_is_dark_souls ? -0.04f : 0.0f) + r * sinf(angle)
        };
        float speed = 2.4f + rand_f() * 5.0f;
        float v_up = 3.8f + rand_f() * 5.8f;
        Vec3 sp_v = (Vec3){
            cosf(angle) * speed,
            v_up,
            sinf(angle) * speed
        };
        RGB col = (rand_f() > 0.35f) ? PALETTE_EMBERS[4] : PALETTE_EMBERS[3];
        spawn_spark_3d(sp_p, sp_v, rand_range(35, 75), col);
    }

    g_ash_bed.heat = 1.0f;

    if (!g_is_dark_souls) {
        if (is_rekindle) {
            stoke_fire_add_wood();
        } else {
            g_sim_time = 0.0f;
            for (int i = 0; i < g_num_logs; i++) {
                for (int s = 0; s < NUM_LOG_SEGS; s++) {
                    g_logs[i].segments[s].temp = fmaxf(g_logs[i].segments[s].temp, 0.45f);
                    g_logs[i].segments[s].moisture = 0.02f;
                }
            }
        }
    }

    for (int y = 0; y < g_pixel_h; y++) {
        for (int x = 0; x < g_pixel_w; x++) {
            int cx = g_pixel_w / 2;
            int cy = (int)(g_pixel_h * 0.72f);
            int dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy < 140) {
                g_fire_heat[y][x] = 0.95f;
            }
        }
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
        } else if (ch == 'p' || ch == 'P') {
            g_pomodoro_paused = !g_pomodoro_paused;
        } else if (ch == 'a' || ch == 'A' || ch == 'h') {
            g_cam_yaw -= 0.08f;
        } else if (ch == 'd' || ch == 'D' || ch == 'l') {
            g_cam_yaw += 0.08f;
        } else if (ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
            if (ch == 'k' || ch == 'K') {
                if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
                    ignite_fireplace(true); // Cutucar a brasa para reacender
                } else if (g_fire_state == FIRE_STATE_UNLIT || g_fire_state == FIRE_STATE_EXTINGUISHED) {
                    ignite_fireplace(false);
                } else {
                    for (int sp = 0; sp < 45; sp++) {
                        Vec3 sp_p = (Vec3){(rand_f() - 0.5f) * 1.6f, -2.6f + rand_f() * 1.4f, (rand_f() - 0.5f) * 1.6f};
                        Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.2f, rand_f() * 4.0f + 2.2f, (rand_f() - 0.5f) * 2.2f};
                        spawn_spark_3d(sp_p, sp_v, rand_range(28, 65), (rand_f() > 0.35f) ? PALETTE_EMBERS[3] : PALETTE_EMBERS[4]);
                    }
                }
            } else {
                g_cam_pitch += 0.06f;
                if (g_cam_pitch > 1.25f) g_cam_pitch = 1.25f;
            }
        } else if (ch == 's' || ch == 'S' || ch == 'n' || ch == 'N') {
            // Skip current Pomodoro phase
            play_alert_sound();
            if (g_fire_state == FIRE_STATE_LIT_FOCUS) {
                g_pomodoro_elapsed = 0.0f;
                g_pomodoro_cycles_done++;
                if (g_sessions_before_long_break > 0 && (g_pomodoro_cycles_done % g_sessions_before_long_break == 0)) {
                    g_is_long_break = true;
                    g_rest_duration = g_long_break_duration;
                    g_banner_type = BANNER_LONG_REST;
                } else {
                    g_is_long_break = false;
                    g_rest_duration = g_short_break_duration;
                    g_banner_type = BANNER_REST;
                }
                g_banner_timer = 3.5f;
                g_fire_state = FIRE_STATE_SMOLDERING_REST;
                send_system_notification("Bonfire Pomodoro", g_is_long_break ? "Skipped to Long Rest." : "Skipped to Short Rest.");
            } else if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
                ignite_fireplace(true);
                send_system_notification("Bonfire Pomodoro", "Rekindled! Beginning focus session.");
            } else if (g_fire_state == FIRE_STATE_UNLIT || g_fire_state == FIRE_STATE_EXTINGUISHED) {
                ignite_fireplace(false);
                send_system_notification("Bonfire Pomodoro", "Kindled! Beginning focus session.");
            }
        } else if (ch == 'j' || ch == 'J' || ch == 'z' || ch == 'Z') {
            g_cam_pitch -= 0.06f;
            if (g_cam_pitch < -0.15f) g_cam_pitch = -0.15f;
        } else if (ch == 'e' || ch == 'E') {
            if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
                ignite_fireplace(true); // Cutucar a brasa para reacender!
            } else if (g_fire_state == FIRE_STATE_UNLIT || g_fire_state == FIRE_STATE_EXTINGUISHED) {
                ignite_fireplace(false); // Acender
            } else {
                // Fogo já ativo: avivar fagulhas
                for (int sp = 0; sp < 55; sp++) {
                    Vec3 sp_p = (Vec3){(rand_f() - 0.5f) * 1.6f, -2.6f + rand_f() * 1.4f, (rand_f() - 0.5f) * 1.6f};
                    Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.2f, rand_f() * 4.0f + 2.2f, (rand_f() - 0.5f) * 2.2f};
                    spawn_spark_3d(sp_p, sp_v, rand_range(28, 65), (rand_f() > 0.35f) ? PALETTE_EMBERS[3] : PALETTE_EMBERS[4]);
                }
            }
        } else if (ch == '\n' || ch == '\r') {
            if (g_fire_state == FIRE_STATE_SMOLDERING_REST) {
                ignite_fireplace(true);
            } else if (g_fire_state == FIRE_STATE_UNLIT || g_fire_state == FIRE_STATE_EXTINGUISHED) {
                ignite_fireplace(false);
            }
        } else if (ch == ' ' || ch == 'g' || ch == 'G' || ch == 't' || ch == 'T') {
            g_auto_turntable = !g_auto_turntable;
        } else if (ch == 'f' || ch == 'F') {
            if (!g_is_dark_souls) {
                stoke_fire_add_wood();
            } else {
                for (int sp = 0; sp < 45; sp++) {
                    Vec3 sp_p = (Vec3){(rand_f() - 0.5f) * 1.6f, -2.6f + rand_f() * 1.4f, (rand_f() - 0.5f) * 1.6f};
                    Vec3 sp_v = (Vec3){(rand_f() - 0.5f) * 2.2f, rand_f() * 4.0f + 2.2f, (rand_f() - 0.5f) * 2.2f};
                    spawn_spark_3d(sp_p, sp_v, rand_range(28, 65), (rand_f() > 0.35f) ? PALETTE_EMBERS[3] : PALETTE_EMBERS[4]);
                }
            }
        } else if (ch == 'r' || ch == 'R') {
            g_fire_state = FIRE_STATE_UNLIT;
            g_bonfire_lit = false;
            g_banner_type = BANNER_NONE;
            g_banner_timer = 0.0f;
            g_ignition_timer = 0.0f;
            g_pomodoro_elapsed = 0.0f;
            g_pomodoro_paused = false;
            init_scene();
        } else if (ch == 'm' || ch == 'M') {
            g_sound_enabled = !g_sound_enabled;
            if (g_sound_enabled && g_sound_volume == 0) g_sound_volume = 50;
            if (g_shared_volume) *g_shared_volume = g_sound_enabled ? g_sound_volume : 0;
            if (!g_sound_enabled) {
                stop_ambient_sound();
            } else if (g_ambient_pgid <= 0 && !g_pomodoro_paused && g_fire_state == FIRE_STATE_LIT_FOCUS) {
                start_ambient_sound(g_is_dark_souls, g_sound_volume);
            }
            sync_volume_config();
        } else if (ch == '[' || ch == '-') {
            g_sound_volume -= 10;
            if (g_sound_volume < 0) g_sound_volume = 0;
            if (g_shared_volume) *g_shared_volume = g_sound_volume;
            if (g_sound_volume == 0) {
                stop_ambient_sound();
            } else if (g_ambient_pgid <= 0 && g_sound_enabled && !g_pomodoro_paused && g_fire_state == FIRE_STATE_LIT_FOCUS) {
                start_ambient_sound(g_is_dark_souls, g_sound_volume);
            }
            sync_volume_config();
        } else if (ch == ']' || ch == '+' || ch == '=') {
            g_sound_volume += 10;
            if (g_sound_volume > 100) g_sound_volume = 100;
            g_sound_enabled = true;
            if (g_shared_volume) *g_shared_volume = g_sound_volume;
            if (g_ambient_pgid <= 0 && !g_pomodoro_paused && g_fire_state == FIRE_STATE_LIT_FOCUS) {
                start_ambient_sound(g_is_dark_souls, g_sound_volume);
            }
            sync_volume_config();
        } else if (ch == 'q' || ch == 'Q') {
            g_running = 0;
        }
    }
}

static inline double get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

typedef struct {
    char name[64];
    int pixel_w;
    int pixel_h;
    int frames;
    double sim_avg_ms;
    double render_avg_ms;
    double format_avg_ms;
    double total_min_ms;
    double total_med_ms;
    double total_avg_ms;
    double total_p95_ms;
    double total_p99_ms;
    double total_max_ms;
    double throughput_fps;
    double low_1pct_fps;
    size_t avg_buffer_bytes;
} BenchmarkResult;

static BenchmarkResult benchmark_pipeline(const char *scenario_name, bool ds_mode, int w, int h, int num_frames) {
    static char s_bench_buf[524288];
    g_is_dark_souls = ds_mode;
    g_fire_state = FIRE_STATE_LIT_FOCUS;
    g_bonfire_lit = true;
    g_pixel_w = w;
    g_pixel_h = h;
    init_scene();

    // Warm-up 20 frames so caches are hot, fire cellular automata is active, and sparks are spawned
    for (int i = 0; i < 20; i++) {
        g_cam_yaw += 0.03f;
        g_anim_time += 0.04f;
        update_simulation();
        render_scene();
        format_frame_buffer(s_bench_buf, sizeof(s_bench_buf));
    }

    double *times = malloc(num_frames * sizeof(double));
    if (!times) {
        BenchmarkResult empty = {0};
        return empty;
    }

    double tot_sim = 0.0, tot_render = 0.0, tot_format = 0.0;
    size_t tot_bytes = 0;

    for (int i = 0; i < num_frames; i++) {
        g_cam_yaw += 0.02f;
        g_anim_time += 0.04f;

        double t0 = get_monotonic_time_ms();
        update_simulation();
        double t1 = get_monotonic_time_ms();
        render_scene();
        double t2 = get_monotonic_time_ms();
        int bytes = format_frame_buffer(s_bench_buf, sizeof(s_bench_buf));
        double t3 = get_monotonic_time_ms();

        double d_sim = t1 - t0;
        double d_render = t2 - t1;
        double d_format = t3 - t2;
        double d_total = d_sim + d_render + d_format;

        times[i] = d_total;
        tot_sim += d_sim;
        tot_render += d_render;
        tot_format += d_format;
        tot_bytes += (bytes > 0 ? (size_t)bytes : 0);
    }

    qsort(times, num_frames, sizeof(double), cmp_double);

    BenchmarkResult res;
    memset(&res, 0, sizeof(res));
    snprintf(res.name, sizeof(res.name), "%s", scenario_name);
    res.pixel_w = w;
    res.pixel_h = h;
    res.frames = num_frames;
    res.sim_avg_ms = tot_sim / num_frames;
    res.render_avg_ms = tot_render / num_frames;
    res.format_avg_ms = tot_format / num_frames;
    res.total_min_ms = times[0];
    res.total_med_ms = times[num_frames / 2];
    res.total_avg_ms = (tot_sim + tot_render + tot_format) / num_frames;
    int idx_p95 = (int)(num_frames * 0.95);
    if (idx_p95 >= num_frames) idx_p95 = num_frames - 1;
    res.total_p95_ms = times[idx_p95];
    int idx_p99 = (int)(num_frames * 0.99);
    if (idx_p99 >= num_frames) idx_p99 = num_frames - 1;
    res.total_p99_ms = times[idx_p99];
    res.total_max_ms = times[num_frames - 1];
    res.throughput_fps = (res.total_avg_ms > 0.0001) ? (1000.0 / res.total_avg_ms) : 9999.0;
    res.low_1pct_fps = (res.total_p99_ms > 0.0001) ? (1000.0 / res.total_p99_ms) : 9999.0;
    res.avg_buffer_bytes = tot_bytes / num_frames;

    free(times);
    return res;
}

static void run_benchmark_suite(int frames) {
    if (frames < 30) frames = 30;
    printf("\n===================================================================================================\n");
    printf("                    FIREPLACE & DARK SOULS BONFIRE BENCHMARK REPORT (%d frames)\n", frames);
    printf("===================================================================================================\n");

    BenchmarkResult r[4];
    r[0] = benchmark_pipeline("Dark Souls Bonfire", true, 120, 70, frames);
    r[1] = benchmark_pipeline("Standard Fireplace", false, 120, 70, frames);
    r[2] = benchmark_pipeline("Dark Souls Bonfire", true, 160, 90, frames);
    r[3] = benchmark_pipeline("Standard Fireplace", false, 160, 90, frames);

    printf("+----------------------+------------+------------+-----------+-----------+-----------+-----------------------+-------------+-------------+\n");
    printf("| Scenario             | Res (char) | Avg Total  | Median    | P95       | P99       | Stage Split (S/R/F)   | Throughput  | 1%% Low FPS  |\n");
    printf("+----------------------+------------+------------+-----------+-----------+-----------+-----------------------+-------------+-------------+\n");

    for (int i = 0; i < 4; i++) {
        double total = r[i].total_avg_ms;
        double s_pct = (total > 0.0) ? (r[i].sim_avg_ms / total * 100.0) : 0.0;
        double r_pct = (total > 0.0) ? (r[i].render_avg_ms / total * 100.0) : 0.0;
        double f_pct = (total > 0.0) ? (r[i].format_avg_ms / total * 100.0) : 0.0;
        char char_res[24];
        snprintf(char_res, sizeof(char_res), "%dx%d", r[i].pixel_w, r[i].pixel_h / 2);

        printf("| %-20s | %-10s | %6.2f ms | %6.2f ms | %6.2f ms | %6.2f ms | %4.1f%% / %4.1f%% / %4.1f%% | %7.1f FPS | %7.1f FPS |\n",
               r[i].name, char_res, r[i].total_avg_ms, r[i].total_med_ms, r[i].total_p95_ms, r[i].total_p99_ms,
               s_pct, r_pct, f_pct, r[i].throughput_fps, r[i].low_1pct_fps);
    }
    printf("+----------------------+------------+------------+-----------+-----------+-----------+-----------------------+-------------+-------------+\n\n");

    printf("Analysis & 60 FPS Target (Budget: 16.67 ms):\n");
    for (int i = 0; i < 4; i++) {
        double headroom = (16.6667 - r[i].total_avg_ms) / 16.6667 * 100.0;
        printf(" - [%s @ %dx%d (%dx%d chars)]: Avg %.2f ms (%.1f FPS) | ANSI frame size: %4.1f KB | Headroom: %+.1f%%\n",
               r[i].name, r[i].pixel_w, r[i].pixel_h, r[i].pixel_w, r[i].pixel_h / 2,
               r[i].total_avg_ms, r[i].throughput_fps, (double)r[i].avg_buffer_bytes / 1024.0, headroom);
    }
    printf("===================================================================================================\n\n");
}

static void get_config_path(char *out_path, size_t max_len) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        home = ".";
    }
    snprintf(out_path, max_len, "%s/.fireplace_conf", home);
}

static void load_user_config(int *style, int *focus_min, int *short_break_min, int *long_break_min, int *sessions, int *volume) {
    char path[1024];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf(line, "style=%d", &val) == 1) {
            *style = (val == 1) ? 1 : 0;
        } else if (sscanf(line, "focus_min=%d", &val) == 1) {
            if (val >= 1 && val <= 180) *focus_min = val;
        } else if (sscanf(line, "short_break_min=%d", &val) == 1) {
            if (val >= 1 && val <= 60) *short_break_min = val;
        } else if (sscanf(line, "long_break_min=%d", &val) == 1) {
            if (val >= 1 && val <= 120) *long_break_min = val;
        } else if (sscanf(line, "sessions=%d", &val) == 1) {
            if (val >= 1 && val <= 24) *sessions = val;
        } else if (sscanf(line, "volume=%d", &val) == 1) {
            if (val >= 0 && val <= 100) *volume = val;
        }
    }
    fclose(f);
}

static void save_user_config(int style, int focus_min, int short_break_min, int long_break_min, int sessions, int volume) {
    char path[1024];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "style=%d\n", style);
    fprintf(f, "focus_min=%d\n", focus_min);
    fprintf(f, "short_break_min=%d\n", short_break_min);
    fprintf(f, "long_break_min=%d\n", long_break_min);
    fprintf(f, "sessions=%d\n", sessions);
    fprintf(f, "volume=%d\n", volume);
    fclose(f);
}

static void sync_volume_config(void) {
    int focus_min = (int)(g_focus_duration / 60.0f);
    int short_break_min = (int)(g_short_break_duration / 60.0f);
    int long_break_min = (int)(g_long_break_duration / 60.0f);
    int sessions = g_sessions_before_long_break;
    int style = g_is_dark_souls ? 1 : 0;
    save_user_config(style, focus_min, short_break_min, long_break_min, sessions, g_sound_volume);
}

static bool run_setup_menu(void) {
    int selected = 0; // 0: Style, 1: Focus, 2: Short Break, 3: Long Break, 4: Sessions, 5: Volume
    bool in_menu = true;

    int focus_min = (int)(g_focus_duration / 60.0f);
    int short_break_min = (int)(g_short_break_duration / 60.0f);
    int long_break_min = (int)(g_long_break_duration / 60.0f);
    int sessions = g_sessions_before_long_break;
    int style = g_is_dark_souls ? 1 : 0;
    int volume = g_sound_volume;

    load_user_config(&style, &focus_min, &short_break_min, &long_break_min, &sessions, &volume);
    g_is_dark_souls = (style == 1);
    g_sound_volume = volume;
    g_sound_enabled = (volume > 0);

    char menu_buf[8192];
    struct timespec ts = {0, 25000000L}; // 40 FPS

    safe_write(STDOUT_FILENO, "\033[2J\033[H", 7);

    while (in_menu && g_running) {
        if (g_resized) {
            g_resized = 0;
            update_dimensions();
            safe_write(STDOUT_FILENO, "\033[2J\033[H", 7);
        }

        char ch;
        while (read(STDIN_FILENO, &ch, 1) > 0) {
            if (ch == '\033') {
                char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                    if (seq[0] == '[') {
                        if (seq[1] == 'A') { // Up
                            selected = (selected + 5) % 6;
                        } else if (seq[1] == 'B') { // Down
                            selected = (selected + 1) % 6;
                        } else if (seq[1] == 'D') { // Left
                            if (selected == 0) g_is_dark_souls = !g_is_dark_souls;
                            else if (selected == 1) { focus_min = (focus_min > 5) ? focus_min - 5 : 5; }
                            else if (selected == 2) { short_break_min = (short_break_min > 1) ? short_break_min - 1 : 1; }
                            else if (selected == 3) { long_break_min = (long_break_min > 5) ? long_break_min - 5 : 5; }
                            else if (selected == 4) { sessions = (sessions > 1) ? sessions - 1 : 1; }
                            else if (selected == 5) { volume = (volume >= 10) ? volume - 10 : 0; }
                        } else if (seq[1] == 'C') { // Right
                            if (selected == 0) g_is_dark_souls = !g_is_dark_souls;
                            else if (selected == 1) { focus_min = (focus_min < 120) ? focus_min + 5 : 120; }
                            else if (selected == 2) { short_break_min = (short_break_min < 30) ? short_break_min + 1 : 30; }
                            else if (selected == 3) { long_break_min = (long_break_min < 60) ? long_break_min + 5 : 60; }
                            else if (selected == 4) { sessions = (sessions < 12) ? sessions + 1 : 12; }
                            else if (selected == 5) { volume = (volume <= 90) ? volume + 10 : 100; }
                        }
                    }
                }
            } else if (ch >= '1' && ch <= '6') {
                selected = ch - '1';
            } else if (ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K') {
                selected = (selected + 5) % 6;
            } else if (ch == 's' || ch == 'S' || ch == 'j' || ch == 'J') {
                selected = (selected + 1) % 6;
            } else if (ch == 'a' || ch == 'A' || ch == 'h' || ch == '-' || ch == '_') {
                if (selected == 0) g_is_dark_souls = !g_is_dark_souls;
                else if (selected == 1) { focus_min = (focus_min > 5) ? focus_min - 5 : 5; }
                else if (selected == 2) { short_break_min = (short_break_min > 1) ? short_break_min - 1 : 1; }
                else if (selected == 3) { long_break_min = (long_break_min > 5) ? long_break_min - 5 : 5; }
                else if (selected == 4) { sessions = (sessions > 1) ? sessions - 1 : 1; }
                else if (selected == 5) { volume = (volume >= 10) ? volume - 10 : 0; }
            } else if (ch == 'd' || ch == 'D' || ch == 'l' || ch == '+' || ch == '=') {
                if (selected == 0) g_is_dark_souls = !g_is_dark_souls;
                else if (selected == 1) { focus_min = (focus_min < 120) ? focus_min + 5 : 120; }
                else if (selected == 2) { short_break_min = (short_break_min < 30) ? short_break_min + 1 : 30; }
                else if (selected == 3) { long_break_min = (long_break_min < 60) ? long_break_min + 5 : 60; }
                else if (selected == 4) { sessions = (sessions < 12) ? sessions + 1 : 12; }
                else if (selected == 5) { volume = (volume <= 90) ? volume + 10 : 100; }
            } else if (ch == '\n' || ch == '\r' || ch == ' ') {
                in_menu = false;
                break;
            } else if (ch == 'q' || ch == 'Q') {
                g_running = 0;
                return false;
            }
        }

        if (!in_menu) break;

        int box_w = 66;
        int box_h = 17;
        int start_r = (g_term_rows - box_h) / 2;
        if (start_r < 1) start_r = 1;
        int start_c = (g_term_cols - box_w) / 2;
        if (start_c < 1) start_c = 1;

        int len = 0;
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len, "\033[H");

        // Border & Header
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[1;38;2;220;140;40m╔════════════════════════════════════════════════════════════════╗\033[0m",
            start_r, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[1;38;2;255;210;110m║                  BONFIRE POMODORO SETUP                        ║\033[0m",
            start_r + 1, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[38;2;180;120;60m║            Configure your session before kindling              ║\033[0m",
            start_r + 2, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[1;38;2;220;140;40m╠════════════════════════════════════════════════════════════════╣\033[0m",
            start_r + 3, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║                                                                ║\033[0m",
            start_r + 4, start_c);

        // Row 0: Style
        const char *style_str = g_is_dark_souls ? "Dark Souls Bonfire" : "Classic Wood Fireplace";
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 5, start_c,
            (selected == 0) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 0) ? "▶ " : "  ",
            "[1] Fireplace Style:",
            style_str);

        // Row 1: Focus
        char foc_str[32];
        snprintf(foc_str, sizeof(foc_str), "%d min", focus_min);
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 6, start_c,
            (selected == 1) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 1) ? "▶ " : "  ",
            "[2] Focus Duration:",
            foc_str);

        // Row 2: Short Break
        char sb_str[32];
        snprintf(sb_str, sizeof(sb_str), "%d min", short_break_min);
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 7, start_c,
            (selected == 2) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 2) ? "▶ " : "  ",
            "[3] Short Break:",
            sb_str);

        // Row 3: Long Break
        char lb_str[32];
        snprintf(lb_str, sizeof(lb_str), "%d min", long_break_min);
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 8, start_c,
            (selected == 3) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 3) ? "▶ " : "  ",
            "[4] Long Break:",
            lb_str);

        // Row 4: Sessions
        char sess_str[32];
        snprintf(sess_str, sizeof(sess_str), "%d sessions", sessions);
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 9, start_c,
            (selected == 4) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 4) ? "▶ " : "  ",
            "[5] Sessions / Cycle:",
            sess_str);

        // Row 5: Sound Volume
        char vol_str[32];
        if (volume == 0) snprintf(vol_str, sizeof(vol_str), "Mute (0%%)");
        else snprintf(vol_str, sizeof(vol_str), "%d%%", volume);
        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║%s %s%-21s   ◄ %-23s ►          \033[0m\033[48;2;16;10;8m║\033[0m",
            start_r + 10, start_c,
            (selected == 5) ? "\033[48;2;55;25;12m\033[1;38;2;255;235;130m" : "\033[48;2;16;10;8m\033[38;2;200;190;175m",
            (selected == 5) ? "▶ " : "  ",
            "[6] Sound Volume:",
            vol_str);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m║                                                                ║\033[0m",
            start_r + 11, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[1;38;2;220;140;40m╠════════════════════════════════════════════════════════════════╣\033[0m",
            start_r + 12, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[38;2;220;190;140m║ [↑/↓ or 1-6] Navigate  [←/→] Adjust  [Enter/Space] Start       ║\033[0m",
            start_r + 13, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[38;2;160;140;120m║ [Q] Exit                                                       ║\033[0m",
            start_r + 14, start_c);

        len += snprintf(menu_buf + len, sizeof(menu_buf) - len,
            "\033[%d;%dH\033[48;2;16;10;8m\033[1;38;2;220;140;40m╚════════════════════════════════════════════════════════════════╝\033[0m",
            start_r + 15, start_c);

        safe_write(STDOUT_FILENO, menu_buf, len);
        nanosleep(&ts, NULL);
    }

    if (!g_running) return false;

    // Apply configuration and persist to ~/.fireplace_conf
    g_sound_volume = volume;
    g_sound_enabled = (volume > 0);
    if (g_shared_volume) *g_shared_volume = volume;
    save_user_config(g_is_dark_souls ? 1 : 0, focus_min, short_break_min, long_break_min, sessions, volume);
    g_focus_duration = (float)focus_min * 60.0f;
    g_short_break_duration = (float)short_break_min * 60.0f;
    g_long_break_duration = (float)long_break_min * 60.0f;
    g_sessions_before_long_break = sessions;
    g_rest_duration = g_short_break_duration;
    g_cycle_duration = g_focus_duration;

    safe_write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return true;
}

int main(int argc, char **argv) {
    g_rng ^= (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ 0x9e3779b9;

    g_shared_volume = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_shared_volume != MAP_FAILED) {
        *g_shared_volume = g_sound_volume;
    } else {
        g_shared_volume = NULL;
    }

    bool do_benchmark = false;
    int benchmark_frames = 200;
    bool force_unlit = false;
    bool skip_menu = false;

    const char *snapshot_out = NULL;
    float snapshot_sim = 0.0f;
    float snapshot_yaw = 0.0f;
    float snapshot_pitch = 20.0f;
    bool do_snapshot = false;

    const char *turntable_dir = NULL;
    int turntable_frames = 60;
    float turntable_sim = 250.0f;
    bool do_turntable = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--history") == 0 || strcmp(argv[i], "-h") == 0) {
            print_bonfire_history();
            return 0;
        } else if (strcmp(argv[i], "--no-menu") == 0) {
            skip_menu = true;
        } else if (strcmp(argv[i], "--menu") == 0) {
            skip_menu = false;
        } else if (strcmp(argv[i], "--benchmark") == 0 || strcmp(argv[i], "--perf") == 0) {
            do_benchmark = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                benchmark_frames = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--souls") == 0 || strcmp(argv[i], "--ds") == 0 || strcmp(argv[i], "--darksouls") == 0) {
            g_is_dark_souls = true;
            g_bonfire_lit = false;
        } else if (strcmp(argv[i], "--unlit") == 0) {
            force_unlit = true;
            g_bonfire_lit = false;
        } else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            do_snapshot = true;
            snapshot_out = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') snapshot_sim = (float)atof(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') snapshot_yaw = (float)atof(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') snapshot_pitch = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--turntable") == 0) {
            do_turntable = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') turntable_dir = argv[++i];
            else turntable_dir = "/tmp";
            if (i + 1 < argc && argv[i + 1][0] != '-') turntable_frames = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') turntable_sim = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--fast") == 0) {
            g_realtime_mode = false;
            g_time_scale = 30.0f;
        } else if ((strcmp(argv[i], "--wood") == 0 || strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--madeira") == 0) && i + 1 < argc) {
            const char *w = argv[++i];
            g_wood_type_forced = true;
            if (strcasecmp(w, "carvalho") == 0 || strcasecmp(w, "oak") == 0) g_wood_type = 0;
            else if (strcasecmp(w, "pinho") == 0 || strcasecmp(w, "pine") == 0) g_wood_type = 1;
            else if (strcasecmp(w, "betula") == 0 || strcasecmp(w, "birch") == 0) g_wood_type = 2;
            else if (strcasecmp(w, "cerejeira") == 0 || strcasecmp(w, "cherry") == 0) g_wood_type = 3;
        } else if (strcmp(argv[i], "--lit") == 0) {
            g_fire_state = FIRE_STATE_LIT_FOCUS;
            g_bonfire_lit = true;
        } else if ((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--pomodoro") == 0) && i + 1 < argc) {
            const char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (colon) {
                float foc = (float)atof(arg);
                float rst = (float)atof(colon + 1);
                if (foc > 0.0f) {
                    g_focus_duration = foc * 60.0f;
                    g_cycle_duration = g_focus_duration;
                }
                if (rst > 0.0f) {
                    g_rest_duration = rst * 60.0f;
                }
            } else {
                float mins = (float)atof(arg);
                if (mins > 0.0f) {
                    g_focus_duration = mins * 60.0f;
                    g_cycle_duration = g_focus_duration;
                }
            }
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_time_scale = (float)atof(argv[++i]);
            g_realtime_mode = (fabsf(g_time_scale - 1.0f) < 0.1f);
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9' && strstr(argv[i], ".ppm") == NULL) {
            float mins = (float)atof(argv[i]);
            if (mins > 0.0f) {
                g_focus_duration = mins * 60.0f;
                g_cycle_duration = g_focus_duration;
            }
        }
    }

    if (do_benchmark) {
        g_fire_state = FIRE_STATE_LIT_FOCUS;
        g_bonfire_lit = true;
        run_benchmark_suite(benchmark_frames);
        return 0;
    }

    if (do_snapshot && snapshot_out != NULL) {
        g_pixel_w = 120;
        g_pixel_h = 70;
        if (force_unlit) {
            g_fire_state = FIRE_STATE_UNLIT;
            g_bonfire_lit = false;
        } else {
            g_fire_state = FIRE_STATE_LIT_FOCUS;
            g_bonfire_lit = true;
        }
        init_scene();
        g_cam_yaw = snapshot_yaw * (float)M_PI / 180.0f;
        g_cam_pitch = snapshot_pitch * (float)M_PI / 180.0f;
        g_time_scale = 30.0f;
        while (g_sim_time < snapshot_sim) {
            update_simulation();
        }
        render_scene();
        FILE *f = fopen(snapshot_out, "wb");
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

    if (do_turntable) {
        g_pixel_w = 120;
        g_pixel_h = 70;
        g_fire_state = FIRE_STATE_LIT_FOCUS;
        g_bonfire_lit = true;
        init_scene();
        g_time_scale = 30.0f;
        while (g_sim_time < turntable_sim) {
            update_simulation();
        }
        g_time_scale = 1.0f;
        for (int fr = 0; fr < turntable_frames; fr++) {
            g_cam_yaw = (float)fr / (float)turntable_frames * 2.0f * (float)M_PI;
            g_cam_pitch = 0.32f;
            g_anim_time += 0.05f;
            update_simulation();
            render_scene();
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%04d.ppm", turntable_dir, fr);
            FILE *f = fopen(path, "wb");
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
        }
        return 0;
    }

    setup_terminal();
    update_dimensions();

    if (!skip_menu) {
        if (!run_setup_menu()) {
            return 0;
        }
    }

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
        update_ambient_audio();
        render_scene();
        present_frame();

        ts.tv_sec = 0;
        ts.tv_nsec = 24000000L; // ~40 FPS
        nanosleep(&ts, NULL);
    }

    stop_ambient_sound();

    // Save history if closed prematurely after at least 1 minute
    if (!g_cycle_logged) {
        float elapsed_sec = fminf(g_cycle_duration, g_sim_time * (g_cycle_duration / 3000.0f));
        int el_min = (int)(elapsed_sec / 60.0f);
        int target_min = (int)(g_cycle_duration / 60.0f);
        if (el_min >= 1) {
            char mode_desc[64];
            if (g_is_dark_souls) {
                snprintf(mode_desc, sizeof(mode_desc), "Dark Souls Bonfire (Coiled Sword)");
            } else {
                snprintf(mode_desc, sizeof(mode_desc), "Teepee (%s)", WOOD_SPECIES[g_wood_type].name);
            }
            log_bonfire_history(target_min, el_min, mode_desc, false);
        }
    }

    return 0;
}
