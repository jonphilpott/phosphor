#pragma once

extern "C" {
#include "lua.h"
}

namespace lua_3d {
    // Register all 3D wireframe functions as Lua globals.
    //
    // Setup:
    //   camera_3d(ex,ey,ez, tx,ty,tz)        -- set eye position and look target
    //   perspective_3d(fov [, near [, far]])  -- fov in radians; auto aspect from screen size
    //   reset_3d()                            -- restore defaults (eye=(0,3,6), target=origin)
    //
    // Projection:
    //   sx, sy = project_3d(wx, wy, wz)       -- returns nil,nil if behind camera
    //
    // Shapes (drawn with current stroke colour + weight, transform stack applies):
    //   draw_wire_cube(cx,cy,cz, size, rx,ry,rz)
    //   draw_wire_sphere(cx,cy,cz, r [, lat_segs [, lon_segs]])
    //   draw_wire_grid(size, divs [, y])
    void register_all(lua_State* L);
}
