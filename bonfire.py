#!/usr/bin/env python3
"""
3D Per-Object Pixel Art Bonfire Simulation (Python Edition)

Features:
- 3D Spherical Orbit Camera (Yaw θ, Pitch φ) with real-time controls & turntable
- 3D Stone Fire Ring Base (Círculo de pedras de contenção no chão)
- Segmented Wood Combustion (10 longitudinal segments per log):
  Fresh Wood -> Smoking -> Burning Flames -> Charred Black -> Brittle Ash
- Zero Floor Fire: Flames originate strictly from burning wood segments and central kindling
- Physical Self-Collapse Kinematics under gravity as structural mass burns away
- 3 Physical Stacking Modes: Fogueira Quadrada (Log Cabin), Tenda Cônica (Teepee), Pirâmide
- Discrete Object-Space Bark Plates (No orange tiger stripes, zero pixel creep)
- Concentric Growth Rings on Cut End-Caps
- Unicode half-blocks '▀' (2 vertical pixels per cell) with terminal transparency
"""

import sys
import os
import time
import math
import random
import select
import termios
import tty
import signal

PALETTE_WOOD = [
    (20, 12, 8),      # 0: Outline / deep crevice
    (42, 27, 18),     # 1: Deep shadow bark (raw umber)
    (72, 48, 32),     # 2: Dark weathered oak
    (105, 72, 48),    # 3: Mid oak bark
    (138, 96, 64),    # 4: Warm dry timber
    (168, 120, 82),   # 5: Muted wood highlight
    (200, 145, 102)   # 6: Warm firelit rim
]

PALETTE_CHARRED = [
    (14, 10, 10),     # 0: Pure charcoal black crevice
    (26, 22, 22),     # 1: Charred black bark
    (46, 42, 42),     # 2: Dark charcoal
    (68, 62, 60),     # 3: Burnt grey bark
    (95, 88, 86)      # 4: Ash surface
]

PALETTE_STONE = [
    (18, 16, 14),     # 0: Stone outline / deep shade
    (42, 38, 35),     # 1: Dark basalt
    (68, 62, 58),     # 2: Mid granite grey
    (96, 88, 82),     # 3: Weathered mineral rock
    (128, 115, 102),  # 4: Warm firelit stone face
    (165, 145, 122)   # 5: Bright fire reflection
]

PALETTE_ENDCAP = [
    (20, 12, 8),      # 0: Bark rim
    (92, 60, 38),     # 1: Dark ring
    (130, 88, 56),    # 2: Mid ring
    (168, 116, 78),   # 3: Sapwood ring
    (205, 150, 105)   # 4: Pith core
]

PALETTE_EMBERS = [
    (140, 18, 5),     # 0: Deep red ember
    (228, 48, 10),    # 1: Bright red flame
    (255, 115, 18),   # 2: Hot orange flame
    (255, 205, 48),   # 3: Yellow incandescence
    (255, 255, 210)   # 4: White hot core
]

PALETTE_ASH = [
    (38, 35, 42),     # 0: Charcoal crust
    (68, 64, 74),     # 1: Dark ash
    (115, 112, 122),  # 2: Mid ash grey
    (165, 162, 172),  # 3: Light chalky ash
    (210, 208, 218)   # 4: White ash powder
]

NUM_LOG_SEGS = 10

class Stone3D:
    def __init__(self, obj_id, center, radius):
        self.obj_id = obj_id
        self.center = list(center)
        self.radius = radius

    def intersect(self, ro, rd):
        oc = [ro[0] - self.center[0], ro[1] - self.center[1], ro[2] - self.center[2]]
        b = oc[0]*rd[0] + oc[1]*rd[1] + oc[2]*rd[2]
        c = (oc[0]*oc[0] + oc[1]*oc[1] + oc[2]*oc[2]) - self.radius * self.radius
        disc = b * b - c
        if disc < 0:
            return None
        sdisc = math.sqrt(disc)
        t = -b - sdisc
        if t < 0.1:
            t = -b + sdisc
        if t < 0.1:
            return None
        pt = [ro[0] + t * rd[0], ro[1] + t * rd[1], ro[2] + t * rd[2]]
        inv_r = 1.0 / self.radius
        norm = [(pt[0] - self.center[0]) * inv_r, (pt[1] - self.center[1]) * inv_r, (pt[2] - self.center[2]) * inv_r]
        return t, pt, norm

