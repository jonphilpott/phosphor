// glad.h first — must precede any other GL header.
#include <glad/glad.h>

#include "snapshot.h"
#include "stb_image_write.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>   // mkdir, stat

namespace {

// ── The job handed from the GL thread to the writer thread ────────────────────
//
// It owns its pixels outright.  There is no pointer back into anything the main
// thread might touch, which is what makes this safe: once the job is on the
// queue the writer can take as long as it likes and the render loop can carry
// on overwriting FBOs, resizing the window, or loading a new scene.
struct Job {
    std::vector<unsigned char> rgba;   // width*height*4, bottom-up (GL order)
    int         width  = 0;
    int         height = 0;
    std::string path;
};

// ── Writer-thread state ───────────────────────────────────────────────────────
// Guarded by g_mutex; g_cv is how the GL thread wakes the writer up.
std::mutex              g_mutex;
std::condition_variable g_cv;
std::queue<Job>         g_jobs;
std::thread             g_worker;
bool                    g_worker_started = false;
bool                    g_quit           = false;

// ── Pending-request state ─────────────────────────────────────────────────────
// Main thread only — no lock needed.  request() sets it, service() consumes it.
bool        g_pending = false;
std::string g_pending_name;

// The directory snapshots land in, relative to wherever phosphor was launched.
const char* k_dir = "snapshots";

// ── writer_loop() ─────────────────────────────────────────────────────────────
// Pulls jobs off the queue and writes them, forever, until told to quit AND the
// queue has run dry.  Draining before exit is deliberate: hitting S and then
// Esc a moment later must still produce a file.

void writer_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);

            // wait() releases the mutex and sleeps until notified, then
            // re-acquires it and re-checks the predicate.  The predicate form
            // is used rather than a bare wait() because condition variables are
            // allowed to wake up spuriously — without the check we could
            // proceed to pop an empty queue.
            g_cv.wait(lock, [] { return g_quit || !g_jobs.empty(); });

            if (g_jobs.empty()) return;   // woken to quit, nothing left to do

            job = std::move(g_jobs.front());
            g_jobs.pop();
        }

        // ── Step 1: RGBA → RGB ────────────────────────────────────────────
        //
        // We read four channels off the GPU because 4 bytes per pixel keeps
        // every row naturally aligned and is the path drivers optimise for;
        // asking for GL_RGB can be markedly slower.  But the alpha channel is
        // meaningless here — scenes using additive blending leave all sorts in
        // it — and a PNG with a junk alpha channel opens as a half-transparent
        // mess.  So we throw it away, and we do it here rather than on the GL
        // thread because this thread is the one nobody is waiting for.
        const size_t px = (size_t)job.width * (size_t)job.height;
        std::vector<unsigned char> rgb(px * 3);
        for (size_t i = 0; i < px; ++i) {
            rgb[i * 3 + 0] = job.rgba[i * 4 + 0];
            rgb[i * 3 + 1] = job.rgba[i * 4 + 1];
            rgb[i * 3 + 2] = job.rgba[i * 4 + 2];
        }

        // ── Step 2: Write the PNG ─────────────────────────────────────────
        //
        // OpenGL's origin is bottom-left, so glReadPixels hands back rows in
        // the opposite order to the one PNG stores them in.  Left alone the
        // image comes out upside down.  This flag makes stb walk the rows
        // backwards as it encodes — cheaper than flipping the buffer ourselves.
        // It is a global inside stb, but this is the only thread that ever
        // writes an image, so there is nothing to race with.
        stbi_flip_vertically_on_write(1);

        const int stride = job.width * 3;   // bytes per row
        if (stbi_write_png(job.path.c_str(), job.width, job.height, 3,
                           rgb.data(), stride)) {
            printf("[snapshot] %s (%dx%d)\n", job.path.c_str(),
                   job.width, job.height);
        } else {
            fprintf(stderr, "[snapshot] failed to write %s\n", job.path.c_str());
        }
    }
}

// Start the writer on first use.  Most sessions never take a snapshot, and
// there is no reason to carry a sleeping thread around for them.
void ensure_worker() {
    if (g_worker_started) return;
    g_worker_started = true;
    g_worker = std::thread(writer_loop);
}

