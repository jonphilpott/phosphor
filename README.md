# Phosphor

A Lua-scriptable real-time graphics engine for live performance and audiovisual work. Write a scene file, run it, edit it while it runs — changes appear in ~200 ms without restarting.

Built on SDL2 + OpenGL 3.3 Core + Lua 5.4. No runtime dependencies beyond what the build system fetches from your system package manager.

---

## Build

Requires: **CMake 3.20+**, **SDL2**, a C++17 compiler.

```bash
# macOS (Homebrew)
brew install cmake sdl2

cmake -B build -DCMAKE_PREFIX_PATH=$(brew --prefix)
cmake --build build
```

```bash
# Linux (apt)
sudo apt install cmake libsdl2-dev

cmake -B build
cmake --build build
```

The binary lands at `./build/phosphor`.

---

## Run

```bash
./build/phosphor -s scenes/test.lua
```

| Flag | Description |
|------|-------------|
| `-s <path>` | Scene file to load |
| `-d <n>` | Display index (0 = primary) |
| `-h` | Print help and exit |

**Keyboard:** `F` toggles fullscreen · `Esc` quits

---

## Writing a Scene

A scene is a plain Lua file. Three optional hooks are called by the engine:

```lua
local t = 0

function on_load()
    -- Called once after the file loads.
    -- Allocate canvases, load images, set initial shaders here.
    shader_set("scanlines")
end

function on_frame(dt)
    -- Called every frame. dt = seconds since last frame.
    t = t + dt
    clear(0, 0, 0, 1)
    set_color(0, 1, 0.4, 1)
    draw_circle(screen_width / 2, screen_height / 2, 60 + math.sin(t * 2) * 20)
end

function on_osc(addr, ...)
    -- Called once per incoming OSC message (port 9000).
    local args = {...}
    if addr == "/color" then set_color(args[1], args[2], args[3], 1) end
end
```

Built-in globals: `screen_width`, `screen_height` (updated on resize and fullscreen toggle).

**Hot reload:** save the file while Phosphor is running — the Lua VM reloads automatically. GPU state is untouched.

---

## API Overview

Full reference is in [`docs/index.html`](docs/index.html) — open it in any browser.

### Drawing

```lua
clear(r, g, b, a)
set_color(r, g, b, a)          -- fill colour for rect/circle
set_stroke(r, g, b, a)         -- stroke colour for lines/points
set_stroke_weight(w)
draw_rect(x, y, w, h)
draw_circle(cx, cy, r)
draw_line(x1, y1, x2, y2)
draw_point(x, y)
```

### Transform Stack

```lua
push()  pop()
translate(x, y)
rotate(radians)
scale(sx [, sy])
```

### Feedback

Blends the previous frame back over the current one — classic CRT phosphor trail effect.

```lua
draw_feedback(alpha [, scale [, angle]])
```

### Post-Process Shaders

```lua
shader_set("scanlines", "chromatic_ab")   -- replace pipeline
shader_add("name")                         -- append
shader_clear()
shader_set_uniform("u_chrom_amount", 0.004)
```

Built-in shaders: `scanlines`, `chromatic_ab`. Custom shaders go in `shaders/<name>.frag`.

### Noise

```lua
noise(x [, y [, z]])           -- Perlin noise → [-1, 1]
fbm(x, y [, octaves, ...])     -- fractal Brownian motion → [-1, 1]
```

### Waveforms

```lua
-- Value functions → [-1, 1], t in cycles
wave_sine(t)
wave_saw(t)
wave_square(t [, duty])
wave_tri(t)

-- Polyline renderer (respects transform stack)
draw_waveform(type, x, y, w, h [, cycles [, phase]])
```

### 3D Wireframe

```lua
camera_3d(ex, ey, ez, tx, ty, tz)
perspective_3d(fov [, near [, far]])

sx, sy = project_3d(wx, wy, wz)   -- returns nothing if behind camera

draw_wire_cube(cx, cy, cz, size, rx, ry, rz)
draw_wire_sphere(cx, cy, cz, r [, lat [, lon]])
draw_wire_grid(size, divs [, y])
```

### Canvas

Offscreen render target with its own optional local shader chain.

