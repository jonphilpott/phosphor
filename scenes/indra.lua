-- scenes/indra.lua
--
-- INDRA'S.. HEX.
--
-- Toroidal grid of nodes that propagate excitation as a ripple.
-- OSC sources excite individual nodes; influence spreads to neighbours
-- each tick at INFLUENCE_RATE Hz.  The view scrolls continuously across
-- the grid, driven by /scroll dx dy (cells per second).
--
-- OSC (port 9000):
--   /left_vu   f  i   -- excite left-channel node i with amplitude f (0-100)
--   /right_vu  f  i   -- excite right-channel node i with amplitude f (0-100)
--   /input_vu  f      -- excite centre node with amplitude f
--   /tape_vu   f  i   -- excite tape-position node i with amplitude f
--   /scroll    dx dy  -- set scroll velocity in grid cells per second

local NODE_BASE_RADIUS = 1
local NODE_SMOOTH_RATE = 5
local GRID_SIZE        = 32
local INFLUENCE_RATE   = 4   -- Hz; lower = slower, more distinct wave front
local TEXT_MODE        = false

local INPUT_CELL       = {math.floor(GRID_SIZE/2), math.floor(GRID_SIZE/2)}
local LINE_THRESHOLD   = 10   -- input node val() above which web lines are drawn

local FEEDBACK_ALPHA_BASE = 0.5
local FEEDBACK_SCALE_BASE = 0.5

local feedback_alpha = 0
local feedback_scale = 0

-- ── Node ──────────────────────────────────────────────────────────────────────

local CHARS = "!@#$%&*+=<>?|/\\"

