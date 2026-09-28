# fireplace-experiment

Real-time physical bonfire simulation rendered as 2D pixel art over 3D geometric models in the terminal.

Uses a per-object pixel art shader pipeline with object-space snapping to eliminate pixel creep, stepped cel-shading, 1-pixel discontinuity outlines, and Unicode half-block characters (`▀`) for square pixel aspect ratio at 60 FPS with terminal transparency support.

![3D Pixel Art Bonfire Combustion and Natural Hearth Stages](docs/media/natural_stone_segmented_stages.png)

## Key Techniques

- **Natural Interlocking Stone Ring (`Stone3D`)**: 16 anisotropic fieldstones modeled as oriented ellipsoids with local tangent, vertical, and radial frames. Features flattened heights, irregular spacing, and interlocking perimeter contact forming an authentic continuous hearth wall.
- **Irregular Hand-Cut Firewood Stacks**: Randomized variations in log diameter ($R \in [0.92, 1.38]$), asymmetric lengths, and slight angular tilts across all 3 stacking modes (Log Cabin, Teepee, and Pyramid).
- **Central Core Draft Combustion ($\eta(r)$)**: Simulates natural chimney convection. Flames and intense combustion are concentrated in the center core ($r < 3.2$), while outer log ends stick out intact with raw oak bark and ambient convective cooling.
- **Asynchronous, Segment-Driven Collapse Kinematics**: Structural contact graph where logs collapse individually when their specific support points and central segments lose structural mass, rather than collapsing simultaneously.
- **Piece-by-Piece Heterogeneous Lifecycle**: Conservative 1D thermal diffusion along wood grain prevents instantaneous flashover. Segments transition independently: Raw Oak $\to$ Scorched Soot $\to$ Alligator Charred $\to$ Calcified White Ash $\to$ Disintegration.
- **3D Hearth Ash Bed (`AshBed3D`)**: Analytical 3D dome mound that grows dynamically inside the stone ring as wood burns, with glowing cellular fissure veins peeking through.
- **Gravity-Settled Wood Ash Mantle**: Ash accumulates naturally on upward-facing log surfaces ($N_y > 0$), while vertical flanks and undersides retain charred carbon crusts.
- **3D Camera Orbit & Turntable**: Real-time yaw and pitch camera orbit with arbitrary view matrix raycasting from any angle.
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
