# fireplace-experiment

Real-time physical bonfire simulation rendered as 2D pixel art over 3D geometric models in the terminal.

Uses a per-object pixel art shader pipeline with object-space snapping to eliminate pixel creep, stepped cel-shading, 1-pixel discontinuity outlines, and Unicode half-block characters (`▀`) for square pixel aspect ratio at 60 FPS with terminal transparency support.

![3D Pixel Art Bonfire Stacking Modes](docs/media/stacks_c_engine.png)

## Key Techniques

- **Physical Stacking Modes & Contact Physics**: Supports three classic bonfire geometries (Log Cabin / Cribbing, Conical Teepee, and Cross Pyramid) with grounded base logs, notched tier resting heights, and gravity collapse kinematics.
- **Discrete Object-Space Bark Plates**: Wood bark is partitioned into discrete cellular plates with dark crevice furrows. Eliminates periodic trigonometric stripes while preventing pixel creep during camera orbit.
- **Concentric Tree Rings**: End-caps are rendered with circular growth rings (bark rim, sapwood, heartwood, and pith core).
- **Localized Charring & Crevice Embers**: Surfaces exposed to the fire core develop charred charcoal crusts and incandescent ember cracks only where combustion actively occurs.
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
