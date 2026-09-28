# Technical Architecture & Experiment Notes

This document records the design decisions, mathematical formulations, and iterative discoveries made during the development of the terminal 3D pixel art bonfire engine.

---

## 1. The 3D-to-Pixel-Art Pipeline

Naive approaches to 3D pixel art usually apply a full-screen downsampling post-process filter. This produces three critical artifacts:
1. **Subpixel Jitter & Pixel Creep**: Edges and surface details swim across the screen as objects or camera move fractionally.
2. **Inconsistent Texel Density**: Distant objects lose character resolution while foreground objects display coarse pixels.
3. **Loss of Silhouette Definition**: High-contrast edges blend into neighbor pixels without clean artist-drawn outlines.

To solve this, the engine implements a per-object shader pipeline:

```
[3D Mesh / Cylinder Definition]
          │
          ▼
[Raycasting & Depth Pass] ───────► Object ID Buffer & Depth Buffer
          │
          ▼
[Object-Space UV Snapping] ──────► Anti-Pixel Creep (texels rigidly bound to mesh)
          │
          ▼
[Stepped Cel-Shading Ramp] ──────► Quantized point-light from fire core
          │
          ▼
[1-Pixel Discontinuity Filter] ──► Sharp silhouette outline
          │
          ▼
[3D Fire & Depth Interleaving] ──► Flames wrap before/behind logs
          │
          ▼
[Half-Block ANSI Presenter] ─────► 2 vertical pixels per cell (`▀`)
```

---

## 2. Object-Space Texel Snapping (Anti-Pixel Creep)

### Problem
When sampling surface patterns in continuous screen space, fractional movements of the model cause texel boundaries to shift between raster pixels, creating a flickering/sliding effect.

### Solution
Coordinates are computed in the local coordinate frame of each 3D cylinder:
- Cylinder axis $\vec{A} = \vec{P}_2 - \vec{P}_1$, length $L = \|\vec{A}\|$, direction $\hat{D} = \vec{A} / L$.
- Reference up vector $\hat{R}$ defines orthogonal tangents:
  $$\hat{T} = \frac{\hat{D} \times \hat{R}}{\|\hat{D} \times \hat{R}\|}, \quad \hat{B} = \hat{D} \times \hat{T}$$
- For any intersection point $\vec{P}_{\text{hit}}$ with surface normal $\hat{N}$:
  $$v = \frac{(\vec{P}_{\text{hit}} - \vec{P}_1) \cdot \hat{D}}{L}, \quad u = \frac{\text{atan2}(\hat{N} \cdot \hat{B}, \hat{N} \cdot \hat{T}) + \pi}{2\pi}$$
- Texture coordinates are then snapped to fixed discrete cells:
  $$u_{\text{snap}} = \frac{\lfloor u \cdot N_{\text{radial}} \rfloor}{N_{\text{radial}}}, \quad v_{\text{snap}} = \frac{\lfloor v \cdot N_{\text{longitudinal}} \rfloor}{N_{\text{longitudinal}}}$$

Because $(u_{\text{snap}}, v_{\text{snap}})$ are defined in the object's local frame, moving, tilting, or collapsing the log rotates the pixel grid rigidly with the geometry.

---

## 3. Curated Stepped Palettes & Cel-Shading

Continuous lighting ramps (Phong/Blinn-Phong) break pixel art aesthetics. The engine uses discrete banded shading:

### Light Calculation
From the fire's 3D point light $\vec{P}_{\text{light}}$ with intensity $I$:
$$\vec{L} = \vec{P}_{\text{light}} - \vec{P}_{\text{hit}}, \quad d = \|\vec{L}\|, \quad \hat{L} = \frac{\vec{L}}{d}$$
$$\text{Atten} = \frac{1}{1 + 0.10 d + 0.02 d^2}$$
$$I_{\text{diffuse}} = \max(0, \hat{N} \cdot \hat{L}) \times \text{Atten} \times I_{\text{fire}} + I_{\text{ambient}}$$

