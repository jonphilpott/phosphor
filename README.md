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
| `-f` | Start fullscreen |
| `-p <n>` | OSC UDP port (default 9000) |
| `-h` | Print help and exit |

### Keyboard

**Output — live safety**

| Key | Action |
|-----|--------|
| `B` | Blackout. Output to black instantly; the scene keeps running underneath, so restoring brings back a live image, not a frozen one |
| `-` / `=` | Master output level down / up, 10% steps |
| `F` | Toggle fullscreen on the current display |

**Transport**

| Key | Action |
|-----|--------|
| `Space` | Pause — scene redraws with `dt = 0`, so trails hold still instead of going black |
| `[` / `]` | Halve / double the rate time passes (`dt`, `elapsed()`, `u_time`) |
| `\` | Back to ×1 speed, unpaused |

Transport does not affect the musical clock, which runs on the wall clock —
pausing the visuals won't distort tempo or beat phase.

**Scene and parameters**

| Key | Action |
|-----|--------|
| `R` | Reload now, without waiting for a file save (`persist` still survives) |
| `P` | Overlay declared parameters and their values; `*` marks live OSC control |
| `0` | Reset parameters to the defaults written in the scene file |

**Setup**

| Key | Action |
|-----|--------|
| `T` | Projector alignment grid, over the running scene — includes a circle that reveals aspect-ratio stretch |
| `Esc` | Quit |

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

Built-in globals: `screen_width`, `screen_height` (updated on resize and fullscreen toggle), `elapsed()` for seconds since startup — the same clock shaders get as `u_time` — and `persist`, a table that survives hot reload.

**Hot reload:** save the file while Phosphor is running — the Lua VM reloads automatically. GPU state is untouched. Fragment shaders in `shaders/` reload too, recompiling in place; if one fails to compile the previous version keeps running.

**State across reloads:** a reload rebuilds the Lua VM, so a scene normally
restarts from nothing on every save. Anything in the global `persist` table
survives instead — create it only if it isn't already there:

```lua
function on_load()
    if not persist.particles then
        persist.particles = {}      -- built once, kept across every reload
        for i = 1, 300 do
            persist.particles[i] = { pos = vec(0, 0), vel = vec.random(50) }
        end
    end
    persist.t = persist.t or 0
end
```

Numbers, strings, booleans, vectors and nested tables of those cross. Functions
and the resource-owning userdata types (canvas, image, sprite_sheet, conway,
wolfram) cannot — they're dropped and reported rather than vanishing quietly.

**Errors don't stop the show:** a runtime error in `on_frame` holds the last good frame on screen and prints the message over it rather than going black, and throttles the log to once a second. Fix the file and the next clean frame clears it.

---

## API Overview

Full reference is in [`docs/index.html`](docs/index.html) — open it in any browser.

### Drawing

```lua
clear(r, g, b, a)
set_color(r, g, b, a)          -- fill colour for rect/circle
set_stroke(r, g, b, a)         -- stroke colour for lines/points
set_stroke_weight(w)
set_circle_segments(n)         -- draw_circle tessellation (default 32)
set_blend(mode)                -- "alpha" (default), "add", "multiply", "screen"
draw_rect(x, y, w, h)
draw_circle(cx, cy, r)
draw_line(x1, y1, x2, y2)
draw_point(x, y)
```

`set_blend("add")` makes light accumulate instead of replace, so overlapping
strokes brighten toward white — the phosphor/neon look, especially with
`draw_feedback`. Blend mode resets to `"alpha"` each frame.

### Transform Stack

```lua
push()  pop()
translate(x, y)
rotate(radians)
scale(sx [, sy])
```

### Text

Built-in 8×8 bitmap font — printable ASCII (`0x20`–`0x7E`), no font file needed.
Uses the fill colour and goes through the transform stack, so text rotates and
scales like any other geometry.

```lua
draw_text(x, y, str [, scale])    -- scale 1 = 8px glyphs; \n starts a new line
text_width(str [, scale])         -- pixel width of the longest line
```

```lua
local label = "SIGNAL LOST"
set_color(1, 0.3, 0.2, 1)
draw_text(screen_width / 2 - text_width(label, 2) / 2, 100, label, 2)
```

### Vectors

A 2D vector type in the spirit of Processing's `PVector`, implemented in C.

```lua
vec(x, y)                         -- also vec.new(x, y)
vec.from_angle(a [, len])
vec.random([len])

v.x, v.y                          -- readable and writable
a + b   a - b   -a                -- component-wise
v * 2   2 * v   v / 2             -- scalar
a * b   a / b                     -- component-wise (use a:dot(b) for dot product)

