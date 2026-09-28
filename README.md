# fireplace-experiment

Real-time physical bonfire simulation rendered as 2D pixel art over 3D geometric models in the terminal.

Uses a per-object pixel art shader pipeline with object-space snapping to eliminate pixel creep, stepped cel-shading, 1-pixel discontinuity outlines, and Unicode half-block characters (`▀`) for square pixel aspect ratio at 60 FPS with terminal transparency support.

![3D Pixel Art Bonfire with Stone Ring and Combustion Lifecycle](docs/media/stone_ring_c_stages.png)

## Key Techniques

- **3D Stone Fire Ring Containment**: Hearth base ring composed of rounded granite/basalt stones (`Stone3D`) resting at ground level ($Y = -4.2$), receiving dynamic firelight and ash settling.
- **Segmented Firewood Combustion Lifecycle**: Each log is discretized into $N_{\text{seg}} = 10$ axial segments tracking independent heat conduction, flame ignition, charred alligator cracking, brittle ash degradation, and disintegration.
- **Zero Floor Fire Spawning**: Flames and heat emit strictly from the central kindling bundle and ignited wood segments ($T > 0.35$). The dirt floor no longer emits fire.
- **Mass-Loss-Driven Self-Collapse**: As wood burns and structural mass decays, logs sag, lose support, and naturally collapse inward into the central ash bed under gravity.
- **Physical Stacking Modes & Contact Physics**: Supports three classic bonfire geometries (Log Cabin / Cribbing, Conical Teepee, and Cross Pyramid) with grounded base logs, notched tier resting heights, and gravity collapse kinematics.
- **Discrete Object-Space Bark Plates**: Wood bark is partitioned into discrete cellular plates with dark crevice furrows. Eliminates periodic trigonometric stripes while preventing pixel creep during camera orbit.
- **Concentric Tree Rings**: End-caps are rendered with circular growth rings (bark rim, sapwood, heartwood, and pith core).
- **1-Pixel Outlines**: Screen-space edge detector over the object ID and depth buffers paints dark contour pixels at silhouette boundaries without anti-aliasing blur.
- **Volumetric Fire Interleaving**: Convective flame grid tested against the log depth buffer, allowing fire to wrap naturally both in front of and behind the wood.
- **Falling Sand Ash Physics**: Burnt wood peels off as physical ash flakes that fall with gravity, roll over sloped log contours, and pile up at the hearth base.
- **3D Camera Orbit & Turntable**: Real-time yaw and pitch camera orbit with arbitrary view matrix raycasting, allowing inspection of the 3D log stack and fire volume from any angle.
- **ANSI Half-Block Output**: Encodes two vertical pixels per character cell (`\033[38;2;...m\033[48;2;...m▀`) with full native terminal background transparency.

## Building & Running

### C Engine (Recommended)

Requires GCC or Clang with POSIX terminal support:

```bash
make
./fireplace
```

### Python Prototype

Requires Python 3.8+:

```bash
python3 bonfire.py
```

## Controls

| Key | Action |
| --- | --- |
| `m` | Cycle wood stacking mode (Log Cabin / Teepee / Pyramid) |
| `←` / `→` or `a` / `d` or `h` / `l` | Orbit camera left / right (yaw) |
| `↑` / `↓` or `w` / `s` or `k` / `j` | Orbit camera up / down (pitch) |
| `t` | Toggle auto-turntable 360° rotation |
| `0` or `z` | Reset camera angle to default |
| `r` | Rekindle / restart bonfire lifecycle |
| `c` | Trigger immediate structural log collapse |
| `+` / `-` | Increase / decrease simulation speed |
| `Space` | Pause / resume simulation |
| `q` | Quit and restore terminal |

## Documentation

See [docs/architecture.md](docs/architecture.md) for detailed notes on the 3D pixel art shader math, combustion stages, and experiment log.