### Color Ramp Ramps
The scalar $I_{\text{diffuse}}$ is quantized into discrete bands:

- **Oak Wood**:
  - `Band 0 (Outline)`: `#180C06`
  - `Band 1 (Shadow Bark)`: `#341C10`
  - `Band 2 (Dark Oak)`: `#58301C`
  - `Band 3 (Mid Grain)`: `#844826`
  - `Band 4 (Heartwood)`: `#B06636`
  - `Band 5 (Firelit Timber)`: `#DC8A48`
  - `Band 6 (Rim Highlight)`: `#FCB260`

- **Cut End-Caps**:
  - Concentric tree rings: $r_{\text{fraction}} = \|\vec{P}_{\text{hit}} - \vec{P}_{\text{cap}}\| / R$.
  - Quantized rings: $\lfloor r_{\text{fraction}} \times 7 \rfloor \pmod 2$.

- **Embers & Fire**:
  - Deep Red `#8C1205` $\to$ Bright Red `#E12D0A` $\to$ Hot Orange `#FF6E12` $\to$ Yellow `#FFC830` $\to$ White Core `#FFFFCD`.

- **Ash & Charcoal**:
  - Charcoal `#302D34` $\to$ Dark Ash `#58545E` $\to$ Mid Grey `#87848E` $\to$ Chalky Light `#B9B6C0` $\to$ White Powder `#E6E4EB`.

---

## 4. 1-Pixel Edge Detection (Outlines)

To create crisp hand-drawn pixel art outlines, a neighborhood discontinuity test is performed on the G-buffer:
$$\Delta_{\text{ID}} = \text{ID}(x, y) \neq \text{ID}(x \pm 1, y \pm 1)$$
$$\Delta_{\text{depth}} = |\text{Depth}(x, y) - \text{Depth}(x \pm 1, y \pm 1)| > \epsilon$$

If either condition is met, the pixel is drawn using the darkest material tone (`#180C06`), yielding a strictly 1-pixel-thick border without anti-aliasing blur.

---

## 5. 7-Stage Combustion Lifecycle

1. **Stage 1 (0-12s)**: Kindling ignition. Tiny flame in central cradle between Log 1 and Log 2.
2. **Stage 2 (12-35s)**: Flame catches onto the logs. Twin convective spires begin to form.
3. **Stage 3 (35-80s)**: Roaring bonfire at peak heat. Tallest right spire reaches apex, left spire licks upright branch, sparks launch into updraft.
4. **Stage 4 (80-120s)**: Wood surface ashens. Loose chalky ash flakes detach under heat and drift downward.
5. **Stage 5 (120-150s)**: Structural collapse. Core wood degrades, 3D logs lose structural support and sag/fall toward the hearth base.
6. **Stage 6 (150-195s)**: Glowing ember bed. Open flames die down; logs rest as glowing charcoal heaps.
7. **Stage 7 (195s+)**: Cold ash mound. Heat dissipates into residual white and grey ash pile.

---

## 6. Experiment Discoveries & Fixes Log

| Issue | Root Cause | Solution |
| --- | --- | --- |
| Fire losing contrast on transparent terminal | Native black/blue background was stripped completely | Preserved terminal background transparency when `is_sky == true`, while logs and fire retain fully opaque color blocks. |
| ASCII glyphs looking like alphabet soup | Character glyphs (`L, J, F, %, 3, K`) degraded pixel fidelity | Switched to Unicode half-block characters (`▀`, `▄`), doubling vertical resolution and providing true square pixels. |
| Wood logs completely invisible | Comma operator typo in C outline pass: `g_id_buf[y, x][0]` evaluated to `g_id_buf[x][0] == 0` | Fixed to `g_id_buf[y][x]`. Wood now renders with full cel-shading, outlines, and tree rings. |
| Fire drawn strictly behind logs | Fire simulation lacked 3D depth testing | Integrated fire Z coordinate testing against log depth buffer, allowing flames to wrap organically around logs. |
