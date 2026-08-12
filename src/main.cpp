#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// ── CLI argument parsing ──────────────────────────────────────────────────────
// Usage: phosphor [-d <display_index>] [-s <scene_path>] [-f]
//
//   -d N      Open on display N (0 = primary, 1 = first external, etc.)
//   -s path   Load a Lua scene file at startup (e.g. -s scenes/test.lua)
//   -f        Start in fullscreen (same as pressing F after launch)
//   -p N      UDP port to listen for OSC on (default 9000)
//   --allow-remote-scene   Let off-machine OSC clients send /scene

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [-d <display_index>] [-s <scene_path>] [-f] [-p <port>]\n",
            argv0);
    fprintf(stderr, "  -d N      Open on display N (default: 0)\n");
    fprintf(stderr, "  -s path   Load scene file at startup\n");
    fprintf(stderr, "  -f        Start fullscreen\n");
    fprintf(stderr, "  -p N      OSC UDP port (default 9000)\n");
    fprintf(stderr, "  --allow-remote-scene\n"
                    "            Honour OSC /scene from other machines.  Off by default:\n"
                    "            /scene executes a Lua file, so accepting it from the\n"
                    "            network lets anyone who can reach port 9000 run code.\n"
                    "            Scenes must still resolve inside the -s scene directory.\n");
}

int main(int argc, char* argv[]) {
    // Line-buffer stdout.
    //
    // C defaults to block buffering whenever stdout isn't a terminal, so piping
    // phosphor into a log file or another program held back every message until
    // 4KB had accumulated or the process exited. That silence defeats the point
    // of the hot-reload and error reporting: they exist to tell you what just
    // happened, and "in a few minutes, maybe" is no use during a set.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    int         display_index = 0;
    const char* scene_path    = nullptr;
    bool        start_fullscreen = false;
    bool        allow_remote_scene = false;
    int         osc_port = 9000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            display_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            scene_path = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0) {
            start_fullscreen = true;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            osc_port = atoi(argv[++i]);
            if (osc_port < 1 || osc_port > 65535) {
                fprintf(stderr, "Invalid OSC port %d (must be 1-65535)\n", osc_port);
                return 1;
            }
        } else if (strcmp(argv[i], "--allow-remote-scene") == 0) {
            allow_remote_scene = true;
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
    engine.set_osc_port((uint16_t)osc_port);

    if (!engine.init()) {
        fprintf(stderr, "Engine init failed — exiting.\n");
        return 1;
    }

    // Queue fullscreen for after the first frame — see Engine::request_fullscreen()
    // for why it must be deferred on Wayland.
    if (start_fullscreen) {
        engine.request_fullscreen();
    }

    // Establish where OSC-loaded scenes are allowed to come from before any
    // OSC message can be dispatched: the directory holding the startup scene,
    // or the working directory when launched without -s.
    engine.set_scene_root(scene_path);
    engine.set_allow_remote_scene(allow_remote_scene);
    if (allow_remote_scene) {
        fprintf(stderr, "Warning: remote OSC /scene enabled — any host that can "
                        "reach port 9000 may load scenes from the scene directory.\n");
    }

    // Load the startup scene after init so the GL context is ready and
    // screen_width/screen_height globals are already set.
    if (scene_path) {
        engine.load_scene(scene_path);
    }

    engine.run();
    return 0;
}
