#!/usr/bin/env python3
"""
3D Per-Object Pixel Art Bonfire Simulation (Python version)
Features:
- Unicode half-blocks '▀' (2 vertical pixels per cell)
- 3D cylindrical logs with grounded bases (true gravity stack)
- Rotational collapse under gravity around grounded base pivots
- Fire emanates directly from burning wood surfaces and cradle (not the floor)
- Dynamic flickering wrap lighting bathing the logs in warm amber & golden tones
- 1-pixel cel-art outlines
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
    (24, 12, 6),      # 0: Outline
    (56, 26, 14),     # 1: Deep shadow bark
    (96, 46, 24),     # 2: Dark oak bark
    (145, 72, 36),    # 3: Mid grain / warm bark
    (195, 104, 50),   # 4: Heartwood
    (238, 142, 68),   # 5: Firelit timber
    (255, 185, 95)    # 6: Bright golden rim highlight
]

PALETTE_ENDCAP = [
    (24, 12, 6),      # 0: Outline
    (115, 58, 30),    # 1: Dark ring
    (168, 92, 48),    # 2: Mid ring
    (218, 134, 74),   # 3: Light ring
    (255, 180, 110)   # 4: Firelit cut face
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
    def __init__(self, obj_id, p1, p2, p2_collapsed, radius):
        self.obj_id = obj_id
        self.p1 = list(p1)
        self.p2 = list(p2)
        self.p1_orig = list(p1)
        self.p2_orig = list(p2)
        self.p2_collapsed = list(p2_collapsed)
        self.radius = radius
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

    ground_y = -4.2
    logs = [
        Cylinder3D(1, [-5.2, ground_y, 0.5], [0.8, 0.8, -0.3], [0.2, ground_y + 0.8, -0.2], 1.35),
        Cylinder3D(2, [5.6, ground_y, 0.3], [-0.5, 1.6, 0.2], [-0.2, ground_y + 0.9, 0.2], 1.30),
        Cylinder3D(3, [-8.8, ground_y, 1.1], [-4.0, ground_y + 0.4, 0.6], [-4.0, ground_y + 0.4, 0.6], 1.05),
        Cylinder3D(4, [-2.8, ground_y, 2.0], [-2.2, 4.4, 1.5], [-2.0, ground_y + 1.4, 1.2], 0.95),
    ]

    sim_time = 0.0
    time_scale = 1.0
    wind = 0.0
    wind_tgt = 0.0
    collapse = 0.0
    force_collapse = False
    paused = False

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

        r, _, _ = select.select([sys.stdin], [], [], 0)
        if r:
            ch = sys.stdin.read(1)
            if ch in ('q', 'Q'):
                cleanup()
            elif ch in ('r', 'R'):
                sim_time = 0.0
                collapse = 0.0
                force_collapse = False
                for l in logs:
                    l.p2 = list(l.p2_orig)
                    l.wood_health = 100.0
                    l.ash_amount = 0.0
                    l.recompute()
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

            # Rotational gravity collapse
            if (sim_time > 115.0 or force_collapse) and collapse < 1.0:
                collapse = min(1.0, collapse + 0.015 * time_scale)
                for l in logs:
                    l.p2[0] = l.p2_orig[0] * (1.0 - collapse) + l.p2_collapsed[0] * collapse
                    l.p2[1] = l.p2_orig[1] * (1.0 - collapse) + l.p2_collapsed[1] * collapse
                    l.p2[2] = l.p2_orig[2] * (1.0 - collapse) + l.p2_collapsed[2] * collapse
                    l.recompute()

            world_w, world_h = 22.0, 14.0
            cx = pw // 2
            left_spire_x = cx - int(pw * 0.11)
            right_spire_x = cx + int(pw * 0.09)

            # 1. Heat from burning wood contacts
            for l in logs:
                if intensity < 0.15: continue
                num_pts = int(l.length * 6)
                for s in range(num_pts):
                    t_val = s / float(num_pts)
                    px = l.p1[0] + t_val * l.axis[0]
                    py = l.p1[1] + t_val * l.axis[1]
                    pz = l.p1[2] + t_val * l.axis[2]
                    dist_c = math.sqrt(px*px + (py + 2.0)**2)
                    if dist_c < 4.5:
                        gx = int(((px / world_w) + 0.5) * pw)
                        gy = int(((0.5 - (py / world_h))) * ph)
                        if 0 <= gx < pw and 0 <= gy < ph:
                            wfire = (0.92 - (dist_c / 4.5) * 0.25) * intensity
                            fire_heat[gy][gx] = max(fire_heat[gy][gx], wfire)
                            fire_z[gy][gx] = pz - 0.25

            # 2. Glowing cradle ember bed
            cradle_y = int(ph * (0.74 + collapse * 0.08))
            for dy in range(-4, 5):
                for dx in range(-14, 15):
                    d = math.sqrt((dx * 0.75)**2 + (dy * 1.8)**2)
                    if d < 12.0:
                        h = (1.0 - (d / 12.0)**1.8) * intensity
                        gx, gy = cx + dx, cradle_y + dy
                        if 0 <= gx < pw and 0 <= gy < ph:
                            fire_heat[gy][gx] = max(fire_heat[gy][gx], h)
                            fire_z[gy][gx] = -0.4

            # 3. Convective flame propagation: ROARING TWIN SPIRES
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

                    if is_left: fire_z[y][x] = 0.25
                    elif is_right: fire_z[y][x] = -0.25
                    else: fire_z[y][x] = -0.45

            for y in range(2, cradle_y + 1):
                for x in range(pw):
                    fire_heat[y][x] = next_fire[y][x]

            # Sparks
            if intensity > 0.40 and random.random() < 0.20:
                sx = left_spire_x + (random.random() - 0.5) * 4.0
                sy = cradle_y * 0.35
                sparks.append([sx, sy, wind * 0.25, -0.7 - random.random() * 0.4, random.randint(18, 35), PALETTE_EMBERS[1]])
            if intensity > 0.50 and random.random() < 0.25:
                sx = right_spire_x + (random.random() - 0.5) * 5.0
                sy = cradle_y * 0.20
                sparks.append([sx, sy, wind * 0.30, -0.8 - random.random() * 0.4, random.randint(20, 40), PALETTE_EMBERS[2]])

            alive_sparks = []
            for s in sparks:
                s[0] += s[2]
                s[1] += s[3]
                s[4] -= 1
                if s[4] > 0 and 0 <= s[0] < pw and 0 <= s[1] < ph:
                    alive_sparks.append(s)
            sparks = alive_sparks

            # Flakes
            if intensity > 0.40 and random.random() < 0.15:
                ash_flakes.append([cx + (random.random() - 0.5) * 20.0, cradle_y * 0.6, (random.random() - 0.5) * 0.4, 0.4, PALETTE_ASH[2]])

            alive_flakes = []
            for f in ash_flakes:
                f[0] += f[2] + wind * 0.1
                f[1] += f[3]
                ix, iy = int(round(f[0])), int(round(f[1]))
                if 0 <= ix < pw and iy < ph:
                    if iy >= int(ph * 0.85) or settled_ash[iy][ix]:
                        settled_ash[min(ph - 1, iy)][ix] = 1
                    else:
                        alive_flakes.append(f)
            ash_flakes = alive_flakes

        # 3D Raycasting with Wrap Lighting
        id_buf = [[0 for _ in range(pw)] for _ in range(ph)]
        depth_buf = [[1e9 for _ in range(pw)] for _ in range(ph)]
        shade_buf = [[(0, 0, 0) for _ in range(pw)] for _ in range(ph)]
        frame_sky = [[True for _ in range(pw)] for _ in range(ph)]
        frame_col = [[(0, 0, 0) for _ in range(pw)] for _ in range(ph)]

        world_w, world_h = 22.0, 14.0
        rd = [0.0, 0.0, 1.0]
        
        flicker = 1.0 + 0.16 * math.sin(sim_time * 8.0) + 0.10 * math.cos(sim_time * 13.0)
        light_pos = [0.0, -1.8, -1.2]
        light_intensity = 1.8 * flicker

        for y in range(ph):
            wy = (((ph - 1 - y) / ph) - 0.5) * world_h
            for x in range(pw):
                wx = ((x / pw) - 0.5) * world_w
                ro = [wx, wy, -40.0]

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
                    depth_buf[y][x] = pt[2]

                    # Lighting
                    lx, ly, lz = light_pos[0]-pt[0], light_pos[1]-pt[1], light_pos[2]-pt[2]
                    ldist = math.sqrt(lx*lx + ly*ly + lz*lz) + 1e-6
                    inv_ld = 1.0 / ldist
                    ldir = [lx*inv_ld, ly*inv_ld, lz*inv_ld]
                    atten = 1.0 / (1.0 + 0.08*ldist + 0.015*ldist*ldist)
                    
                    # Wrap lighting
                    dot_raw = norm[0]*ldir[0] + norm[1]*ldir[1] + norm[2]*ldir[2]
                    ndotl = max(0.0, (dot_raw + 0.45) / 1.45)
                    ambient = 0.28 + 0.12 * max(0.0, norm[1])
                    lval = (ndotl * atten * light_intensity * 2.4 + ambient)

                    if is_cap:
                        ring_band = int(math.floor(rf * 6.0)) % 2
                        col_idx = min(4, 1 + ring_band + (1 if lval > 0.8 else 0))
                        shade_buf[y][x] = PALETTE_ENDCAP[col_idx]
                    else:
                        uq = math.floor(u * 22.0) / 22.0
                        vq = math.floor(v * hit_log.length * 3.8) / (hit_log.length * 3.8)
                        grain = math.sin(uq * 38.0 + math.sin(vq * 10.0) * 1.5) * 0.5 + 0.5
                        fissure = math.sin(uq * 18.0 + vq * 8.0) * 0.5 + 0.5
                        bmod = 0.88 + 0.32 * grain

                        if fissure > 0.72 and ldist < 6.0:
                            eidx = min(4, max(0, int(lval * 3.2)))
                            shade_buf[y][x] = PALETTE_EMBERS[eidx]
                        else:
                            bidx = min(6, max(1, int(lval * bmod * 3.3)))
                            shade_buf[y][x] = PALETTE_WOOD[bidx]

        # Outline
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

                frame_sky[y][x] = False
                frame_col[y][x] = PALETTE_WOOD[0] if is_edge else shade_buf[y][x]

        # Fire compositing
        for y in range(ph):
            for x in range(pw):
                heat = fire_heat[y][x]
                if heat > 0.08:
                    if heat > 0.82: fcol = PALETTE_EMBERS[4]
                    elif heat > 0.58: fcol = PALETTE_EMBERS[3]
                    elif heat > 0.35: fcol = PALETTE_EMBERS[2]
                    elif heat > 0.18: fcol = PALETTE_EMBERS[1]
                    else: fcol = PALETTE_EMBERS[0]

                    if id_buf[y][x] > 0:
                        if fire_z[y][x] < depth_buf[y][x] or heat > 0.65:
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

        out.append(f"\033[1;33m[3D Pixel Art Bonfire]\033[0m {sim_time:4.0f}s | Fase: \033[1;37m{stage_name}\033[0m | [r] Reiniciar | [c] Colapsar | [+/-] Vel | [q] Sair ")

        sys.stdout.write("".join(out))
        sys.stdout.flush()

        time.sleep(0.028)

if __name__ == "__main__":
    main()
