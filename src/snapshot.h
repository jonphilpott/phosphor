#pragma once

// ── snapshot ──────────────────────────────────────────────────────────────────
// Saves the finished frame to a PNG on disk.
//
// Why this is a module and not four lines in Engine
// ------------------------------------------------
// Writing a PNG is slow — compressing a 1920x1080 image takes tens of
// milliseconds, which at 60fps is several dropped frames.  During a
// performance that is a visible stutter every time you hit the key.  So the
// work is split across two threads:
//
//   main (GL) thread : glReadPixels off the feedback FBO into a byte buffer.
//                      This part CANNOT move — OpenGL calls are only legal on
//                      the thread that owns the context.  Costs a few ms.
//   writer thread    : convert RGBA to RGB, compress, write the file.
//                      Costs whatever it costs; nobody is waiting.
//
// There is exactly ONE writer thread, not one per snapshot.  If you hammer the
// key ten times the jobs queue up behind each other and the disk is written
// sequentially, rather than ten threads all seeking against one another.
//
// Requests are also QUEUED rather than serviced immediately.  A Lua scene
// calling snapshot() does so from inside on_frame, which runs BEFORE the
// renderer composites — capturing there would save the previous frame.  So
// everything (keypress, OSC, Lua) calls request(), and the engine calls
// service() once per frame after end_frame(), when the feedback FBO holds the
// image that was just shown.

namespace snapshot {

// Queue a capture for the end of the current frame.
//
// basename — optional.  nullptr or "" gives the default timestamped name.
//            Anything else is used as the stem of the filename, so passing
//            "intro" writes snapshots/intro.png (and snapshots/intro-2.png if
//            that already exists).
//
// Main thread only.  Multiple requests in one frame collapse into one — the
// frame only has one image in it, so saving it twice is never what was meant.
void request(const char* basename = nullptr);

// If a capture was requested, read the pixels and hand them to the writer.
// Call once per frame from the engine, after end_frame() has composited.
//
// fbo — framebuffer to read from (the renderer's feedback FBO, which holds the
//       final post-processed image at full brightness).
void service(unsigned int fbo, int width, int height);

// Finish any queued writes and stop the writer thread.  Called at shutdown so
// a snapshot taken just before quitting still reaches the disk.
void shutdown();

}  // namespace snapshot