class SegmentedCylinder3D:
    def __init__(self, obj_id, p1, p2, p1_collapsed, p2_collapsed, radius, charred=0.3):
        self.obj_id = obj_id
        self.p1 = list(p1)
        self.p2 = list(p2)
        self.p1_orig = list(p1)
        self.p2_orig = list(p2)
        self.p1_collapsed = list(p1_collapsed)
        self.p2_collapsed = list(p2_collapsed)
        self.radius = radius
        self.charred = charred
        self.temp = [0.0 for _ in range(NUM_LOG_SEGS)]
        self.burn_progress = [0.0 for _ in range(NUM_LOG_SEGS)]
        self.structural_mass = [1.0 for _ in range(NUM_LOG_SEGS)]
        self.recompute()

    def recompute(self):
        dx = self.p2[0] - self.p1[0]
        dy = self.p2[1] - self.p1[1]
        dz = self.p2[2] - self.p1[2]
        self.axis = [dx, dy, dz]
        self.length = math.sqrt(dx*dx + dy*dy + dz*dz)
        inv = 1.0 / (self.length + 1e-6)
        self.dir = [dx*inv, dy*inv, dz*inv]

        ref = [0.0, 1.0, 0.0]
        dot_ref = ref[0]*self.dir[0] + ref[1]*self.dir[1] + ref[2]*self.dir[2]
        if abs(dot_ref) > 0.88:
            ref = [1.0, 0.0, 0.0]
        
        tx = self.dir[1]*ref[2] - self.dir[2]*ref[1]
        ty = self.dir[2]*ref[0] - self.dir[0]*ref[2]
        tz = self.dir[0]*ref[1] - self.dir[1]*ref[0]
        t_len = math.sqrt(tx*tx + ty*ty + tz*tz) + 1e-6
        self.tangent = [tx/t_len, ty/t_len, tz/t_len]

        self.bitangent = [
            self.dir[1]*self.tangent[2] - self.dir[2]*self.tangent[1],
            self.dir[2]*self.tangent[0] - self.dir[0]*self.tangent[2],
            self.dir[0]*self.tangent[1] - self.dir[1]*self.tangent[0]
        ]

    def intersect(self, ro, rd):
        best_t = 1e9
        best_res = None

        dp = [ro[0] - self.p1[0], ro[1] - self.p1[1], ro[2] - self.p1[2]]
        d_dot_dir = rd[0]*self.dir[0] + rd[1]*self.dir[1] + rd[2]*self.dir[2]
        dp_dot_dir = dp[0]*self.dir[0] + dp[1]*self.dir[1] + dp[2]*self.dir[2]

        d_proj = [rd[0] - self.dir[0]*d_dot_dir, rd[1] - self.dir[1]*d_dot_dir, rd[2] - self.dir[2]*d_dot_dir]
        dp_proj = [dp[0] - self.dir[0]*dp_dot_dir, dp[1] - self.dir[1]*dp_dot_dir, dp[2] - self.dir[2]*dp_dot_dir]

        a = d_proj[0]*d_proj[0] + d_proj[1]*d_proj[1] + d_proj[2]*d_proj[2]
        if a > 1e-6:
            b = 2.0 * (d_proj[0]*dp_proj[0] + d_proj[1]*dp_proj[1] + d_proj[2]*dp_proj[2])
            c = (dp_proj[0]*dp_proj[0] + dp_proj[1]*dp_proj[1] + dp_proj[2]*dp_proj[2]) - self.radius*self.radius
            disc = b*b - 4.0*a*c
            if disc >= 0:
                sdisc = math.sqrt(disc)
                t0 = (-b - sdisc) / (2.0*a)
                t1 = (-b + sdisc) / (2.0*a)
                t = t0 if t0 > 0.1 else t1
                if t > 0.1:
                    pt = [ro[0] + rd[0]*t, ro[1] + rd[1]*t, ro[2] + rd[2]*t]
                    h = (pt[0]-self.p1[0])*self.dir[0] + (pt[1]-self.p1[1])*self.dir[1] + (pt[2]-self.p1[2])*self.dir[2]
                    if 0.0 <= h <= self.length:
                        axis_pt = [self.p1[0] + self.dir[0]*h, self.p1[1] + self.dir[1]*h, self.p1[2] + self.dir[2]*h]
                        inv_r = 1.0 / self.radius
                        norm = [(pt[0]-axis_pt[0])*inv_r, (pt[1]-axis_pt[1])*inv_r, (pt[2]-axis_pt[2])*inv_r]
                        v = h / self.length
                        dot_tan = norm[0]*self.tangent[0] + norm[1]*self.tangent[1] + norm[2]*self.tangent[2]
                        dot_bit = norm[0]*self.bitangent[0] + norm[1]*self.bitangent[1] + norm[2]*self.bitangent[2]
                        angle = math.atan2(dot_bit, dot_tan)
                        u = (angle + math.pi) / (2.0 * math.pi)
                        seg = min(NUM_LOG_SEGS - 1, max(0, int(v * NUM_LOG_SEGS)))
                        best_t = t
                        best_res = (t, pt, norm, u, v, False, 1.0, seg)

        # Cap 1 (-dir)
        denom1 = rd[0]*(-self.dir[0]) + rd[1]*(-self.dir[1]) + rd[2]*(-self.dir[2])
        if abs(denom1) > 1e-5:
            num1 = (self.p1[0]-ro[0])*(-self.dir[0]) + (self.p1[1]-ro[1])*(-self.dir[1]) + (self.p1[2]-ro[2])*(-self.dir[2])
            t_cap = num1 / denom1
            if 0.1 < t_cap < best_t:
                pt = [ro[0] + rd[0]*t_cap, ro[1] + rd[1]*t_cap, ro[2] + rd[2]*t_cap]
                dist_r = math.sqrt((pt[0]-self.p1[0])**2 + (pt[1]-self.p1[1])**2 + (pt[2]-self.p1[2])**2)
                if dist_r <= self.radius:
                    best_t = t_cap
                    best_res = (t_cap, pt, [-self.dir[0], -self.dir[1], -self.dir[2]], 0.0, 0.0, True, dist_r/self.radius, 0)

        # Cap 2 (+dir)
        denom2 = rd[0]*self.dir[0] + rd[1]*self.dir[1] + rd[2]*self.dir[2]
        if abs(denom2) > 1e-5:
            num2 = (self.p2[0]-ro[0])*self.dir[0] + (self.p2[1]-ro[1])*self.dir[1] + (self.p2[2]-ro[2])*self.dir[2]
            t_cap = num2 / denom2
            if 0.1 < t_cap < best_t:
                pt = [ro[0] + rd[0]*t_cap, ro[1] + rd[1]*t_cap, ro[2] + rd[2]*t_cap]
                dist_r = math.sqrt((pt[0]-self.p2[0])**2 + (pt[1]-self.p2[1])**2 + (pt[2]-self.p2[2])**2)
                if dist_r <= self.radius:
                    best_t = t_cap
                    best_res = (t_cap, pt, [self.dir[0], self.dir[1], self.dir[2]], 0.0, 1.0, True, dist_r/self.radius, NUM_LOG_SEGS - 1)

        return best_res

