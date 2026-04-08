cmake_minimum_required(VERSION 3.20)
project(phosphor CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies ──────────────────────────────────────────────────────────────

# SDL2 via pkg-config — works on macOS Homebrew and Linux (apt/pacman).
# On macOS, make sure pkg-config can find Homebrew: run cmake with
#   cmake -B build -DCMAKE_PREFIX_PATH=$(brew --prefix)
# or export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig before running cmake.
find_package(PkgConfig REQUIRED)
# IMPORTED_TARGET creates a PkgConfig::SDL2 target that carries the lib path,
# include dirs, and compile flags together — avoids manual SDL2_LIBRARY_DIRS wrangling.
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# OpenGL — on macOS this pulls in the OpenGL.framework via the Apple linker.
# On Linux it finds libGL in the standard system paths.
find_package(OpenGL REQUIRED)

# GLAD — a small generated C file that loads OpenGL function pointers at
# runtime.  Without this, calling any GL function (like glClear) would crash
# because the function pointers are null.  We compile it as its own static
# library so CMake keeps it separate from our C++ translation units.
add_library(glad STATIC vendor/glad/src/glad.c)
target_include_directories(glad PUBLIC vendor/glad/include)

# Lua 5.4 — embedded from source so there's no system dependency.
# We glob all .c files in vendor/lua/src/ then remove the two standalone
# entry-point files: lua.c (the REPL) and luac.c (the compiler).
# Everything else is the library itself.
file(GLOB lua_src vendor/lua/src/*.c)
list(REMOVE_ITEM lua_src
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua/src/lua.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua/src/luac.c"
)
add_library(lua54 STATIC ${lua_src})
# PUBLIC so that any target linking lua54 gets the include path automatically.
target_include_directories(lua54 PUBLIC vendor/lua/src)
# Lua is C code — the linker language hint prevents CMake from trying to link
# it as C++ and getting confused about the ABI.
set_target_properties(lua54 PROPERTIES LINKER_LANGUAGE C)
# On Linux, Lua's dynamic loading (require) needs libdl.
if(UNIX AND NOT APPLE)
    target_link_libraries(lua54 PRIVATE dl)
endif()

# ── Main executable ───────────────────────────────────────────────────────────

add_executable(phosphor
    src/main.cpp
    src/engine.cpp
    src/osc.cpp
    src/lua_state.cpp
    src/lua_bindings.cpp
    src/lua_automata.cpp
    src/lua_noise.cpp
    src/lua_image.cpp
    src/lua_canvas.cpp
    src/lua_waveform.cpp
    src/lua_3d.cpp
    src/lua_text.cpp
    src/stb_image_impl.cpp
    src/renderer.cpp
    src/shader_pipeline.cpp
    vendor/tinyosc.c
)

# stb_image.h lives in vendor/ — make it visible to any source that includes it.
target_include_directories(phosphor PRIVATE vendor)

target_include_directories(phosphor PRIVATE src)

# PkgConfig::SDL2 carries includes, lib path, and compile flags in one target.
target_link_libraries(phosphor PRIVATE
    glad
    lua54
    PkgConfig::SDL2
    OpenGL::GL
)

# macOS: OpenGL was deprecated in 10.14 Mojave but still fully works.
# This flag silences the flood of deprecation warnings so we can see real ones.
if(APPLE)
    target_compile_definitions(phosphor PRIVATE GL_SILENCE_DEPRECATION)
endif()
