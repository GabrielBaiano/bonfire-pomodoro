# fireplace-experiment

A real-time physical bonfire Pomodoro timer rendered as 3D pixel art in the terminal.

Focus while the campfire burns. Each session ignites kindling, transitions into a roaring teepee fire, snaps and settles into glowing ember coals, and slowly extinguishes when your focus time ends. Completed sessions are logged to keep track of your focus history.

![Campfire Turntable](docs/media/fireplace_turntable.gif)

## How It Works

- **Pomodoro Cycle**: By default, a session runs for 50 minutes (standard natural firewood burn). You can set custom durations like `--time 25` for classic Pomodoro sessions.
- **Dynamic Physical Combustion**: Logs undergo 1D thermal diffusion along the wood grain, dry out moisture, char, fracture at weakened stress points, and fall with ragdoll gravity into the coal bed.
- **Randomized Wood Species**: Randomizes between Carvalho (Oak), Pinho (Pine), Bétula (Birch), and Cerejeira (Cherry), each with unique palettes, crackle frequency, and burn properties.
- **Focus History**: Finished sessions are automatically logged to `~/.bonfire_history.json`.
- **ANSI Half-Block Pixel Art**: 24-bit TrueColor characters (`▀`) rendered at 60 FPS with native terminal background transparency.

## Usage

### Quick Start

```bash
make
./fireplace
```

### CLI Options

```bash
# Start a 25-minute Pomodoro session
./fireplace --time 25

# Dark Souls Coiled Sword Bonfire mode
./fireplace --souls --time 25

# Specify a wood species (carvalho, pinho, betula, cerejeira)
./fireplace --wood pinho --time 45

# View past completed focus sessions
./fireplace --history

# Fast preview mode (30x speed)
./fireplace --fast
```

![Dark Souls Bonfire](docs/media/dark_souls_bonfire.gif)

## Controls

| Key | Action |
| --- | --- |
| `[E]` / `Space` | Kindle bonfire (when unlit/extinguished) or stoke sparks |
| `[P]` | Pause / resume current Pomodoro phase |
| `[S]` | Skip phase (Focus → Rest → Next Focus) |
| `[M]` | Mute / unmute audio |
| `[` / `]` or `-` / `+` | Decrease / increase sound volume (10% steps) |
| `←` / `→` or `a` / `d` or `h` / `l` | Orbit camera yaw (rotate left/right) |
| `↑` / `↓` or `w` / `s` or `k` / `j` | Orbit camera pitch (tilt up/down) |
| `Space` or `g` or `t` | Toggle automatic 360° turntable rotation (in classic mode) |
| `r` | Rekindle / restart session from unlit state |
| `q` | Quit session |

## Features & Simulation Details

- **Startup Setup Menu**: Interactive pre-launch TUI to pick Fireplace style, focus duration, short/long breaks, cycles, and volume. Settings auto-persist to `~/.fireplace_conf`.
- **Authentic Audio Engine**: Seamless raw PCM streaming via PipeWire/ALSA with zero frame drops. Includes authentic Dark Souls bonfire ignition & loop (filtered of background choir hum) and realistic wood crackle.
- **3D Stone Hearth & Coiled Sword**: Procedural helical geometry and skull bone mound rendered with per-object shaders.
- **Ragdoll Wood Physics**: Burning logs snap unevenly when central mass is exhausted and drop onto the hearth floor with physical impulse and bounce restitution.
- **Turntable & Snapshot Exporter**: Headless rendering to PPM/GIF for captures (`--snapshot` and `--turntable`).

## Documentation & Engineering Blog

- 📖 **[BLOG_POST.md](BLOG_POST.md)**: An in-depth technical write-up (in the style of [Simon Willison's weblog](https://simonwillison.net/)) breaking down the 3D raymarching math, G-buffer cel-shading outlines, cellular automata fire convection, ANSI half-block sub-pixels, and raw PCM streaming audio architecture.
- 📐 **[docs/architecture.md](docs/architecture.md)**: Deep technical notes on the raycast shader, combustion equations, and geometry pipeline.

