#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── CLI argument parsing ──────────────────────────────────────────────────────
// Usage: phosphor [-d <display_index>] [-s <scene_path>]
//
//   -d N      Open on display N (0 = primary, 1 = first external, etc.)
//   -s path   Load a Lua scene file at startup (e.g. -s scenes/test.lua)

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [-d <display_index>] [-s <scene_path>]\n", argv0);
    fprintf(stderr, "  -d N      Open on display N (default: 0)\n");
    fprintf(stderr, "  -s path   Load scene file at startup\n");
}

int main(int argc, char* argv[]) {
    int   display_index = 0;
    const char* scene_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            display_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scene_path = argv[++i];
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

    // Load the startup scene after init so the GL context is ready and
    // screen_width/screen_height globals are already set.
    if (scene_path) {
        engine.load_scene(scene_path);
    }

    engine.run();
    return 0;
}
