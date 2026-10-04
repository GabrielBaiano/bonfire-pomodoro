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
    def __init__(self, obj_id, center, u_tan, v_up, w_rad, ru, rv, rw, shade_var=0.0):
        self.obj_id = obj_id
        self.center = list(center)
        self.u_tan = list(u_tan)
        self.v_up = list(v_up)
        self.w_rad = list(w_rad)
        self.ru = ru
        self.rv = rv
        self.rw = rw
        self.shade_var = shade_var

    def intersect(self, ro, rd):
        oc = [ro[0] - self.center[0], ro[1] - self.center[1], ro[2] - self.center[2]]
        inv_u = 1.0 / self.ru
        inv_v = 1.0 / self.rv
        inv_w = 1.0 / self.rw

        p0 = [
            (oc[0]*self.u_tan[0] + oc[1]*self.u_tan[1] + oc[2]*self.u_tan[2]) * inv_u,
            (oc[0]*self.v_up[0]  + oc[1]*self.v_up[1]  + oc[2]*self.v_up[2])  * inv_v,
            (oc[0]*self.w_rad[0] + oc[1]*self.w_rad[1] + oc[2]*self.w_rad[2]) * inv_w
        ]
        d0 = [
            (rd[0]*self.u_tan[0] + rd[1]*self.u_tan[1] + rd[2]*self.u_tan[2]) * inv_u,
            (rd[0]*self.v_up[0]  + rd[1]*self.v_up[1]  + rd[2]*self.v_up[2])  * inv_v,
            (rd[0]*self.w_rad[0] + rd[1]*self.w_rad[1] + rd[2]*self.w_rad[2]) * inv_w
        ]

        a = d0[0]*d0[0] + d0[1]*d0[1] + d0[2]*d0[2]
        b = p0[0]*d0[0] + p0[1]*d0[1] + p0[2]*d0[2]
        c = (p0[0]*p0[0] + p0[1]*p0[1] + p0[2]*p0[2]) - 1.0
        disc = b * b - a * c
        if disc < 0:
            return None
        sdisc = math.sqrt(disc)
        t = (-b - sdisc) / a
        if t < 0.1:
            t = (-b + sdisc) / a
        if t < 0.1:
            return None

        pt = [ro[0] + t * rd[0], ro[1] + t * rd[1], ro[2] + t * rd[2]]
        loc_p = [p0[0] + t * d0[0], p0[1] + t * d0[1], p0[2] + t * d0[2]]
        loc_n = [loc_p[0] * inv_u, loc_p[1] * inv_v, loc_p[2] * inv_w]
        world_n = [
            loc_n[0]*self.u_tan[0] + loc_n[1]*self.v_up[0] + loc_n[2]*self.w_rad[0],
            loc_n[0]*self.u_tan[1] + loc_n[1]*self.v_up[1] + loc_n[2]*self.w_rad[1],
            loc_n[0]*self.u_tan[2] + loc_n[1]*self.v_up[2] + loc_n[2]*self.w_rad[2]
        ]
        n_len = math.sqrt(world_n[0]*world_n[0] + world_n[1]*world_n[1] + world_n[2]*world_n[2]) + 1e-6
        norm = [world_n[0] / n_len, world_n[1] / n_len, world_n[2] / n_len]
        return t, pt, norm