// ── sanitise() ────────────────────────────────────────────────────────────────
// Reduces a caller-supplied name to something safe to paste into a path.
//
// This matters more than it looks.  The name can arrive over the network in an
// OSC packet, and OSC has no authentication whatsoever — anything that can
// reach the port can send one.  A name of "../../.ssh/authorized_keys" would
// otherwise let a stranger on the same wifi choose where phosphor writes.  So
// every character outside a small safe set becomes an underscore, which makes
// path traversal impossible rather than merely inconvenient.

std::string sanitise(const char* name) {
    std::string out;
    for (const char* p = name; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += ok ? c : '_';
        if (out.size() >= 64) break;   // keep filenames sane
    }
    return out;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ── make_path() ───────────────────────────────────────────────────────────────
// Builds the full output path, creating the snapshots directory if needed.
// Returns an empty string if the directory could not be made.
//
// basename — nullptr for the default timestamped name.

std::string make_path(const char* basename) {
    // mkdir returns -1 with errno EEXIST if the directory is already there,
    // which is the normal case and not an error.  Rather than inspect errno we
    // just try to create it and then check whether a directory now exists —
    // simpler, and it also catches "a FILE called snapshots is in the way".
    ::mkdir(k_dir, 0755);

    struct stat st;
    if (stat(k_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[snapshot] cannot create directory '%s/'\n", k_dir);
        return std::string();
    }

    std::string stem;
    if (basename && *basename) {
        stem = sanitise(basename);
    }
    if (stem.empty()) {
        // Default: phosphor-YYYYMMDD-HHMMSS, which sorts chronologically as
        // plain text — no date parsing needed to find the last one you took.
        const time_t now = time(nullptr);
        struct tm    tm_buf;
        localtime_r(&now, &tm_buf);     // the _r form is the thread-safe one

        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm_buf);
        stem = std::string("phosphor-") + ts;
    }

    std::string path = std::string(k_dir) + "/" + stem + ".png";

    // Two snapshots inside the same second (or two uses of the same explicit
    // name) would otherwise silently overwrite each other, which during a set
    // means losing the one you actually wanted.  Suffix instead.
    for (int n = 2; n < 1000 && file_exists(path); ++n) {
        path = std::string(k_dir) + "/" + stem + "-" + std::to_string(n) + ".png";
    }

    return path;
}

}  // namespace

// ── Public interface ──────────────────────────────────────────────────────────

void snapshot::request(const char* basename) {
    // Collapse repeats: the frame has one image in it, so a keypress and an OSC
    // message landing in the same frame should not produce two identical files.
    // First request in the frame wins, including its name.
    if (g_pending) return;

    g_pending = true;
    g_pending_name = (basename && *basename) ? basename : "";
}

void snapshot::service(unsigned int fbo, int width, int height) {
    if (!g_pending) return;
    g_pending = false;

    if (width <= 0 || height <= 0) return;

    const std::string path = make_path(g_pending_name.empty()
                                       ? nullptr : g_pending_name.c_str());
    g_pending_name.clear();
    if (path.empty()) return;

    // ── Read the pixels back off the GPU ──────────────────────────────────
    //
    // This is the part that costs the render loop something: glReadPixels is
    // synchronous, so the driver must finish everything queued for this
    // framebuffer before it can hand us the bytes.  A few milliseconds at
    // 1080p — one frame of jitter, once, when you press the key.  (The fully
    // asynchronous version needs a pixel buffer object and a one-frame delay;
    // not worth the machinery for something triggered by hand.)
    Job job;
    job.width  = width;
    job.height = height;
    job.path   = path;
    job.rgba.resize((size_t)width * (size_t)height * 4);

    // Bind as the READ target specifically, leaving the draw binding alone, so
    // this cannot disturb whatever the renderer is set up to draw into next.
    GLint prev_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);

    // How tightly rows are packed on the way back.  At 4 bytes per pixel every
    // row is already 4-byte aligned so the default of 4 is correct, but scenes
    // and libraries can change this global and leave it changed — being
    // explicit costs nothing and removes a whole class of "why is my image
    // sheared" bug.
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, job.rgba.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);

    // ── Hand it off ───────────────────────────────────────────────────────
    ensure_worker();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_jobs.push(std::move(job));
    }
    g_cv.notify_one();
}

void snapshot::shutdown() {
    if (!g_worker_started) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_quit = true;
    }
    g_cv.notify_one();

    // join() blocks until the writer has drained the queue and returned.  Quit
    // may therefore take a moment if a write is in flight, which is the right
    // trade: the alternative is losing the file.
    g_worker.join();
    g_worker_started = false;
}