v:mag()  v:mag_sq()  v:dist(u)  v:dist_sq(u)
v:dot(u) v:cross(u)  v:heading()  v:angle_to(u)  v:xy()
```

Transformations come in two forms. Plain names return a **new** vector; a
trailing underscore **modifies in place** and returns the vector, so calls chain:

```lua
v:normalize()   v:limit(m)   v:rotate(a)   v:lerp(u, t)
v:normalize_()  v:limit_(m)  v:rotate_(a)  v:lerp_(u, t)
v:set_(u)  v:add_(u)  v:sub_(u)  v:scale_(s)

v:smooth(target, rate, dt)        -- frame-rate independent chase
v:smooth_hl(target, half_life, dt)
```

`smooth` is the one worth knowing — anything that follows something else wants
it, and unlike a per-frame `lerp` it behaves the same at 30fps and 144fps.

Every operator allocates a new vector. That's the right default; for a loop
doing thousands of operations a frame, the in-place forms allocate nothing:

```lua
-- readable
local steer = (target - p.pos):normalize() * FORCE * dt
p.vel = (p.vel + steer):limit(MAX_SPEED)

-- allocation-free, one scratch vector reused across all particles
steer:set_(target):sub_(p.pos):normalize_():scale_(FORCE * dt)
p.vel:add_(steer):limit_(MAX_SPEED)
p.pos:add_(p.vel.x * dt, p.vel.y * dt)
```

Anywhere a vector is accepted, two numbers work too: `p:add_(0, 9.8)`.
Vectors survive hot reload, so a field of them can live in `persist`.
See `scenes/vector_test.lua`.

### HSV Colour

```lua
hsv(h, s, v [, a])                -- returns r, g, b, a
set_color_hsv(h, s, v [, a])
set_stroke_hsv(h, s, v [, a])
```

All components are 0..1 (hue is **not** in degrees), and hue **wraps** — so you
can feed an ever-increasing number straight in:

```lua
set_color(hsv(elapsed() * 0.1, 0.9, 1))   -- hue cycling on the engine clock
clear(hsv(0.6, 0.4, 0.12))
```

Because `hsv()` returns four values it must be the last argument — pass alpha to
`hsv` itself rather than after it.

### Easing & Math

```lua
lerp(a, b, t)                     -- linear interpolation, not clamped
smooth(current, target, rate, dt) -- frame-rate independent exponential smoothing
smooth_hl(current, target, half_life, dt)   -- same, parameterised by half-life
map(x, in_lo, in_hi, out_lo, out_hi)        -- remap a range, not clamped
clamp(x, lo, hi)
smoothstep(t)                     -- S-curve on [0,1], clamped
pulse(t, bpm, width)              -- 0..1 spike on each beat
```

`smooth` and `smooth_hl` are the ones worth knowing: chasing a target with
`lerp(current, target, 0.1)` every frame moves twice as far at 30fps as at
60fps, whereas these stay consistent however the frame rate wanders.

### Values that glide

`smooth_hl` needs somewhere to park the value between frames and a call every
frame to advance it. `slew` keeps both for you: set a target from anywhere, read
back a value that is already on its way there.

```lua
slew(name, default [, slew_time])   -- declares and reads, safe every frame
  .set(v)                           -- retarget; the glide starts from here
  .get()                            -- current value; foo() reads it too
```

```lua
function on_frame(dt)
    local hue = slew("hue", 0.55, 0.15)     -- 0.15s to cover 99% of a jump
    if beat_count() % 4 == 0 then hue.set(0.9) end
    set_color_hsv(hue.get(), 0.8, 1.0)
end
```

The curve is exponential, so retargeting mid-glide bends it rather than kinking
it, and a `slew_time` of 0 snaps. Slews run on the engine's clock: pause freezes
one where it stands and `[` / `]` stretch it, like everything else on screen.

Values live outside the Lua VM so they survive a reload, and **the file wins
until `set()`**: a slew follows the default in your source until the scene sets
it, after which the live value sticks and saving mid-glide won't snap it back.
The slew time is a declaration, not a value, so it always follows the file —
retune the glide, save, and the new timing applies at once. Press `0` to fall
back to file defaults.

`hue` is a handle, not a number: read it with `hue.get()` or `hue()`, because
Lua has no way to make a bare table stand in for a number everywhere.

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

reset_3d()                        -- back to the default camera and projection
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

Listens on **UDP port 9000** (change with `-p`). Multiple clients (SuperCollider, Pure Data, TouchOSC) work simultaneously.

If another program on the machine already holds the port — an OSC monitor such
as Protokol, or a forgotten phosphor instance — the bind fails at startup with
a message saying so, and phosphor runs on without OSC. It does **not** quietly
share the port: the kernel would hand each datagram to only one listener, and
losing every message with no error is a far worse way to find out.

```lua
function on_osc(addr, ...)
    local args = {...}
    if addr == "/speed" then speed = args[1] end
