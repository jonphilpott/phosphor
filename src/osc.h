#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

// ── OscArg ────────────────────────────────────────────────────────────────────
// A single typed argument from an OSC message.
// We only care about the three types Phosphor will ever receive: int, float,
// string.  Anything else (blobs, timetags, MIDI) is silently ignored.
struct OscArg {
    char type;       // 'i' = int32, 'f' = float32, 's' = string
    int32_t i = 0;
    float   f = 0.0f;
    std::string s;
};

// ── OscMessage ────────────────────────────────────────────────────────────────
// A fully parsed, owned OSC message.  All strings are copied out of the
// receive buffer, so this struct is safe to queue and read on another thread.
struct OscMessage {
    std::string          address;
    std::vector<OscArg>  args;

    // Convenience accessors — return default value if index is out of range
    // or the type doesn't match.
    int32_t     int_arg(size_t idx, int32_t def = 0)    const;
    float       float_arg(size_t idx, float def = 0.0f) const;
    std::string str_arg(size_t idx, const char* def = "") const;
};

// ── OscServer ─────────────────────────────────────────────────────────────────
// Listens on a UDP port for OSC messages from any number of clients
// (SuperCollider, PureData, TouchOSC, etc.).
//
// Threading model:
//   - A dedicated recv thread calls recvfrom() in a blocking select() loop.
//   - Parsed messages are pushed onto a mutex-protected queue.
//   - The main thread calls poll() once per frame to drain the queue.
//   - Lua callbacks are only ever called from the main thread.
//
// Usage:
//   OscServer osc;
//   osc.start(9000);
//
//   // in main loop:
//   std::vector<OscMessage> msgs;
//   osc.poll(msgs);
//   for (auto& m : msgs) { ... }
//
//   osc.stop();  // called automatically in destructor
class OscServer {
public:
    OscServer();
    ~OscServer();

    // Bind to the given UDP port and start the recv thread.
    // Returns false if the socket could not be opened.
    bool start(uint16_t port);

    // Signal the recv thread to stop and join it.  Safe to call multiple times.
    void stop();

    // Drain all pending messages into 'out'.  Call once per frame on the main
    // thread.  'out' is cleared before messages are appended.
    void poll(std::vector<OscMessage>& out);

private:
    // The recv thread entry point.
    void recv_loop();

    int              m_socket = -1;
    std::thread      m_thread;
    std::atomic<bool> m_running{false};

    std::queue<OscMessage> m_queue;
    std::mutex             m_mutex;
};