class AshBed3D:
    def __init__(self, ground_y=-4.2, radius_xz=5.6, height=0.35):
        self.center = [0.0, ground_y, 0.0]
        self.radius_xz = radius_xz
        self.height = height
        self.heat = 0.35
        self.volume = 0.0

    def intersect(self, ro, rd):
        if self.height < 0.08:
            return None
        inv_r = 1.0 / self.radius_xz
        inv_h = 1.0 / self.height

        p0 = [(ro[0] - self.center[0]) * inv_r, (ro[1] - self.center[1]) * inv_h, (ro[2] - self.center[2]) * inv_r]
        d0 = [rd[0] * inv_r, rd[1] * inv_h, rd[2] * inv_r]

        a = d0[0]*d0[0] + d0[1]*d0[1] + d0[2]*d0[2]
        b = p0[0]*d0[0] + p0[1]*d0[1] + p0[2]*d0[2]
        c = (p0[0]*p0[0] + p0[1]*p0[1] + p0[2]*p0[2]) - 1.0
        disc = b * b - a * c
        if disc < 0:
            return None
        sdisc = math.sqrt(disc)
        t1 = (-b - sdisc) / a
        t2 = (-b + sdisc) / a
        hit_t = -1.0
        if t1 > 0.1:
            pt1_y = ro[1] + t1 * rd[1]
            if pt1_y >= self.center[1] - 0.05:
                hit_t = t1
        if hit_t < 0 and t2 > 0.1:
            pt2_y = ro[1] + t2 * rd[1]
            if pt2_y >= self.center[1] - 0.05:
                hit_t = t2
        if hit_t < 0.1:
            return None
        pt = [ro[0] + hit_t * rd[0], ro[1] + hit_t * rd[1], ro[2] + hit_t * rd[2]]
        n_raw = [
            (pt[0] - self.center[0]) * (inv_r * inv_r),
            max(0.08, pt[1] - self.center[1]) * (inv_h * inv_h),
            (pt[2] - self.center[2]) * (inv_r * inv_r)
        ]
        n_len = math.sqrt(n_raw[0]*n_raw[0] + n_raw[1]*n_raw[1] + n_raw[2]*n_raw[2])
        norm = [n_raw[0] / n_len, n_raw[1] / n_len, n_raw[2] / n_len]
        return hit_t, pt, norm

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
        self.collapse_cur = 0.0
        self.collapse_speed = 0.0
        self.support_log1 = -1
        self.support_log2 = -1
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
    num_stones = 16
    ring_radius = 6.8
    for i in range(num_stones):
        base_angle = i * (2.0 * math.pi / num_stones)
        angle = base_angle + 0.09 * math.sin(i * 2.3 + 1.2)
        r_dist = ring_radius + 0.35 * math.sin(i * 3.7 + 0.5)

        ru = 1.45 + 0.20 * math.sin(i * 4.1)
        rv = 0.78 + 0.15 * math.cos(i * 2.7)
        rw = 1.10 + 0.18 * math.sin(i * 5.3)

        c = [
            r_dist * math.cos(angle),
            ground_y + rv * 0.82 + 0.06 * math.sin(i * 1.9),
            r_dist * math.sin(angle)
        ]

        u = [-math.sin(angle), 0.0, math.cos(angle)]
        v = [0.0, 1.0, 0.0]
        w = [math.cos(angle), 0.0, math.sin(angle)]
        shade_var = 0.12 * math.sin(i * 4.8 + 2.1)

        stones.append(Stone3D(100 + i, c, u, v, w, ru, rv, rw, shade_var))
    return stones

# Stacking generator functions
def get_stack_log_cabin():
    ground_y = -4.2
    span = 3.6

    r0 = 1.32
    y0 = ground_y + r0
    l0 = SegmentedCylinder3D(1, [-4.8, y0, -span - 0.15], [5.3, y0, -span + 0.15], [-4.8, y0, -span - 0.15], [5.3, y0, -span + 0.15], r0, 0.25)
    l0.support_log1 = -1
    l0.support_log2 = -1

    r1 = 1.18
    y1 = ground_y + r1
    l1 = SegmentedCylinder3D(2, [-5.2, y1,  span + 0.20], [4.6, y1,  span - 0.10], [-5.2, y1,  span + 0.20], [4.6, y1,  span - 0.10], r1, 0.25)
    l1.support_log1 = -1
    l1.support_log2 = -1

    r2 = 1.10
    y2 = ground_y + max(r0, r1) + r2 - 0.15
    y2_coll = ground_y + r2
    l2 = SegmentedCylinder3D(3, [-span - 0.20, y2, -5.1], [-span + 0.15, y2, 4.7], [-span - 0.10, y2_coll, -4.6], [-span + 0.10, y2_coll, 4.3], r2, 0.35)
    l2.support_log1 = 0
    l2.support_log2 = 1

    r3 = 1.26
    y3 = ground_y + max(r0, r1) + r3 - 0.15
    y3_coll = ground_y + r3
    l3 = SegmentedCylinder3D(4, [ span - 0.10, y3, -4.6], [ span + 0.25, y3, 5.3], [ span - 0.05, y3_coll, -4.2], [ span + 0.15, y3_coll, 4.8], r3, 0.35)
    l3.support_log1 = 0
    l3.support_log2 = 1

    r4 = 0.92
    y4 = y2 + r2 + r4 - 0.15
    y4_coll = ground_y + r4 + 0.35
    l4 = SegmentedCylinder3D(5, [-3.8, y4, -3.2], [4.2, y4, 2.7], [-2.2, y4_coll, -1.6], [2.4, y4_coll, 1.4], r4, 0.50)
    l4.support_log1 = 2
    l4.support_log2 = 3

    r5 = 1.04
    y5 = y3 + r3 + r5 - 0.15
    y5_coll = ground_y + r5 + 0.30
    l5 = SegmentedCylinder3D(6, [-4.3, y5,  2.8], [3.9, y5, -3.1], [-2.4, y5_coll,  1.5], [2.2, y5_coll, -1.6], r5, 0.50)
    l5.support_log1 = 2
    l5.support_log2 = 3

    return [l0, l1, l2, l3, l4, l5]