```lua
local c = canvas.new(w, h)    -- allocate in on_load, not on_frame

c:begin()
    clear(0, 0, 0, 1)
    draw_circle(...)
c:set_uniform("u_zoom", 1.5)  -- set uniforms on the local pipeline
c:finish("julia")             -- optional local shader pass

c:draw(x, y [, w, h [, angle]])
```

### Fractal Shaders

Render Mandelbrot or Julia sets — full-screen or into a canvas.

```lua
-- Full screen
shader_set("mandelbrot")
shader_set_uniform("u_zoom", 3.0)
shader_set_uniform("u_color_shift", t * 0.03)

-- Into a canvas (use canvas:set_uniform for control)
c:begin()  clear(0,0,0,1)
c:set_uniform("u_zoom",    1.3)
c:set_uniform("u_animate", 0.3)   -- orbits c, morphing the shape
c:finish("julia")
c:draw(0, 0, screen_width, screen_height)
```

### Cellular Automata

```lua
-- Wolfram 1D elementary automata
local ca = wolfram.new(width, rule)
ca:step()
ca:get(x)   -- 0 or 1

-- Conway's Game of Life
local life = conway.new(cols, rows)
life:randomize(density)
life:step()
life:get(col, row)   -- 0 or 1
```

### Images & Sprites

```lua
local img   = image.load("assets/photo.png")
local sheet = sprite_sheet.new("assets/walk.png", 64, 64)

img:draw(x, y [, w, h [, angle]])
sheet:draw(frame_idx, x, y [, w, h [, angle]])
```

### OSC

Listens on **UDP port 9000**. Multiple clients (SuperCollider, Pure Data, TouchOSC) work simultaneously.

```lua
function on_osc(addr, ...)
    local args = {...}
    if addr == "/speed" then speed = args[1] end
end
```

**Engine-level address** (never forwarded to `on_osc`):

```
/scene "scenes/matrix.lua"    ← load a new scene from any OSC client
```

From SuperCollider:

```supercollider
~p = NetAddr("127.0.0.1", 9000);
~p.sendMsg("/speed", 1.5);
~p.sendMsg("/scene", "scenes/life.lua");
```

---

## Example Scenes

All scenes are in `scenes/`. Run any with `./build/phosphor -s scenes/<name>.lua`.

| Scene | What it shows |
|-------|---------------|
| `test.lua` | Core primitives: rect, circle, line, rotating square, dot grid |
| `feedback_test.lua` | Phosphor trails with `draw_feedback` |
| `noise_test.lua` | Perlin noise vs fractal Brownian motion side by side |
| `wolfram_test.lua` | Wolfram elementary cellular automata (rules 30, 90, 110…) |
| `life.lua` | Conway's Game of Life, toroidal, with phosphor afterglow |
| `datafield.lua` | Monochrome data aesthetics — barcode, noise grid, scan pulse |
| `matrix.lua` | Digital rain with geometric glyphs, resize-aware |
| `canvas_test.lua` | Two canvases with different local shaders |
| `waveform_test.lua` | All four wave types as polylines and as modulators |
| `wire3d_test.lua` | Orbiting camera, cube, sphere, floor grid, point cloud |
| `fractal_test.lua` | Mandelbrot and Julia sets, four-phase auto-sequence |
| `everything_test.lua` | All systems together: fractal background, Life grid, 3D wireframe, waveform strip |

---

## Custom Shaders

Drop a `.frag` file in `shaders/` and load it by name.

```glsl
#version 330 core

uniform sampler2D u_texture;   -- previous pipeline stage (or scene FBO)
uniform vec2      u_resolution;
uniform float     u_time;
uniform float     u_beat;

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    frag_color = texture(u_texture, v_uv);
}
```

Custom `float` uniforms are set from Lua with `shader_set_uniform("name", value)`.

---

## Project Structure

```
phosphor/
├── src/            C++ engine source
├── scenes/         Lua example scenes
├── shaders/        GLSL fragment shaders
├── assets/         Images for example scenes
├── docs/           index.html — full API reference
└── vendor/         Embedded dependencies (Lua 5.4, GLAD, tinyosc, stb_image)
```

---

## Dependencies

All vendored — no package manager needed beyond SDL2 and CMake:

- **Lua 5.4** — scripting VM
- **GLAD** — OpenGL function pointer loader
- **tinyosc** — minimal OSC parser
- **stb_image** — PNG/JPG loader