# 3D Stone Fire Ring Generator
def build_stone_ring():
    ground_y = -4.2
    stones = []
    num_stones = 10
    ring_radius = 7.0
    for i in range(num_stones):
        angle = i * (2.0 * math.pi / num_stones) + 0.15
        sx = ring_radius * math.cos(angle)
        sz = ring_radius * math.sin(angle)
        sr = 0.95 + 0.20 * math.sin(i * 3.7)
        sy = ground_y + sr * 0.82
        stones.append(Stone3D(100 + i, [sx, sy, sz], sr))
    return stones

# Stacking generator functions
def get_stack_log_cabin():
    ground_y = -4.2
    r1, r2, r3 = 1.25, 1.15, 0.95
    span = 3.6
    y1 = ground_y + r1
    y2 = y1 + r1 + r2 - 0.25
    y2_coll = ground_y + r2
    y3 = y2 + r2 + r3 - 0.20
    y3_coll = ground_y + r3 + 0.3

    return [
        SegmentedCylinder3D(1, [-5.0, y1, -span], [5.0, y1, -span], [-5.0, y1, -span], [5.0, y1, -span], r1, 0.25),
        SegmentedCylinder3D(2, [-5.0, y1,  span], [5.0, y1,  span], [-5.0, y1,  span], [5.0, y1,  span], r1, 0.25),
        SegmentedCylinder3D(3, [-span, y2, -5.0], [-span, y2, 5.0], [-span, y2_coll, -4.5], [-span, y2_coll, 4.5], r2, 0.40),
        SegmentedCylinder3D(4, [ span, y2, -5.0], [ span, y2, 5.0], [ span, y2_coll, -4.5], [ span, y2_coll, 4.5], r2, 0.40),
        SegmentedCylinder3D(5, [-4.0, y3, -3.0], [4.0, y3, 3.0], [-2.5, y3_coll, -1.8], [2.5, y3_coll, 1.8], r3, 0.60),
        SegmentedCylinder3D(6, [-4.0, y3,  3.0], [4.0, y3, -3.0], [-2.5, y3_coll,  1.8], [2.5, y3_coll, -1.8], r3, 0.60),
    ]

def get_stack_teepee():
    ground_y = -4.2
    base_r = 4.2
    apex_r = 0.6
    apex_y = 2.4
    r = 1.15
    logs = []
    for i in range(5):
        angle = (i * 2.0 * math.pi / 5.0) + 0.3
        p1 = [base_r * math.cos(angle), ground_y + r, base_r * math.sin(angle)]
        p2 = [apex_r * math.cos(angle), apex_y, apex_r * math.sin(angle)]
        p2_coll = [apex_r * 0.4 * math.cos(angle), ground_y + r + 0.4, apex_r * 0.4 * math.sin(angle)]
        logs.append(SegmentedCylinder3D(i + 1, p1, p2, p1, p2_coll, r, 0.45))
    return logs

def get_stack_pyramid():
    ground_y = -4.2
    r_base = 1.35
    r_cross = 1.10
    y1 = ground_y + r_base
    y_apex = 2.2
    y_coll = ground_y + r_cross + 0.2

    return [
        SegmentedCylinder3D(1, [-5.5, y1, -2.5], [5.5, y1, -2.5], [-5.5, y1, -2.5], [5.5, y1, -2.5], r_base, 0.25),
        SegmentedCylinder3D(2, [-5.5, y1,  2.5], [5.5, y1,  2.5], [-5.5, y1,  2.5], [5.5, y1,  2.5], r_base, 0.25),
        SegmentedCylinder3D(3, [-4.2, y1 + r_base - 0.2, -1.0], [0.0, y_apex, -0.2], [-4.0, y_coll, -0.6], [0.0, y_coll, -0.1], r_cross, 0.50),
        SegmentedCylinder3D(4, [ 4.2, y1 + r_base - 0.2, -1.0], [0.0, y_apex, -0.2], [ 4.0, y_coll, -0.6], [0.0, y_coll, -0.1], r_cross, 0.50),
        SegmentedCylinder3D(5, [ 0.0, y1 + r_base - 0.2,  3.0], [0.0, y_apex,  0.4], [ 0.0, y_coll,  2.4], [0.0, y_coll,  0.2], r_cross, 0.50),
    ]