end
```

### Receiving OSC without a dispatch ladder

```lua
-- Events: named arguments, one handler per address
on("/left_vu", function(level, index) ... end)
on("/kick",    function() env_trigger("kick") end)

-- Values: declares and reads in one call, safe to call every frame
local alpha  = param("feedback_alpha", 0.0, 0, 1)   -- /feedback_alpha <f>
local scroll = param("scroll", vec(0, 0))           -- /scroll <f> <f>
local col    = param("color", {0.2, 0.8, 0.4})      -- /color <f> <f> <f>
```

The default's **type** fixes how many OSC arguments a param consumes (number 1,
vec 2, table of N → N, plus string and boolean). Wrong arity or wrong types are
ignored rather than half-applied; `min`/`max` clamp.

Param values live outside the Lua VM so they survive a reload, and **the file
wins until OSC takes over**: a param follows the default in your source until a
message arrives for it, after which the live value sticks and a save won't stomp
it. Press `0` to fall back to file defaults.

Neither mechanism consumes messages — everything still reaches `on_osc`.

### Musical Clock

`/beat` arrives as discrete events; the engine infers tempo from the gaps and
gives you a phase that advances continuously between them.

```lua
bpm([n])             -- get, or SET the tempo directly
beat_phase()  bar_phase()  beat_count()
beats_per_bar([n])   beat_active()   beat_reset()   visual_latency([s])

env_trigger(name)          -- fire a named envelope
env(name [, half_life])    -- read it, decaying 1 → 0
```

```lua
rotate(bar_phase() * math.pi * 2)      -- exactly one turn per bar
on("/kick", function() env_trigger("kick") end)
local flash = env("kick", 0.12)
```

`bpm(128)` sets the tempo directly, so `beat_phase()` and `bar_phase()` run with
**no OSC source at all**. With a manual tempo, incoming `/beat` no longer changes
the rate but still re-aligns the downbeat — the message says *where* the beat is,
your scene says how fast. `bpm(0)` returns to inference. Changing tempo preserves
the current phase, so it accelerates rather than snapping.

Inferred tempo uses the median of the last eight intervals, so one late UDP
packet doesn't drag it. The clock free-runs if beats stop — phase, beat count
and bar position all keep advancing — and survives reload. Shaders get
`u_beat_phase` alongside `u_beat`.

**Engine-level address** (never forwarded to `on_osc`):

```
/beat  0.0                    ← set u_beat and fire the on_beat(phase) hook
```

There is deliberately no OSC address for loading a scene. A scene is arbitrary
Lua with `os.execute` behind it, so anything that loaded one from the network
would be remote code execution. Scenes are chosen with `-s` and changed by
editing the file — hot reload does the rest.

From SuperCollider:

```supercollider
~p = NetAddr("127.0.0.1", 9000);
~p.sendMsg("/speed", 1.5);
~p.sendMsg("/beat", 0.0);
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

Uniforms from Lua:

```lua
shader_set_uniform("u_amount", 0.5)            -- float
shader_set_uniform("u_pos", vec(x, y))         -- vec2 (or two numbers)
shader_set_uniform("u_tint", 1.0, 0.6, 0.3)    -- vec3 (four args → vec4)
shader_set_data("u_bands", {0.2, 0.9, ...})    -- array → 1-row float texture
```

`shader_set_data` is how bulk data reaches a shader — a uniform holds four
numbers, a spectrum is hundreds:

```glsl
uniform sampler2D u_bands;
float band = texture(u_bands, vec2(v_uv.x, 0.5)).r;
```

`canvas:set_uniform` and `canvas:set_data` are the canvas-local equivalents.

---

## Project Structure

```
phosphor/
├── src/            C++ engine source
├── scenes/         Lua example scenes
├── shaders/        GLSL fragment shaders
├── assets/         Images for example scenes
├── docs/           index.html — full API reference
└── vendor/         Embedded dependencies (Lua 5.4, GLAD, stb_image)
```

---

## Dependencies

All vendored — no package manager needed beyond SDL2 and CMake:

- **Lua 5.4** — scripting VM
- **GLAD** — OpenGL function pointer loader
- **stb_image** — PNG/JPG loader

OSC parsing is hand-written in `src/osc_parse.cpp` rather than vendored: the
data arrives over UDP from anywhere on the network, so every read is bounds
checked against the datagram length.