def get_stack_teepee():
    ground_y = -4.2
    base_r = 4.2
    apex_r = 0.65
    apex_y = 2.4
    radii = [1.08, 1.25, 1.14, 1.22, 1.05]
    angle_offsets = [0.30, 1.58, 2.75, 4.02, 5.35]
    logs = []
    for i in range(5):
        angle = angle_offsets[i]
        r = radii[i]
        p1 = [(base_r + 0.2 * math.sin(i * 3.1)) * math.cos(angle), ground_y + r, (base_r + 0.2 * math.cos(i * 2.7)) * math.sin(angle)]
        p2 = [apex_r * math.cos(angle) + 0.08 * math.sin(i * 1.5), apex_y + 0.12 * math.cos(i * 2.0), apex_r * math.sin(angle)]
        p2_coll = [apex_r * 0.35 * math.cos(angle), ground_y + r + 0.35, apex_r * 0.35 * math.sin(angle)]
        log = SegmentedCylinder3D(i + 1, p1, p2, p1, p2_coll, r, 0.45)
        log.support_log1 = -1
        log.support_log2 = -1
        logs.append(log)
    return logs

def get_stack_pyramid():
    ground_y = -4.2
    r_base0 = 1.38
    r_base1 = 1.26
    y1_0 = ground_y + r_base0
    y1_1 = ground_y + r_base1

    l0 = SegmentedCylinder3D(1, [-5.4, y1_0, -2.4], [5.6, y1_0, -2.6], [-5.4, y1_0, -2.4], [5.6, y1_0, -2.6], r_base0, 0.25)
    l0.support_log1 = -1
    l0.support_log2 = -1

    l1 = SegmentedCylinder3D(2, [-5.6, y1_1,  2.6], [5.2, y1_1,  2.4], [-5.6, y1_1,  2.6], [5.2, y1_1,  2.4], r_base1, 0.25)
    l1.support_log1 = -1
    l1.support_log2 = -1

    y_apex = 2.2
    r_cross2, r_cross3, r_cross4 = 1.08, 1.15, 1.12
    y_coll2 = ground_y + r_cross2 + 0.2
    l2 = SegmentedCylinder3D(3, [-4.2, y1_0 + r_base0 - 0.2, -1.1], [-0.1, y_apex + 0.1, -0.2], [-3.8, y_coll2, -0.6], [-0.1, y_coll2, -0.1], r_cross2, 0.50)
    l2.support_log1 = 0
    l2.support_log2 = -1

    y_coll3 = ground_y + r_cross3 + 0.2
    l3 = SegmentedCylinder3D(4, [ 4.3, y1_0 + r_base0 - 0.2, -0.9], [ 0.1, y_apex, -0.3], [ 3.9, y_coll3, -0.6], [ 0.1, y_coll3, -0.1], r_cross3, 0.50)
    l3.support_log1 = 0
    l3.support_log2 = -1

    y_coll4 = ground_y + r_cross4 + 0.2
    l4 = SegmentedCylinder3D(5, [ 0.2, y1_1 + r_base1 - 0.2,  3.1], [ 0.0, y_apex + 0.15, 0.4], [ 0.1, y_coll4,  2.3], [ 0.0, y_coll4,  0.2], r_cross4, 0.50)
    l4.support_log1 = 1
    l4.support_log2 = -1

    return [l0, l1, l2, l3, l4]

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
    ash_bed = AshBed3D()

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
                ash_bed = AshBed3D()
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
                ash_bed = AshBed3D()
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

            # -------------------------------------------------------------
            # HETEROGENEOUS PIECE-BY-PIECE COMBUSTION WITH RADIAL CORE DRAFT
            # -------------------------------------------------------------
            for l in logs:
                for s in range(NUM_LOG_SEGS):
                    t_val = (s + 0.5) / float(NUM_LOG_SEGS)
                    seg_p = [l.p1[k] + t_val * l.axis[k] for k in range(3)]

                    r_seg = math.sqrt(seg_p[0]*seg_p[0] + seg_p[2]*seg_p[2])
                    r_norm = r_seg / 3.6
                    eta_r = max(0.0, 1.0 - r_norm * r_norm)

                    # 1. Initial kindling nest heat (concentrated in center core)
                    if kindle_heat > 0.05:
                        d_k = math.sqrt(sum((seg_p[k] - kindle_pos[k])**2 for k in range(3)))
                        d_surf = max(0.0, d_k - l.radius - 1.5)
                        if d_surf < 2.5:
                            l.temp[s] += 0.018 * (1.0 - d_surf / 2.5) * kindle_heat * eta_r * time_scale

                    # 2. Ash bed radiant ember heat
                    if ash_bed.heat > 0.15 and seg_p[1] < 0.2:
                        bed_dy = max(0.0, seg_p[1] - ash_bed.center[1])
                        if bed_dy < 3.2:
                            bed_fac = (1.0 - bed_dy / 3.2) * eta_r
                            l.temp[s] += 0.005 * ash_bed.heat * bed_fac * time_scale

                    # 3. Cross-log fire radiation from actively burning segments within draft core
                    for other in logs:
                        if other is l:
                            continue
                        for sj in range(NUM_LOG_SEGS):
                            if other.temp[sj] > 0.38 and other.structural_mass[sj] > 0.10:
                                tj = (sj + 0.5) / float(NUM_LOG_SEGS)
                                pj = [other.p1[k] + tj * other.axis[k] for k in range(3)]
                                d_cross = math.sqrt(sum((seg_p[k] - pj[k])**2 for k in range(3)))
                                d_cross_surf = max(0.0, d_cross - l.radius - other.radius)
                                if d_cross_surf < 1.3:
                                    rad_power = (1.0 - d_cross_surf / 1.3) * (0.25 + 0.75 * eta_r)
                                    l.temp[s] += 0.004 * rad_power * time_scale

                    # 4. Conservative 1D thermal diffusion along wood grain
                    cur = l.temp[s]
                    prev_t = l.temp[s-1] if s > 0 else cur
                    next_t = l.temp[s+1] if s < NUM_LOG_SEGS - 1 else cur
                    laplacian = prev_t - 2.0 * cur + next_t
                    l.temp[s] += 0.0016 * laplacian * time_scale

                    # 5. Ambient convective cooling (outer ends exposed to cold air cool down rapidly)
                    cool_factor = 1.0 - 0.75 * eta_r
                    l.temp[s] -= 0.0022 * cur * cool_factor * time_scale

                    # 6. Active combustion & calcification: sustained only if hot and fueled by draft
                    if l.temp[s] > 0.35:
                        if eta_r > 0.05:
                            l.temp[s] = min(1.0, l.temp[s] + 0.0035 * eta_r * time_scale)
                            burn_rate = 0.00042 * (l.temp[s] - 0.30) * (0.20 + 0.80 * eta_r)
                            l.burn_progress[s] += burn_rate * time_scale
                        else:
                            l.burn_progress[s] += 0.00008 * time_scale

                        l.structural_mass[s] = max(0.0, 1.0 - l.burn_progress[s] * 1.25)

                        if l.burn_progress[s] > 0.60 and random.random() < 0.04:
                            ang = random.random() * 6.28318
                            rad_dir = [
                                l.tangent[k] * math.cos(ang) + l.bitangent[k] * math.sin(ang)
                                for k in range(3)
                            ]
                            ash_p = [seg_p[k] + rad_dir[k] * l.radius * 0.95 for k in range(3)]
                            vel = [(random.random() - 0.5) * 0.3 + wind * 0.3, -(random.random() * 0.25 + 0.15), (random.random() - 0.5) * 0.3]
                            ash_flakes.append([ash_p[0], ash_p[1], ash_p[2], vel[0], vel[1], vel[2], PALETTE_ASH[random.choice([2, 3])]])

                    if l.burn_progress[s] > 0.85:
                        l.temp[s] = max(0.12, l.temp[s] - 0.0012 * time_scale)
                    if l.temp[s] < 0.0:
                        l.temp[s] = 0.0

            # -------------------------------------------------------------
            # ASYNCHRONOUS SEGMENT-DRIVEN COLLAPSE PHYSICS
            # -------------------------------------------------------------
            total_mass = sum(l.structural_mass[s] for l in logs for s in range(NUM_LOG_SEGS))
            total_segs = len(logs) * NUM_LOG_SEGS
            active_burning = sum(1 for l in logs for s in range(NUM_LOG_SEGS) if l.temp[s] > 0.35)
            avg_mass = total_mass / float(total_segs)
            burnt_mass = 1.0 - avg_mass

            ash_bed.height = min(1.4, 0.35 + burnt_mass * 0.85 + ash_bed.volume)
            if active_burning > 0:
                ash_bed.heat = min(1.0, ash_bed.heat + 0.003 * time_scale)
            elif sim_time > 25.0:
                ash_bed.heat = max(0.12, ash_bed.heat - 0.0003 * time_scale)

            # Individual collapse calculation per log based on supporting contacts & core mass
            for i, l in enumerate(logs):
                support_integrity = 1.0
                if 0 <= l.support_log1 < len(logs):
                    s1 = logs[l.support_log1]
                    s1_m = (s1.structural_mass[3] + s1.structural_mass[8]) * 0.5
                    s1_eff = s1_m * (1.0 - s1.collapse_cur * 0.65)
                    support_integrity = min(support_integrity, s1_eff)
                if 0 <= l.support_log2 < len(logs):
                    s2 = logs[l.support_log2]
                    s2_m = (s2.structural_mass[3] + s2.structural_mass[8]) * 0.5
                    s2_eff = s2_m * (1.0 - s2.collapse_cur * 0.65)
                    support_integrity = min(support_integrity, s2_eff)

                own_center_mass = (l.structural_mass[4] + l.structural_mass[5] + l.structural_mass[6]) / 3.0
                log_integrity = min(support_integrity, own_center_mass)

                target_c = 0.0
                if force_collapse:
                    target_c = 1.0
                elif log_integrity < 0.45:
                    target_c = min(1.0, (0.45 - log_integrity) / 0.45)

                if target_c > l.collapse_cur:
                    l.collapse_speed += 0.0020 * time_scale
                    l.collapse_cur += l.collapse_speed
                    if l.collapse_cur >= target_c:
                        l.collapse_cur = target_c
                        l.collapse_speed = 0.0

                c = l.collapse_cur
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
            # FIRE INJECTION: Strictly wood segments in central core & kindling
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

            # Burning wood segments inside core draft chimney
            for l in logs:
                for s in range(NUM_LOG_SEGS):
                    if l.temp[s] > 0.38 and l.burn_progress[s] < 0.90:
                        t_val = (s + 0.5) / float(NUM_LOG_SEGS)
                        p = [l.p1[k] + t_val * l.axis[k] for k in range(3)]

                        r_seg = math.sqrt(p[0]*p[0] + p[2]*p[2])
                        r_norm = r_seg / 3.4
                        eta_r = max(0.0, 1.0 - r_norm * r_norm)

                        if eta_r <= 0.08:
                            continue

                        px, py, pz = project_pt(p)
                        flame_h = l.temp[s] * (0.25 + 0.75 * eta_r) * 1.05

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
                f[0] += f[3] * 0.04 + math.sin(f[1] * 1.5 + sim_time) * 0.015
                f[1] += f[4] * 0.04
                f[2] += f[5] * 0.04
                r_sq = f[0]*f[0] + f[2]*f[2]
                r_bed = ash_bed.radius_xz
                bed_h = (ash_bed.height * math.sqrt(max(0.0, 1.0 - r_sq / (r_bed * r_bed)))) if r_sq < r_bed * r_bed else 0.0
                floor_y = -4.2 + bed_h
                if f[1] <= floor_y:
                    if r_sq < r_bed * r_bed:
                        ash_bed.volume += 0.002
                        ash_bed.height = min(1.4, 0.35 + ash_bed.volume)
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

                # Test ash bed
                res = ash_bed.intersect(ro, rd)
                if res and res[0] < closest_t:
                    closest_t = res[0]
                    hit_data = (res, ash_bed)
                    hit_type = 'ash_bed'

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
                    s_idx = min(5, max(1, int((s_val + rock_noise + st.shade_var) * 2.8)))
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

                        u_frac = (u * num_plates_u) - u_plate
                        v_frac = (v * num_plates_v) - v_plate
                        is_furrow = (u_frac < 0.12 or u_frac > 0.88 or (v_frac < 0.08 and (u_plate % 2 == 0)))

                        if burn > 0.70:
                            if is_furrow:
                                if seg_temp > 0.30:
                                    emb_idx = min(3, max(0, int(seg_temp * 3.5)))
                                    shade_buf[y][x] = PALETTE_EMBERS[emb_idx]
                                else:
                                    shade_buf[y][x] = PALETTE_CHARRED[0]
                            else:
                                up_factor = norm[1]
                                if up_factor > 0.25:
                                    ash_idx = min(4, max(2, int(lval * 2.0 + 1.8)))
                                    shade_buf[y][x] = PALETTE_ASH[ash_idx]
                                elif up_factor > -0.1:
                                    ash_idx = min(3, max(1, int(lval * 1.8 + 0.9)))
                                    shade_buf[y][x] = PALETTE_ASH[ash_idx]
                                else:
                                    c_idx = min(2, max(0, int(lval * 1.8)))
                                    shade_buf[y][x] = PALETTE_CHARRED[c_idx]
                        elif burn > 0.40:
                            if is_furrow and seg_temp > 0.40:
                                emb_idx = min(3, max(0, int(seg_temp * 3.5)))
                                shade_buf[y][x] = PALETTE_EMBERS[emb_idx]
                            else:
                                if burn > 0.55 and norm[1] > 0.45:
                                    shade_buf[y][x] = PALETTE_ASH[1]
                                else:
                                    c_idx = min(4, max(0, int(lval * 2.0)))
                                    shade_buf[y][x] = PALETTE_CHARRED[c_idx]
                        elif burn > 0.15:
                            if is_furrow:
                                shade_buf[y][x] = PALETTE_CHARRED[0]
                            else:
                                b_idx = min(4, max(1, int(lval * 2.0)))
                                shade_buf[y][x] = PALETTE_WOOD[b_idx]
                        else:
                            if is_furrow:
                                shade_buf[y][x] = PALETTE_WOOD[0]
                            else:
                                b_idx = min(6, max(1, int(lval * 2.8)))
                                shade_buf[y][x] = PALETTE_WOOD[b_idx]

                elif hit_type == 'ash_bed':
                    res, ab = hit_data
                    t, pt, norm = res
                    id_buf[y][x] = 500
                    depth_buf[y][x] = (pt[0]-cam_pos[0])*fwd[0] + (pt[1]-cam_pos[1])*fwd[1] + (pt[2]-cam_pos[2])*fwd[2]

                    l_vec = [light_pos[k] - pt[k] for k in range(3)]
                    ldist = math.sqrt(sum(k*k for k in l_vec)) + 1e-6
                    ldir = [k / ldist for k in l_vec]
                    atten = 1.0 / (1.0 + 0.08 * ldist + 0.015 * ldist * ldist)
                    ndotl = max(0.0, sum(norm[k] * ldir[k] for k in range(3)))
                    ambient = 0.24 + 0.12 * max(0.0, norm[1])
                    s_val = ndotl * atten * light_intensity * 2.2 + ambient

                    r_core = math.sqrt(pt[0]*pt[0] + pt[2]*pt[2])
                    core_heat = ab.heat * max(0.0, 1.0 - r_core / 4.8)

                    f1 = math.sin(pt[0] * 1.3 + pt[2] * 0.7)
                    f2 = math.cos(pt[2] * 1.4 - pt[0] * 0.6)
                    fissure = abs(f1 * f2)

                    if fissure < 0.18 and core_heat > 0.22:
                        emb_idx = min(3, max(0, int(core_heat * 3.8)))
                        shade_buf[y][x] = PALETTE_EMBERS[emb_idx]
                    else:
                        if r_core < 1.8 and core_heat > 0.35:
                            ash_idx = min(2, max(1, int(s_val * 1.6)))
                        elif r_core < 3.8:
                            ash_idx = min(3, max(2, int(s_val * 2.2)))
                        else:
                            ash_idx = min(4, max(2, int(s_val * 2.6)))
                        shade_buf[y][x] = PALETTE_ASH[ash_idx]

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
                    frame_col[y][x] = PALETTE_ASH[0] if cid == 500 else (18, 14, 12)
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