def main():
    old_settings = termios.tcgetattr(sys.stdin)
    tty.setcbreak(sys.stdin.fileno())

    def cleanup(*args):
        sys.stdout.write("\033[?1049l\033[?25h\033[0m\n")
        sys.stdout.flush()
        termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    sys.stdout.write("\033[?1049h\033[?25l\033[2J")
    sys.stdout.flush()

    try:
        cols, rows = os.get_terminal_size()
    except Exception:
        cols, rows = 80, 24

    pw = min(cols, 120)
    ph = min((rows - 2) * 2, 70)

    stones = build_stone_ring()

    stack_mode = 0 # 0: Log Cabin, 1: Teepee, 2: Pyramid
    stack_funcs = [get_stack_log_cabin, get_stack_teepee, get_stack_pyramid]
    logs = stack_funcs[stack_mode]()

    sim_time = 0.0
    time_scale = 1.0
    wind = 0.0
    wind_tgt = 0.0
    force_collapse = False
    paused = False

    cam_yaw = 0.40
    cam_pitch = 0.35
    auto_turntable = False

    fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
    next_fire = [[0.0 for _ in range(pw)] for _ in range(ph)]
    fire_z = [[0.0 for _ in range(pw)] for _ in range(ph)]
    settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]

    sparks = []
    ash_flakes = []

    while True:
        try:
            cur_cols, cur_rows = os.get_terminal_size()
            if cur_cols != cols or cur_rows != rows:
                cols, rows = cur_cols, cur_rows
                pw = min(cols, 120)
                ph = min((rows - 2) * 2, 70)
                fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
                next_fire = [[0.0 for _ in range(pw)] for _ in range(ph)]
                fire_z = [[0.0 for _ in range(pw)] for _ in range(ph)]
                settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]
                sys.stdout.write("\033[2J")
        except Exception:
            pass

        while True:
            r, _, _ = select.select([sys.stdin], [], [], 0)
            if not r:
                break
            ch = sys.stdin.read(1)
            if ch == '\033':
                seq = ""
                while True:
                    r2, _, _ = select.select([sys.stdin], [], [], 0.005)
                    if not r2: break
                    seq += sys.stdin.read(1)
                    if len(seq) >= 2: break
                if seq == "[A":
                    cam_pitch = min(1.25, cam_pitch + 0.06)
                elif seq == "[B":
                    cam_pitch = max(-0.15, cam_pitch - 0.06)
                elif seq == "[C":
                    cam_yaw += 0.08
                elif seq == "[D":
                    cam_yaw -= 0.08
            elif ch in ('a', 'A', 'h'):
                cam_yaw -= 0.08
            elif ch in ('d', 'D', 'l'):
                cam_yaw += 0.08
            elif ch in ('w', 'W', 'k'):
                cam_pitch = min(1.25, cam_pitch + 0.06)
            elif ch in ('s', 'S', 'j'):
                cam_pitch = max(-0.15, cam_pitch - 0.06)
            elif ch in ('m', 'M'):
                stack_mode = (stack_mode + 1) % 3
                logs = stack_funcs[stack_mode]()
                sim_time = 0.0
                force_collapse = False
                fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
                settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]
                sparks.clear()
                ash_flakes.clear()
            elif ch in ('t', 'T'):
                auto_turntable = not auto_turntable
            elif ch in ('0', 'z', 'Z'):
                cam_yaw = 0.40
                cam_pitch = 0.35
                auto_turntable = False
            elif ch in ('q', 'Q'):
                cleanup()
            elif ch in ('r', 'R'):
                sim_time = 0.0
                force_collapse = False
                logs = stack_funcs[stack_mode]()
                fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
                settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]
                sparks.clear()
                ash_flakes.clear()
            elif ch in ('c', 'C'):
                force_collapse = True
            elif ch == ' ':
                paused = not paused
            elif ch in ('+', '='):
                time_scale = min(8.0, time_scale * 1.4)
            elif ch in ('-', '_'):
                time_scale = max(0.2, time_scale / 1.4)

        if auto_turntable and not paused:
            cam_yaw += 0.015 * time_scale

        if not paused:
            sim_time += 0.045 * time_scale

            if random.random() < 0.05:
                wind_tgt = (random.random() - 0.5) * 1.8
            wind += (wind_tgt - wind) * 0.04

            # -------------------------------------------------------------
            # WOOD COMBUSTION SIMULATION (ZERO FLOOR FIRE!)
            # -------------------------------------------------------------
            kindle_pos = [0.0, -2.6, 0.0]
            kindle_heat = max(0.0, 1.0 - (sim_time / 22.0)) if sim_time < 22.0 else 0.0

            for l in logs:
                for s in range(NUM_LOG_SEGS):
                    t_val = (s + 0.5) / float(NUM_LOG_SEGS)
                    seg_p = [l.p1[k] + t_val * l.axis[k] for k in range(3)]

                    if kindle_heat > 0.05:
                        d_k = math.sqrt(sum((seg_p[k] - kindle_pos[k])**2 for k in range(3)))
                        d_surf = max(0.0, d_k - l.radius - 1.6)
                        if d_surf < 2.8:
                            l.temp[s] += 0.015 * (1.0 - d_surf / 2.8) * kindle_heat * time_scale

                    cur = l.temp[s]
                    prev_t = l.temp[s-1] if s > 0 else cur
                    next_t = l.temp[s+1] if s < NUM_LOG_SEGS - 1 else cur
                    if prev_t > 0.35 or next_t > 0.35:
                        l.temp[s] += 0.0035 * time_scale

                    if l.temp[s] > 0.35:
                        l.temp[s] = min(1.0, l.temp[s] + 0.004 * time_scale)
                        l.burn_progress[s] += 0.00045 * time_scale
                        l.structural_mass[s] = max(0.0, 1.0 - l.burn_progress[s] * 1.15)

                        if l.burn_progress[s] > 0.65 and random.random() < 0.06:
                            ash_p = list(seg_p)
                            ash_p[1] += l.radius * 0.9
                            vel = [(random.random() - 0.5) * 0.5 + wind * 0.4, -(random.random() * 0.4 + 0.2), (random.random() - 0.5) * 0.5]
                            ash_flakes.append([ash_p[0], ash_p[1], ash_p[2], vel[0], vel[1], vel[2], PALETTE_ASH[random.choice([1, 2, 3])]])

                    if l.burn_progress[s] > 0.85:
                        l.temp[s] = max(0.15, l.temp[s] - 0.001 * time_scale)

            # Self-collapse physics
            total_mass = sum(l.structural_mass[s] for l in logs for s in range(NUM_LOG_SEGS))
            total_segs = len(logs) * NUM_LOG_SEGS
            avg_mass = total_mass / float(total_segs)
            collapse_factor = max(0.0, 1.0 - avg_mass)
            if force_collapse: collapse_factor = 1.0

            for l in logs:
                log_loss = 1.0 - (sum(l.structural_mass) / float(NUM_LOG_SEGS))
                c = min(1.0, max(collapse_factor * 0.75, log_loss))
                if force_collapse: c = 1.0

                for k in range(3):
                    l.p1[k] = l.p1_orig[k] * (1.0 - c) + l.p1_collapsed[k] * c
                    l.p2[k] = l.p2_orig[k] * (1.0 - c) + l.p2_collapsed[k] * c
                l.recompute()

            # Camera 3D Orbit Coordinates
            target = [0.0, -1.2, 0.0]
            cam_dist = 28.0
            cam_pos = [
                cam_dist * math.cos(cam_pitch) * math.sin(cam_yaw),
                target[1] + cam_dist * math.sin(cam_pitch),
                -cam_dist * math.cos(cam_pitch) * math.cos(cam_yaw)
            ]

            fx = target[0] - cam_pos[0]
            fy = target[1] - cam_pos[1]
            fz = target[2] - cam_pos[2]
            fl = math.sqrt(fx*fx + fy*fy + fz*fz) + 1e-6
            fwd = [fx/fl, fy/fl, fz/fl]

            rx = fwd[2]
            rz = -fwd[0]
            rl = math.sqrt(rx*rx + rz*rz) + 1e-6
            right = [rx/rl, 0.0, rz/rl]

            up = [
                right[1]*fwd[2] - right[2]*fwd[1],
                right[2]*fwd[0] - right[0]*fwd[2],
                right[0]*fwd[1] - right[1]*fwd[0]
            ]

            world_w, world_h = 22.0, 14.0

            def project_pt(p):
                rx_p = p[0] - cam_pos[0]
                ry_p = p[1] - cam_pos[1]
                rz_p = p[2] - cam_pos[2]
                sx = ((rx_p*right[0] + ry_p*right[1] + rz_p*right[2]) / world_w + 0.5) * pw
                sy = (0.5 - (rx_p*up[0] + ry_p*up[1] + rz_p*up[2]) / world_h) * ph
                sz = rx_p*fwd[0] + ry_p*fwd[1] + rz_p*fwd[2]
                return sx, sy, sz

            # -------------------------------------------------------------
            # FIRE INJECTION: Strictly wood segments & kindling
            # -------------------------------------------------------------
            next_fire = [[0.0 for _ in range(pw)] for _ in range(ph)]

            # Kindling flame
            if kindle_heat > 0.05:
                kx, ky, kz = project_pt(kindle_pos)
                for dy in range(-3, 3):
                    for dx in range(-3, 4):
                        px, py = int(kx) + dx, int(ky) + dy
                        if 0 <= px < pw and 0 <= py < ph:
                            d = math.sqrt((dx * 0.9)**2 + (dy * 1.5)**2)
                            if d < 3.2:
                                fire_heat[py][px] = max(fire_heat[py][px], kindle_heat * 0.85 * (1.0 - d / 3.2))
                                fire_z[py][px] = kz - 0.25

            # Burning wood segments
            for l in logs:
                for s in range(NUM_LOG_SEGS):
                    if l.temp[s] > 0.35 and l.burn_progress[s] < 0.90:
                        t_val = (s + 0.5) / float(NUM_LOG_SEGS)
                        p = [l.p1[k] + t_val * l.axis[k] for k in range(3)]
                        px, py, pz = project_pt(p)
                        flame_h = l.temp[s] * 0.98

                        for dy in range(-4, 3):
                            for dx in range(-4, 5):
                                sx, sy = int(px) + dx, int(py) + dy
                                if 0 <= sx < pw and 0 <= sy < ph:
                                    d = math.sqrt((dx * 0.85)**2 + (dy * 1.5)**2)
                                    if d < 4.2:
                                        fire_heat[sy][sx] = max(fire_heat[sy][sx], flame_h * (1.0 - d / 4.2))
                                        fire_z[sy][sx] = pz - 0.25

            # Convection upwards
            for y in range(ph - 4, 1, -1):
                for x in range(pw):
                    src_x = x
                    if wind > 0.35 and random.random() < 0.4:
                        src_x = min(pw - 1, x + 1)
                    elif wind < -0.35 and random.random() < 0.4:
                        src_x = max(0, x - 1)
                    else:
                        src_x = max(0, min(pw - 1, x + random.choice([-1, 0, 1])))

                    below = fire_heat[y + 1][src_x]
                    if below <= 0.04:
                        next_fire[y][x] = 0.0
                        continue

                    decay = 0.024 + 0.026 * random.random()
                    val = max(0.0, below - decay)
                    next_fire[y][x] = val
                    fire_z[y][x] = fire_z[y + 1][src_x] - 0.02

            for y in range(ph):
                for x in range(pw):
                    fire_heat[y][x] = next_fire[y][x]

            # Sparks & Ash Flakes
            if random.random() < 0.60:
                burning_logs = [l for l in logs if any(t > 0.5 for t in l.temp)]
                if burning_logs:
                    l = random.choice(burning_logs)
                    s = random.randint(0, NUM_LOG_SEGS - 1)
                    t_val = (s + 0.5) / float(NUM_LOG_SEGS)
                    p = [l.p1[k] + t_val * l.axis[k] for k in range(3)]
                    vel = [(random.random() - 0.5) * 1.5 + wind * 1.2, random.random() * 3.2 + 2.0, (random.random() - 0.5) * 1.5]
                    col = PALETTE_EMBERS[random.choice([2, 3, 4])]
                    sparks.append([p[0], p[1], p[2], vel[0], vel[1], vel[2], random.randint(16, 42), col])

            alive_sparks = []
            for s in sparks:
                s[0] += s[3] * 0.05
                s[1] += s[4] * 0.05
                s[2] += s[5] * 0.05
                s[4] -= 0.04
                s[6] -= 1
                if s[6] > 0 and s[1] > -4.5:
                    alive_sparks.append(s)
            sparks = alive_sparks

            alive_flakes = []
            for f in ash_flakes:
                f[0] += f[3] * 0.05 + math.sin(f[1] * 2.0 + sim_time) * 0.02
                f[1] += f[4] * 0.05
                f[2] += f[5] * 0.05
                if f[1] <= -4.15:
                    sx, sy, _ = project_pt([f[0], f[1], f[2]])
                    ix, iy = int(sx), int(sy)
                    if 0 <= ix < pw and 0 <= iy < ph:
                        settled_ash[iy][ix] = 1
                else:
                    alive_flakes.append(f)
            ash_flakes = alive_flakes

        # -------------------------------------------------------------
        # CAMERA PROJECTION & RAYCASTING
        # -------------------------------------------------------------
        id_buf = [[0 for _ in range(pw)] for _ in range(ph)]
        depth_buf = [[1e9 for _ in range(pw)] for _ in range(ph)]
        shade_buf = [[(0, 0, 0) for _ in range(pw)] for _ in range(ph)]
        frame_sky = [[True for _ in range(pw)] for _ in range(ph)]
        frame_col = [[(0, 0, 0) for _ in range(pw)] for _ in range(ph)]

        rd = fwd
        flicker = 1.0 + 0.16 * math.sin(sim_time * 8.0) + 0.10 * math.cos(sim_time * 13.0)
        light_pos = [0.0, -1.8, 0.0]
        light_intensity = 1.9 * flicker

        for y in range(ph):
            wy = (((ph - 1 - y) / ph) - 0.5) * world_h
            for x in range(pw):
                wx = ((x / pw) - 0.5) * world_w
                ro = [
                    cam_pos[0] + wx * right[0] + wy * up[0],
                    cam_pos[1] + wx * right[1] + wy * up[1],
                    cam_pos[2] + wx * right[2] + wy * up[2]
                ]

                closest_t = 1e9
                hit_data = None
                hit_type = None

                # Test stones
                for st in stones:
                    res = st.intersect(ro, rd)
                    if res and res[0] < closest_t:
                        closest_t = res[0]
                        hit_data = (res, st)
                        hit_type = 'stone'

                # Test logs
                for l in logs:
                    res = l.intersect(ro, rd)
                    if res and res[0] < closest_t:
                        closest_t = res[0]
                        hit_data = (res, l)
                        hit_type = 'log'

                if hit_type == 'stone':
                    res, st = hit_data
                    t, pt, norm = res
                    id_buf[y][x] = st.obj_id
                    depth_buf[y][x] = (pt[0]-cam_pos[0])*fwd[0] + (pt[1]-cam_pos[1])*fwd[1] + (pt[2]-cam_pos[2])*fwd[2]

                    l_vec = [light_pos[k] - pt[k] for k in range(3)]
                    ldist = math.sqrt(sum(k*k for k in l_vec)) + 1e-6
                    ldir = [k / ldist for k in l_vec]
                    atten = 1.0 / (1.0 + 0.07 * ldist + 0.015 * ldist * ldist)
                    ndotl = max(0.0, sum(norm[k] * ldir[k] for k in range(3)))
                    ambient = 0.22 + 0.10 * max(0.0, norm[1])
                    s_val = ndotl * atten * light_intensity * 2.2 + ambient

                    rock_noise = (math.sin(pt[0] * 3.5 + pt[2] * 4.1) * 0.5 + 0.5) * 0.18
                    s_idx = min(5, max(1, int((s_val + rock_noise) * 2.8)))
                    shade_buf[y][x] = PALETTE_STONE[s_idx]

                elif hit_type == 'log':
                    res, l = hit_data
                    t, pt, norm, u, v, is_cap, rf, hit_seg = res
                    id_buf[y][x] = l.obj_id
                    depth_buf[y][x] = (pt[0]-cam_pos[0])*fwd[0] + (pt[1]-cam_pos[1])*fwd[1] + (pt[2]-cam_pos[2])*fwd[2]

                    l_vec = [light_pos[k] - pt[k] for k in range(3)]
                    ldist = math.sqrt(sum(k*k for k in l_vec)) + 1e-6
                    ldir = [k / ldist for k in l_vec]
                    atten = 1.0 / (1.0 + 0.08 * ldist + 0.015 * ldist * ldist)
                    ndotl = max(0.0, (sum(norm[k] * ldir[k] for k in range(3)) + 0.45) / 1.45)
                    ambient = 0.28 + 0.12 * max(0.0, norm[1])
                    lval = (ndotl * atten * light_intensity * 2.4 + ambient)

                    burn = l.burn_progress[hit_seg]
                    seg_temp = l.temp[hit_seg]

                    if is_cap:
                        r_q = math.floor(rf * 8.0) / 8.0
                        ring_band = int(r_q * 8.0) % 2
                        col_idx = min(4, max(0, 1 + ring_band + (1 if rf > 0.70 else 0) + (1 if lval > 0.80 else 0)))
                        shade_buf[y][x] = PALETTE_ENDCAP[col_idx]
                    else:
                        num_plates_u = 14.0
                        num_plates_v = l.length * 2.2
                        u_plate = math.floor(u * num_plates_u)
                        v_plate = math.floor(v * num_plates_v)
                        plate_hash = math.sin(u_plate * 12.9898 + v_plate * 78.233) * 43758.5453
                        plate_var = (plate_hash - math.floor(plate_hash)) * 0.25 - 0.12

                        u_frac = (u * num_plates_u) - u_plate
                        v_frac = (v * num_plates_v) - v_plate
                        is_furrow = (u_frac < 0.12 or u_frac > 0.88 or (v_frac < 0.08 and (u_plate % 2 == 0)))

                        if burn > 0.75:
                            ash_idx = min(4, max(0, int((lval + plate_var) * 2.4)))
                            shade_buf[y][x] = PALETTE_ASH[ash_idx]
                        elif burn > 0.40:
                            if is_furrow and seg_temp > 0.50:
                                emb_idx = min(3, max(0, int(seg_temp * 3.5)))
                                shade_buf[y][x] = PALETTE_EMBERS[emb_idx]
                            else:
                                c_idx = min(4, max(0, int((lval + plate_var) * 2.0)))
                                shade_buf[y][x] = PALETTE_CHARRED[c_idx]
                        elif burn > 0.15:
                            if is_furrow:
                                shade_buf[y][x] = PALETTE_CHARRED[0]
                            else:
                                b_idx = min(4, max(1, int((lval + plate_var) * 2.0)))
                                shade_buf[y][x] = PALETTE_WOOD[b_idx]
                        else:
                            if is_furrow:
                                shade_buf[y][x] = PALETTE_WOOD[0]
                            else:
                                b_idx = min(6, max(1, int((lval + plate_var) * 2.8)))
                                shade_buf[y][x] = PALETTE_WOOD[b_idx]

        # 1-Pixel Cel Outline
        for y in range(ph):
            for x in range(pw):
                cid = id_buf[y][x]
                if cid == 0: continue
                is_edge = False
                for dy, dx in ((-1,0), (1,0), (0,-1), (0,1)):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < ph and 0 <= nx < pw:
                        nid = id_buf[ny][nx]
                        if nid != cid:
                            if nid == 0 or depth_buf[y][x] < depth_buf[ny][nx]:
                                is_edge = True
                                break
                        elif abs(depth_buf[y][x] - depth_buf[ny][nx]) > 0.75:
                            is_edge = True
                            break

                if is_edge:
                    frame_col[y][x] = (18, 14, 12)
                    frame_sky[y][x] = False
                else:
                    frame_col[y][x] = shade_buf[y][x]
                    frame_sky[y][x] = False

        # Composite Fire Heat Layer with Depth Buffer
        for y in range(ph):
            for x in range(pw):
                heat = fire_heat[y][x]
                if heat > 0.05:
                    if heat > 0.85: fcol = PALETTE_EMBERS[4]
                    elif heat > 0.65: fcol = PALETTE_EMBERS[3]
                    elif heat > 0.40: fcol = PALETTE_EMBERS[2]
                    elif heat > 0.18: fcol = PALETTE_EMBERS[1]
                    else: fcol = PALETTE_EMBERS[0]

                    if id_buf[y][x] > 0:
                        if fire_z[y][x] < depth_buf[y][x] or heat > 0.55:
                            frame_col[y][x] = fcol
                            frame_sky[y][x] = False
                    else:
                        frame_col[y][x] = fcol
                        frame_sky[y][x] = False

        # Settled ash
        for y in range(ph):
            for x in range(pw):
                if settled_ash[y][x]:
                    frame_col[y][x] = PALETTE_ASH[2 if (x+y)%2 == 0 else 3]
                    frame_sky[y][x] = False

        # Flakes
        for f in ash_flakes:
            sx, sy, sz = project_pt([f[0], f[1], f[2]])
            ix, iy = int(sx), int(sy)
            if 0 <= ix < pw and 0 <= iy < ph:
                if sz < depth_buf[iy][ix]:
                    frame_col[iy][ix] = f[6]
                    frame_sky[iy][ix] = False

        # Sparks
        for s in sparks:
            sx, sy, sz = project_pt([s[0], s[1], s[2]])
            ix, iy = int(sx), int(sy)
            if 0 <= ix < pw and 0 <= iy < ph:
                if sz < depth_buf[iy][ix] or fire_heat[iy][ix] > 0.2:
                    frame_col[iy][ix] = s[7]
                    frame_sky[iy][ix] = False

        # Present frame
        out = ["\033[H"]
        p_fg, p_bg, p_transp = None, None, True

        for tr in range(ph // 2):
            yt = tr * 2
            yb = tr * 2 + 1
            for x in range(pw):
                st, sb = frame_sky[yt][x], frame_sky[yb][x]
                ct, cb = frame_col[yt][x], frame_col[yb][x]

                if st and sb:
                    if not p_transp:
                        out.append("\033[49m")
                        p_transp = True
                        p_bg = None
                    out.append(" ")
                elif st and not sb:
                    if cb != p_fg:
                        out.append(f"\033[38;2;{cb[0]};{cb[1]};{cb[2]}m")
                        p_fg = cb
                    if not p_transp:
                        out.append("\033[49m")
                        p_transp = True
                        p_bg = None
                    out.append("▄")
                elif not st and sb:
                    if ct != p_fg:
                        out.append(f"\033[38;2;{ct[0]};{ct[1]};{ct[2]}m")
                        p_fg = ct
                    if not p_transp:
                        out.append("\033[49m")
                        p_transp = True
                        p_bg = None
                    out.append("▀")
                else:
                    if ct != p_fg:
                        out.append(f"\033[38;2;{ct[0]};{ct[1]};{ct[2]}m")
                        p_fg = ct
                    if cb != p_bg:
                        out.append(f"\033[48;2;{cb[0]};{cb[1]};{cb[2]}m")
                        p_bg = cb
                        p_transp = False
                    out.append("▀")
            out.append("\033[0m\n")
            p_fg, p_bg, p_transp = None, None, True

        stage_name = "1/7: Gravetos e Ignição"
        if sim_time > 180.0: stage_name = "7/7: Cinzas Frias"
        elif sim_time > 140.0: stage_name = "6/7: Leito de Brasas"
        elif sim_time > 100.0: stage_name = "5/7: Colapso por Gravidade"
        elif sim_time > 65.0: stage_name = "4/7: Madeira em Cinza"
        elif sim_time > 28.0: stage_name = "3/7: Fogueira Roaring"
        elif sim_time > 10.0: stage_name = "2/7: Chamas nas Toras"

        stack_name = ["Fogueira Quadrada", "Tenda Cônica", "Pirâmide"][stack_mode]
        yaw_deg = int(math.degrees(cam_yaw)) % 360
        pitch_deg = int(math.degrees(cam_pitch))
        tt_status = " [Turntable ON]" if auto_turntable else ""
        out.append(f"\033[1;33m[3D Bonfire]\033[0m {sim_time:4.0f}s | Base: Pedras | Madeira: \033[1;32m{stack_name}\033[0m | Fase: \033[1;37m{stage_name}\033[0m | Cam: {yaw_deg:3d}°/{pitch_deg:2d}°{tt_status} | [m] Modo | [Setas/WASD] Girar | [t] Turntable | [c] Colapsar | [q] Sair ")

        sys.stdout.write("".join(out))
        sys.stdout.flush()

        time.sleep(0.028)

if __name__ == '__main__':
    main()
