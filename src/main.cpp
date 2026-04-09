#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── CLI argument parsing ──────────────────────────────────────────────────────
// Usage: phosphor [-d <display_index>] [-s <scene_path>] [-f]
//
//   -d N      Open on display N (0 = primary, 1 = first external, etc.)
//   -s path   Load a Lua scene file at startup (e.g. -s scenes/test.lua)
//   -f        Start in fullscreen (same as pressing F after launch)

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [-d <display_index>] [-s <scene_path>] [-f]\n", argv0);
    fprintf(stderr, "  -d N      Open on display N (default: 0)\n");
    fprintf(stderr, "  -s path   Load scene file at startup\n");
    fprintf(stderr, "  -f        Start fullscreen\n");
}

int main(int argc, char* argv[]) {
    int         display_index = 0;
    const char* scene_path    = nullptr;
    bool        start_fullscreen = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            display_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scene_path = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0) {
            start_fullscreen = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    Engine engine(display_index);

    if (!engine.init()) {
        fprintf(stderr, "Engine init failed — exiting.\n");
        return 1;
    }

    // Go fullscreen before loading the scene so on_load() sees the correct
    // screen_width/screen_height globals for the fullscreen resolution.
    if (start_fullscreen) {
        engine.toggle_fullscreen();
    }

    // Load the startup scene after init so the GL context is ready and
    // screen_width/screen_height globals are already set.
    if (scene_path) {
        engine.load_scene(scene_path);
    }

    engine.run();
    return 0;
}