local function rand_char()
    local i = math.random(1, #CHARS)
    return string.sub(CHARS, i, i)   -- same index for both bounds = exactly one character
end


function make_node()
   local n = {
      r  = 1,                  -- current smoothed radius
      t  = NODE_BASE_RADIUS,   -- target radius
      c  = {0.8, 0.8, 0.8},   -- colour {r, g, b}
      g  = rand_char(),
      s  = 1.0,
      input = false,
   }

   -- Advance simulation state: smooth r toward target, reset when overshot.
   -- Called once per frame in a full-grid pass before drawing, so no node
   -- is ever updated more than once even at the scroll wrap seam.
   function n:update(dt)
      self.r  = smooth(self.r, self.t, NODE_SMOOTH_RATE, dt)
      if self.r + 1 > self.t then
         self.t = NODE_BASE_RADIUS
      end
   end

   -- Draw only — no state change.  Safe to call multiple times per frame
   -- (the extra wrap column/row calls this on already-updated cells).
   function n:draw()
      local rad = self.r + 1

      if self.r < 4 and self.input == false then
         local rc = 0.1
         set_color(rc, rc, rc)
      else
         set_color(self.c[1], self.c[2], self.c[3], 0.9)
      end

      if TEXT_MODE then
         -- Each glyph is 8×8 at scale 1, so 8*sz × 8*sz at scale sz.
         -- Offset by -4*sz so the character is centred on the node origin.
         local sz = clamp(rad * 0.1, 1, 16) * self.s
         draw_text(-4 * sz, -4 * sz, self.g, sz)
      else
         draw_rect(-rad/2,-rad/2,rad,rad)
      end
   end

   -- Propose a peak target height.  Uses max() rather than addition so a node
   -- rises to the strongest influence it receives but can't accumulate higher
   -- than that — prevents runaway when multiple neighbours are excited.
   function n:target(it)
      --self.g = rand_char()
      self.t = math.max(self.t, NODE_BASE_RADIUS + it)
   end

   function n:val()
      return self.r
   end

   function n:scale(s)
      self.s = s
   end

   return n
end


-- ── Grid ──────────────────────────────────────────────────────────────────────

local grid = {}
for y = 1, GRID_SIZE do
   grid[y] = {}
   for x = 1, GRID_SIZE do
      grid[y][x] = make_node()
   end
end

-- Random node maps for left/right VU sources — each of the 16 bands is
-- wired to a fixed random position in the grid, set once at load time.
local left_map  = {}
local right_map = {}
for i = 1, 16 do
   left_map[i]  = {math.floor(math.random() * GRID_SIZE) + 1,
                   math.floor(math.random() * GRID_SIZE) + 1}
   right_map[i] = {math.floor(math.random() * GRID_SIZE) + 1,
                   math.floor(math.random() * GRID_SIZE) + 1}
end


-- ── Scroll state ──────────────────────────────────────────────────────────────
-- scroll_x/y are fractional grid-cell positions (0..GRID_SIZE, wrapping).
-- target_dx/dy are OSC-requested velocities; actual velocity smooths toward
-- them so direction changes feel gradual rather than instantaneous.

local scroll_x  = 0.0
local scroll_y  = 0.0
local scroll_dx = 1.0
local scroll_dy = 0.0
local target_dx = 1.0
local target_dy = -2.0


-- ── Simulation ────────────────────────────────────────────────────────────────

local t               = 0
local influence_timer = 0

function grid_influence()
   for y = 1, GRID_SIZE do
      for x = 1, GRID_SIZE do
         -- Find the strongest excited neighbour (Moore neighbourhood, toroidal).
         -- Using max rather than sum prevents interior nodes from being amplified
         -- by multiple excited neighbours — each ring stays weaker than the last.
         -- 1-indexed wrap: (x - 1 + dx) % GRID_SIZE + 1
         local neighbour_max = 0
         for dy = -1, 1 do
            for dx = -1, 1 do
               if not (dx == 0 and dy == 0) then
                  local nx     = (x - 1 + dx) % GRID_SIZE + 1
                  local ny     = (y - 1 + dy) % GRID_SIZE + 1
                  local excess = math.max(0, grid[ny][nx]:val() - NODE_BASE_RADIUS)
                  neighbour_max = math.max(neighbour_max, excess)
               end
            end
         end

         -- randomize the influence so the fields aren't always so square
         if math.random() > 0.33 then
               -- Small random noise seeds organic variation across the field;
               -- the max() in target() ensures it can't cause runaway on its own.
               grid[y][x]:target(neighbour_max + math.random() * 0.2)
         end
      end
   end
end


-- ── Lifecycle ─────────────────────────────────────────────────────────────────

function on_load()
   shader_set("glitch", "chromatic_ab", "scanlines")
   shader_set_uniform("u_glitch_amount", 0.3)
end

function on_osc(addr, ...)
   local args = {...}

   if addr == "/left_vu" then
      local pos = left_map[args[2]]
      local n   = grid[pos[1]][pos[2]]
      n.c = {0.0, 1.0, 1.0}
      n.s = 2
      n.input = true
      n:target(args[1] * 0.2)

   elseif addr == "/right_vu" then
      local pos = right_map[args[2]]
      local n   = grid[pos[1]][pos[2]]
      n.c = {0.0, 1.0, 0.0}
      n.s = 2
      n.input = true
      n:target(args[1] * 0.2)

   elseif addr == "/input_vu" then
      local n = grid[INPUT_CELL[1]][INPUT_CELL[2]]
      n.c = {1.0, 1.0, 0.0}
      n.s = 2
      n.input = true
      n:target(args[1] * 0.2)

   elseif addr == "/tape_vu" then
      local tape_pos = math.floor(args[2]) * 8
      local n        = grid[1][tape_pos]
      n.c = {1.0, 0.5, 0.2}
      n.s = 2
      n.input = true
      n:target(args[1] * 0.2)

   elseif addr == "/feedback_alpha" then
      feedback_alpha = (args[1] * 0.5) or feedback_alpha
   elseif addr == "/feedback_scale" then
      feedback_scale = (args[1] * 0.5) or feedback_scale
   elseif addr == "/scroll" then
      target_dx = args[1] or target_dx
      target_dy = args[2] or target_dy
   end
end

function on_frame(dt)
   t = t + dt

   local W = screen_width
   local H = screen_height

   draw_feedback(FEEDBACK_ALPHA_BASE + feedback_alpha, FEEDBACK_SCALE_BASE + feedback_scale, 0.005)

   -- ── Influence tick ────────────────────────────────────────────────────────
   influence_timer = influence_timer + dt
   if influence_timer >= 1.0 / INFLUENCE_RATE then
      influence_timer = influence_timer - 1.0 / INFLUENCE_RATE
      grid_influence()
   end

   -- ── Update all nodes ──────────────────────────────────────────────────────
   for y = 1, GRID_SIZE do
      for x = 1, GRID_SIZE do
         grid[y][x]:update(dt)
      end
   end

   -- ── Advance scroll ────────────────────────────────────────────────────────
   scroll_dx = smooth(scroll_dx, target_dx, 3, dt)
   scroll_dy = smooth(scroll_dy, target_dy, 3, dt)
   scroll_x  = (scroll_x + scroll_dx * dt) % GRID_SIZE
   scroll_y  = (scroll_y + scroll_dy * dt) % GRID_SIZE

   -- Integer cell offset and sub-cell pixel remainder for smooth motion.
   local cell_ox  = math.floor(scroll_x)
   local cell_oy  = math.floor(scroll_y)
   local x_step   = W / GRID_SIZE
   local y_step   = H / GRID_SIZE
   local pixel_ox = -(scroll_x - cell_ox) * x_step
   local pixel_oy = -(scroll_y - cell_oy) * y_step

   -- ── Draw ──────────────────────────────────────────────────────────────────
   -- GRID_SIZE + 1 iterations per axis so the extra wrap cell fills the gap
   -- left by the sub-cell pixel offset at the right/bottom edge.
   for row = 0, GRID_SIZE do
      for col = 0, GRID_SIZE do
         local gx = (cell_ox + col) % GRID_SIZE + 1
         local gy = (cell_oy + row) % GRID_SIZE + 1
         push()
         translate(col * x_step + pixel_ox, row * y_step + pixel_oy)
         scale(W / (GRID_SIZE * 18))
         grid[gy][gx]:draw()
         pop()
      end
   end

   -- ── Input web ─────────────────────────────────────────────────────────────
   -- When the input node is excited above LINE_THRESHOLD, draw lines from it
   -- to every node in left_map.  Alpha scales with excitation level so the
   -- web fades in rather than snapping on at the threshold.
   local input_node = grid[INPUT_CELL[1]][INPUT_CELL[2]]
   if input_node:val() > LINE_THRESHOLD then

      -- Convert a {gy, gx} grid position to screen pixels using the current
      -- scroll offset.  Modulo GRID_SIZE handles toroidal wrap correctly.
      local function to_screen(pos)
         local col = (pos[2] - 1 - cell_ox + GRID_SIZE) % GRID_SIZE
         local row = (pos[1] - 1 - cell_oy + GRID_SIZE) % GRID_SIZE
         return col * x_step + pixel_ox, row * y_step + pixel_oy
      end

      local alpha = clamp(map(input_node:val(), LINE_THRESHOLD, 20, 0, 0.7), 0, 0.7)
      set_stroke(1, 1, 0.5, alpha)
      set_stroke_weight(1.0)

      local ix, iy = to_screen(INPUT_CELL)
      for _, pos in ipairs(left_map) do
         local lx, ly = to_screen(pos)
         draw_line(ix, iy, lx, ly)
      end
   end
end
