#!/usr/bin/env python3
"""
3D Per-Object Pixel Art Bonfire Simulation (Python Edition)

Features:
- 3D Spherical Orbit Camera (Yaw θ, Pitch φ) with real-time controls & turntable
- 3 Physical Wood Stacking Modes: Fogueira Quadrada (Log Cabin), Tenda Cônica (Teepee), Pirâmide
- Discrete Object-Space Bark Plates (No periodic orange stripes, zero pixel creep)
- Concentric Growth Rings on Cut End-Caps
- Localized Charring & Crevice Embers
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
    (200, 145, 102)   # 6: Warm firelit rim (natural, NOT neon orange!)
]

PALETTE_CHARRED = [
    (16, 12, 12),     # 0: Black crevice
    (30, 26, 26),     # 1: Charred black bark
    (52, 48, 48),     # 2: Dark charcoal
    (80, 72, 68),     # 3: Burnt ash bark
    (115, 110, 108)   # 4: Ash surface
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
    (48, 45, 52),     # 0: Charcoal crust
    (88, 84, 94),     # 1: Dark ash
    (135, 132, 142),  # 2: Mid ash grey
    (185, 182, 192),  # 3: Light chalky ash
    (230, 228, 235)   # 4: White ash powder
]

class Cylinder3D:
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
        self.wood_health = 100.0
        self.ash_amount = 0.0
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
                        best_t = t
                        best_res = (t, pt, norm, u, v, False, 1.0)

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
                    best_res = (t_cap, pt, [-self.dir[0], -self.dir[1], -self.dir[2]], 0.0, 0.0, True, dist_r/self.radius)

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
                    best_res = (t_cap, pt, [self.dir[0], self.dir[1], self.dir[2]], 0.0, 1.0, True, dist_r/self.radius)

        return best_res

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
        Cylinder3D(1, [-5.0, y1, -span], [5.0, y1, -span], [-5.0, y1, -span], [5.0, y1, -span], r1, 0.25),
        Cylinder3D(2, [-5.0, y1,  span], [5.0, y1,  span], [-5.0, y1,  span], [5.0, y1,  span], r1, 0.25),
        Cylinder3D(3, [-span, y2, -5.0], [-span, y2, 5.0], [-span, y2_coll, -4.6], [-span, y2_coll, 4.6], r2, 0.40),
        Cylinder3D(4, [ span, y2, -5.0], [ span, y2, 5.0], [ span, y2_coll, -4.6], [ span, y2_coll, 4.6], r2, 0.40),
        Cylinder3D(5, [-4.0, y3, -3.0], [4.0, y3, 3.0], [-2.5, y3_coll, -1.8], [2.5, y3_coll, 1.8], r3, 0.60),
        Cylinder3D(6, [-4.0, y3,  3.0], [4.0, y3, -3.0], [-2.5, y3_coll,  1.8], [2.5, y3_coll, -1.8], r3, 0.60),
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
        logs.append(Cylinder3D(i + 1, p1, p2, p1, p2_coll, r, 0.45))
    return logs

def get_stack_pyramid():
    ground_y = -4.2
    r_base = 1.35
    r_cross = 1.10
    y1 = ground_y + r_base
    y_apex = 2.2
    y_coll = ground_y + r_cross + 0.2

    return [
        Cylinder3D(1, [-5.5, y1, -2.5], [5.5, y1, -2.5], [-5.5, y1, -2.5], [5.5, y1, -2.5], r_base, 0.25),
        Cylinder3D(2, [-5.5, y1,  2.5], [5.5, y1,  2.5], [-5.5, y1,  2.5], [5.5, y1,  2.5], r_base, 0.25),
        Cylinder3D(3, [-4.2, y1 + r_base - 0.2, -1.0], [0.0, y_apex, -0.2], [-4.0, y_coll, -0.6], [0.0, y_coll, -0.1], r_cross, 0.50),
        Cylinder3D(4, [ 4.2, y1 + r_base - 0.2, -1.0], [0.0, y_apex, -0.2], [ 4.0, y_coll, -0.6], [0.0, y_coll, -0.1], r_cross, 0.50),
        Cylinder3D(5, [ 0.0, y1 + r_base - 0.2,  3.0], [0.0, y_apex,  0.4], [ 0.0, y_coll,  2.4], [0.0, y_coll,  0.2], r_cross, 0.50),
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

    stack_mode = 0 # 0: Log Cabin, 1: Teepee, 2: Pyramid
    stack_funcs = [get_stack_log_cabin, get_stack_teepee, get_stack_pyramid]
    logs = stack_funcs[stack_mode]()

    sim_time = 0.0
    time_scale = 1.0
    wind = 0.0
    wind_tgt = 0.0
    collapse = 0.0
    force_collapse = False
    paused = False

    # Camera Orbit Parameters
    cam_yaw = 0.40
    cam_pitch = 0.35
    auto_turntable = False

    fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
    next_fire = [[0.0 for _ in range(pw)] for _ in range(ph)]
    fire_z = [[0.0 for _ in range(pw)] for _ in range(ph)]
    settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]

    sparks = []
    ash_flakes = []

    # Initial ignition
    cradle_y = int(ph * 0.72)
    cx = pw // 2
    fire_heat[cradle_y][cx] = 0.65

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

        # Handle non-blocking keyboard input
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
                if seq == "[A": # Up arrow -> pitch up
                    cam_pitch = min(1.25, cam_pitch + 0.06)
                elif seq == "[B": # Down arrow -> pitch down
                    cam_pitch = max(-0.15, cam_pitch - 0.06)
                elif seq == "[C": # Right arrow -> yaw right
                    cam_yaw += 0.08
                elif seq == "[D": # Left arrow -> yaw left
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
                collapse = 0.0
                force_collapse = False
                fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
                settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]
                sparks.clear()
                ash_flakes.clear()
                fire_heat[int(ph * 0.72)][pw // 2] = 0.65
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
                collapse = 0.0
                force_collapse = False
                logs = stack_funcs[stack_mode]()
                fire_heat = [[0.0 for _ in range(pw)] for _ in range(ph)]
                settled_ash = [[0 for _ in range(pw)] for _ in range(ph)]
                sparks.clear()
                ash_flakes.clear()
                fire_heat[int(ph * 0.72)][pw // 2] = 0.65
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

            # Combustion intensity
            if sim_time < 12.0:
                intensity = 0.20 + (sim_time / 12.0) * 0.35
            elif sim_time < 35.0:
                intensity = 0.55 + ((sim_time - 12.0) / 23.0) * 0.45
            elif sim_time < 85.0:
                intensity = 1.0
            elif sim_time < 125.0:
                intensity = 0.88
            elif sim_time < 155.0:
                intensity = 0.65
            elif sim_time < 200.0:
                intensity = 0.35
            else:
                fade = (sim_time - 200.0) / 45.0
                intensity = max(0.0, 0.22 - fade * 0.22)

            # Physical gravity collapse
            if (sim_time > 115.0 or force_collapse) and collapse < 1.0:
                collapse = min(1.0, collapse + 0.015 * time_scale)
                for l in logs:
                    l.p1[0] = l.p1_orig[0] * (1.0 - collapse) + l.p1_collapsed[0] * collapse
                    l.p1[1] = l.p1_orig[1] * (1.0 - collapse) + l.p1_collapsed[1] * collapse
                    l.p1[2] = l.p1_orig[2] * (1.0 - collapse) + l.p1_collapsed[2] * collapse
                    l.p2[0] = l.p2_orig[0] * (1.0 - collapse) + l.p2_collapsed[0] * collapse
                    l.p2[1] = l.p2_orig[1] * (1.0 - collapse) + l.p2_collapsed[1] * collapse
                    l.p2[2] = l.p2_orig[2] * (1.0 - collapse) + l.p2_collapsed[2] * collapse
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

            # 3D projected screen positions for fire emitters
            left_spire_w = [-1.2, -1.6, 0.4]
            right_spire_w = [1.0, -1.6, -0.2]
            core_w = [0.0, -1.8, 0.0]

            def project_pt(p):
                rx_p = p[0] - cam_pos[0]
                ry_p = p[1] - cam_pos[1]
                rz_p = p[2] - cam_pos[2]
                sx = ((rx_p*right[0] + ry_p*right[1] + rz_p*right[2]) / world_w + 0.5) * pw
                sy = (0.5 - (rx_p*up[0] + ry_p*up[1] + rz_p*up[2]) / world_h) * ph
                sz = rx_p*fwd[0] + ry_p*fwd[1] + rz_p*fwd[2]
                return sx, sy, sz

            left_spire_x, _, _ = project_pt(left_spire_w)
            right_spire_x, _, _ = project_pt(right_spire_w)
            cx_f, cy_f, _ = project_pt(core_w)
            cx = int(cx_f)
            cradle_y = max(10, min(ph - 4, int(cy_f)))

            # 1. Heat from burning wood contacts
            for l in logs:
                if intensity < 0.15: continue
                num_pts = int(l.length * 6)
                for s in range(num_pts):
                    t_val = s / float(num_pts)
                    px = l.p1[0] + t_val * l.axis[0]
                    py = l.p1[1] + t_val * l.axis[1]
                    pz = l.p1[2] + t_val * l.axis[2]
                    dist_c = math.sqrt(px*px + (py + 1.8)**2 + pz*pz)
                    if dist_c < 4.5:
                        gx_f, gy_f, gz = project_pt([px, py, pz])
                        gx, gy = int(gx_f), int(gy_f)
                        if 0 <= gx < pw and 0 <= gy < ph:
                            wfire = (0.92 - (dist_c / 4.5) * 0.25) * intensity
                            fire_heat[gy][gx] = max(fire_heat[gy][gx], wfire)
                            fire_z[gy][gx] = gz - 0.25

            # 2. Glowing cradle ember bed
            for dy in range(-4, 5):
                for dx in range(-14, 15):
                    d = math.sqrt((dx * 0.75)**2 + (dy * 1.8)**2)
                    if d < 12.0:
                        h = (1.0 - (d / 12.0)**1.8) * intensity
                        gx, gy = cx + dx, cradle_y + dy
                        if 0 <= gx < pw and 0 <= gy < ph:
                            fire_heat[gy][gx] = max(fire_heat[gy][gx], h)
                            _, _, gz = project_pt(core_w)
                            fire_z[gy][gx] = gz - 0.4

            # 3. Convective flame propagation
            for y in range(cradle_y, 2, -1):
                hr = (cradle_y - y) / float(cradle_y)
                for x in range(pw):
                    src_x = x
                    if wind > 0.35 and random.random() < 0.4:
                        src_x = min(pw - 1, x + 1)
                    elif wind < -0.35 and random.random() < 0.4:
                        src_x = max(0, x - 1)
                    else:
                        src_x = max(0, min(pw - 1, x + random.choice([-1, 0, 1])))

                    below = fire_heat[y + 1][src_x]
                    if below <= 0.03:
                        next_fire[y][x] = 0.0
                        continue

                    decay = 0.020 + 0.028 * random.random()
                    dl = abs(x - (left_spire_x + math.sin(y * 0.14 + sim_time) * 1.8))
                    dr = abs(x - (right_spire_x + math.sin(y * 0.18 + sim_time) * 2.2))
                    dc = abs(x - cx)

                    is_left = (dl < 8.0) and (hr < 0.78)
                    is_right = (dr < 9.5) and (hr < 0.94)
                    is_core = (dc < 6.5) and (hr < 0.52)

                    if is_left or is_right or is_core:
                        decay *= 0.48
                    else:
                        decay *= 2.6

                    val = max(0.0, below - decay)
                    next_fire[y][x] = val

                    _, _, gz_core = project_pt(core_w)
                    if is_left:
                        _, _, gz_l = project_pt(left_spire_w)
                        fire_z[y][x] = gz_l - 0.2
                    elif is_right:
                        _, _, gz_r = project_pt(right_spire_w)
                        fire_z[y][x] = gz_r - 0.2
                    else:
                        fire_z[y][x] = gz_core - 0.45

            for y in range(ph):
                for x in range(pw):
                    fire_heat[y][x] = next_fire[y][x]

            # 4. Particles (Sparks & Ash)
            if intensity > 0.3 and len(sparks) < 55 and random.random() < 0.75:
                sx = cx_f + (random.random() - 0.5) * 16.0
                sy = float(cradle_y) - 2.0
                vx = (random.random() - 0.5) * 1.5 + wind * 1.8
                vy = -(random.random() * 2.2 + 1.2)
                col = PALETTE_EMBERS[random.choice([2, 3, 4])]
                sparks.append([sx, sy, vx, vy, random.randint(18, 48), col])

            alive_sparks = []
            for s in sparks:
                s[0] += s[2]
                s[1] += s[3]
                s[3] += 0.04
                s[4] -= 1
                if s[4] > 0 and 0 <= s[0] < pw and 0 <= s[1] < ph:
                    alive_sparks.append(s)
            sparks = alive_sparks

            if intensity > 0.4 and len(ash_flakes) < 45 and random.random() < 0.45:
                ax = cx_f + (random.random() - 0.5) * 32.0
                ay = max(2.0, float(cradle_y) - 18.0)
                vx = (random.random() - 0.5) * 0.9 + wind * 1.4
                vy = random.random() * 0.6 + 0.3
                ash_col = PALETTE_ASH[random.choice([1, 2, 3])]
                ash_flakes.append([ax, ay, vx, vy, ash_col])

            alive_flakes = []
            for f in ash_flakes:
                f[0] += f[2] + math.sin(f[1] * 0.2 + sim_time) * 0.35
                f[1] += f[3]
                ix, iy = int(round(f[0])), int(round(f[1]))
                if 0 <= ix < pw and iy < ph:
                    if iy >= int(ph * 0.85) or settled_ash[iy][ix]:
                        settled_ash[min(ph - 1, iy)][ix] = 1
                    else:
                        alive_flakes.append(f)
            ash_flakes = alive_flakes

        # Camera Projection & Raycasting
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
                hit_log = None

                for l in logs:
                    res = l.intersect(ro, rd)
                    if res and res[0] < closest_t:
                        closest_t = res[0]
                        hit_data = res
                        hit_log = l

                if hit_log:
                    t, pt, norm, u, v, is_cap, rf = hit_data
                    id_buf[y][x] = hit_log.obj_id
                    depth_buf[y][x] = (pt[0]-cam_pos[0])*fwd[0] + (pt[1]-cam_pos[1])*fwd[1] + (pt[2]-cam_pos[2])*fwd[2]

                    # Lighting
                    lx, ly, lz = light_pos[0]-pt[0], light_pos[1]-pt[1], light_pos[2]-pt[2]
                    ldist = math.sqrt(lx*lx + ly*ly + lz*lz) + 1e-6
                    inv_ld = 1.0 / ldist
                    ldir = [lx*inv_ld, ly*inv_ld, lz*inv_ld]
                    atten = 1.0 / (1.0 + 0.08*ldist + 0.015*ldist*ldist)
                    
                    dot_raw = norm[0]*ldir[0] + norm[1]*ldir[1] + norm[2]*ldir[2]
                    ndotl = max(0.0, (dot_raw + 0.45) / 1.45)
                    ambient = 0.28 + 0.12 * max(0.0, norm[1])
                    lval = (ndotl * atten * light_intensity * 2.4 + ambient)

                    if is_cap:
                        r_q = math.floor(rf * 8.0) / 8.0
                        ring_band = int(r_q * 8.0) % 2
                        col_idx = min(4, max(0, 1 + ring_band + (1 if rf > 0.70 else 0) + (1 if lval > 0.80 else 0)))
                        shade_buf[y][x] = PALETTE_ENDCAP[col_idx]
                    else:
                        num_plates_u = 14.0
                        num_plates_v = hit_log.length * 2.2
                        u_plate = math.floor(u * num_plates_u)
                        v_plate = math.floor(v * num_plates_v)
                        
                        plate_hash = math.sin(u_plate * 12.9898 + v_plate * 78.233) * 43758.5453
                        plate_var = (plate_hash - math.floor(plate_hash)) * 0.25 - 0.12

                        u_frac = (u * num_plates_u) - u_plate
                        v_frac = (v * num_plates_v) - v_plate
                        is_furrow = (u_frac < 0.12 or u_frac > 0.88 or (v_frac < 0.08 and (u_plate % 2 == 0)))

                        dist_to_core = math.sqrt(pt[0]*pt[0] + (pt[1] + 1.8)**2 + pt[2]*pt[2])
                        heat_exposure = max(0.0, 1.0 - dist_to_core / 4.8) * hit_log.charred

                        if hit_log.ash_amount > 40.0:
                            ash_idx = min(4, max(0, int((lval + plate_var) * 2.2)))
                            shade_buf[y][x] = PALETTE_ASH[ash_idx]
                        elif heat_exposure > 0.45:
                            if is_furrow and heat_exposure > 0.60:
                                emb_idx = min(3, max(0, int(heat_exposure * 3.5)))
                                shade_buf[y][x] = PALETTE_EMBERS[emb_idx]
                            else:
                                c_idx = min(4, max(0, int((lval + plate_var) * 2.2)))
                                shade_buf[y][x] = PALETTE_CHARRED[c_idx]
                        elif is_furrow:
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
                    frame_col[y][x] = PALETTE_WOOD[0]
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
                        if fire_z[y][x] < depth_buf[y][x]:
                            frame_col[y][x] = fcol
                            frame_sky[y][x] = False
                        elif heat > 0.55:
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
            ix, iy = int(round(f[0])), int(round(f[1]))
            if 0 <= ix < pw and 0 <= iy < ph:
                frame_col[iy][ix] = f[4]
                frame_sky[iy][ix] = False

        # Sparks
        for s in sparks:
            ix, iy = int(round(s[0])), int(round(s[1]))
            if 0 <= ix < pw and 0 <= iy < ph:
                frame_col[iy][ix] = s[5]
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
        if sim_time > 200.0: stage_name = "7/7: Cinzas Frias"
        elif sim_time > 155.0: stage_name = "6/7: Leito de Brasas"
        elif sim_time > 125.0: stage_name = "5/7: Colapso por Gravidade"
        elif sim_time > 85.0: stage_name = "4/7: Madeira em Cinza"
        elif sim_time > 35.0: stage_name = "3/7: Fogueira Roaring"
        elif sim_time > 12.0: stage_name = "2/7: Chamas nas Toras"

        stack_name = ["Fogueira Quadrada", "Tenda Cônica", "Pirâmide"][stack_mode]
        yaw_deg = int(math.degrees(cam_yaw)) % 360
        pitch_deg = int(math.degrees(cam_pitch))
        tt_status = " [Turntable ON]" if auto_turntable else ""
        out.append(f"\033[1;33m[3D Bonfire]\033[0m {sim_time:4.0f}s | Madeira: \033[1;32m{stack_name}\033[0m | Fase: \033[1;37m{stage_name}\033[0m | Cam: {yaw_deg:3d}°/{pitch_deg:2d}°{tt_status} | [m] Modo | [Setas/WASD] Girar | [t] Turntable | [c] Colapsar | [q] Sair ")

        sys.stdout.write("".join(out))
        sys.stdout.flush()

        time.sleep(0.028)

if __name__ == '__main__':
    main()
