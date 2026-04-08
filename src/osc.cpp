#include "osc.h"

// tinyosc is a C library — we wrap it in extern "C" (though tinyosc.h already
// does this, belt-and-suspenders when including from C++).
#include "../vendor/tinyosc.h"

// POSIX socket API — available on both macOS and Linux.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>    // close()
#include <fcntl.h>     // fcntl(), O_NONBLOCK
#include <sys/select.h>

#include <cstdio>
#include <cstring>

// Maximum UDP datagram we'll accept.  OSC messages in practice are tiny
// (a few hundred bytes), but 4096 gives plenty of headroom.
static constexpr int RECV_BUFSIZE = 4096;

// Forward declaration — defined below recv_loop.
static OscMessage parse_message(tosc_message* msg);

// ── OscMessage convenience accessors ─────────────────────────────────────────

int32_t OscMessage::int_arg(size_t idx, int32_t def) const {
    if (idx < args.size() && args[idx].type == 'i') return args[idx].i;
    return def;
}

float OscMessage::float_arg(size_t idx, float def) const {
    if (idx < args.size() && args[idx].type == 'f') return args[idx].f;
    return def;
}

std::string OscMessage::str_arg(size_t idx, const char* def) const {
    if (idx < args.size() && args[idx].type == 's') return args[idx].s;
    return def;
}

// ── OscServer ────────────────────────────────────────────────────────────────

OscServer::OscServer() = default;

OscServer::~OscServer() {
    stop();
}

bool OscServer::start(uint16_t port) {
    // Step 1: Create a UDP socket.
    // AF_INET = IPv4, SOCK_DGRAM = UDP (datagrams, not a stream).
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        perror("osc: socket()");
        return false;
    }

    // Step 2: Allow the port to be reused immediately after restart.
    // Without SO_REUSEADDR, if we crash and restart quickly, the OS may
    // refuse to bind because it thinks the port is still in use.
    int yes = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // SO_REUSEPORT allows multiple processes/sockets to bind the same port.
    // Not strictly needed here but handy if you run multiple phosphor instances.
    // It's not POSIX but is available on macOS and Linux 3.9+.
#ifdef SO_REUSEPORT
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    // Step 3: Bind to all network interfaces (INADDR_ANY) on the given port.
    // This means we'll receive messages sent to any of our IP addresses —
    // localhost from SC/PD on the same machine, or LAN from TouchOSC.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);   // htons = host-to-network byte order

    if (bind(m_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("osc: bind()");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    printf("OSC listening on UDP port %d\n", port);

    // Step 4: Start the recv thread.  The thread sets m_running = true before
    // blocking, and checks it each iteration so stop() can shut it down cleanly.
    m_running = true;
    m_thread  = std::thread(&OscServer::recv_loop, this);
    return true;
}

void OscServer::stop() {
    if (!m_running) return;
    m_running = false;

    // Closing the socket unblocks any pending select()/recvfrom() in the thread.
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
    if (m_thread.joinable()) m_thread.join();
}

void OscServer::poll(std::vector<OscMessage>& out) {
    out.clear();
    // Lock briefly to swap the queue contents into a local queue.
    // The lock is held for microseconds — just pointer/size swaps.
    std::queue<OscMessage> local;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::swap(m_queue, local);
    }
    // Drain the local queue into the output vector — no lock needed here.
    while (!local.empty()) {
        out.push_back(std::move(local.front()));
        local.pop();
    }
}

// ── recv_loop() — runs on the recv thread ────────────────────────────────────

void OscServer::recv_loop() {
    char buf[RECV_BUFSIZE];

    while (m_running) {
        // Use select() with a short timeout so we check m_running regularly.
        // Without this, recvfrom() would block forever after stop() is called
        // (unless the socket is closed, which we do — but the timeout is a
        // belt-and-suspenders safety net).
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_socket, &fds);

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;  // 50ms timeout

        int ready = select(m_socket + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;  // timeout or error — loop and check m_running

        // Receive one UDP datagram.  recvfrom fills in the sender address but
        // we don't need it — any sender is welcome (multi-client works for free).
        sockaddr_in sender{};
        socklen_t   sender_len = sizeof(sender);
        int nbytes = recvfrom(m_socket, buf, RECV_BUFSIZE - 1, 0,
                              (sockaddr*)&sender, &sender_len);
        if (nbytes <= 0) continue;

        // Step 1: Check if this is a bundle (multiple messages in one datagram).
        // OSC bundles start with the string "#bundle".
        if (tosc_isBundle(buf)) {
            tosc_bundle bundle;
            tosc_parseBundle(&bundle, buf, nbytes);
            tosc_message msg;
            while (tosc_getNextMessage(&bundle, &msg)) {
                OscMessage parsed = parse_message(&msg);
                if (!parsed.address.empty()) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queue.push(std::move(parsed));
                }
            }
        } else {
            // Step 2: Single message.
            tosc_message msg;
            if (tosc_parseMessage(&msg, buf, nbytes) == 0) {
                OscMessage parsed = parse_message(&msg);
                if (!parsed.address.empty()) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queue.push(std::move(parsed));
                }
            }
        }
    }
}

// ── parse_message() — convert tinyosc struct to our owned OscMessage ─────────
// Static free function — only used within this translation unit.
// tinyosc's message struct just points into the recv buffer; this function
// copies everything into a fully-owned OscMessage before the buffer is reused.

static OscMessage parse_message(tosc_message* msg) {
    OscMessage out;

    // Copy the address string immediately — tinyosc only points into our recv
    // buffer which will be overwritten on the next recvfrom().
    const char* addr = tosc_getAddress(msg);
    if (!addr || addr[0] != '/') return out;   // not a valid OSC address
    out.address = addr;

    // Iterate the format string (e.g. "ifs" = int, float, string).
    // Note: tosc_getFormat returns the format WITHOUT the leading comma —
    // it stores o->format = buffer + i + 1 (past the comma) in parseMessage.
    // So we start at index 0, not 1.
    const char* fmt = tosc_getFormat(msg);
    if (!fmt) return out;

    for (const char* t = fmt; *t != '\0'; ++t) {
        OscArg arg;
        arg.type = *t;
        switch (*t) {
            case 'i':
                arg.i = tosc_getNextInt32(msg);
                break;
            case 'f':
                arg.f = tosc_getNextFloat(msg);
                break;
            case 's': {
                // getNextString returns a pointer into the recv buffer — copy it.
                const char* sv = tosc_getNextString(msg);
                arg.s = sv ? sv : "";
                break;
            }
            default:
                // Unknown type (blob, timetag, MIDI, etc.) — skip silently.
                // tinyosc advances the read head internally so we can continue.
                continue;
        }
        out.args.push_back(std::move(arg));
    }

    return out;
}
