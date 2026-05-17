


function make_node()
   local n = {
      r = 10,
      c = {1, 0, 0}
   }

   function n:draw()
      set_color(self.c[1], self.c[2], self.c[3])
      draw_circle(0, 0, self.r)
   end

   return n
end



function on_load()
   shader_set("chromatic_ab", "glitch")
end


local t = 0
local n = make_node();


function on_frame(dt)
   t = t + dt

   local W = screen_width
   local H = screen_height

   clear(0, 0, 0, 1)

   push()
   translate(W*0.5, H*0.5)
   n:draw()
   pop()
end
