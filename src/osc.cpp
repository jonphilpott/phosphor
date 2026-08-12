#include "osc.h"
#include "osc_parse.h"

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

    // Deliberately NOT setting SO_REUSEPORT.
    //
    // SO_REUSEPORT lets several processes bind the same UDP port — but the
    // kernel then delivers each datagram to exactly ONE of them, chosen by the
    // kernel. So if anything else on the machine is already listening on this
    // port (an OSC monitor such as Protokol, or a forgotten phosphor instance),
    // our bind succeeds, we print "OSC listening", and messages then vanish
    // into the other process with no error anywhere.
    //
    // Silently receiving nothing is the worst possible failure for a live
    // control path: everything looks fine and the visuals just don't respond.
    // Without the option, a clash fails the bind loudly at startup instead, and
    // -p gives you another port to move to.

    // Step 3: Bind to all network interfaces (INADDR_ANY) on the given port.
    // This means we'll receive messages sent to any of our IP addresses —
    // localhost from SC/PD on the same machine, or LAN from TouchOSC.
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);   // htons = host-to-network byte order

    if (bind(m_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("osc: bind()");
        fprintf(stderr,
                "osc: could not listen on UDP port %u — another program is "
                "probably using it\n"
                "     (an OSC monitor, or another phosphor instance).\n"
                "     Close it, or start phosphor with -p <port>.\n", port);
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

        // Receive one UDP datagram.  The sender address is filled in but not
        // used: every address is dispatched to the scene the same way, and no
        // OSC message can do anything privileged.
        sockaddr_in sender{};
        socklen_t   sender_len = sizeof(sender);
        ssize_t nbytes = recvfrom(m_socket, buf, sizeof(buf), 0,
                                  (sockaddr*)&sender, &sender_len);
        if (nbytes <= 0) continue;

        // Parse the whole datagram.  parse_packet handles both plain messages
        // and bundles, and never reads past nbytes — no part of the packet is
        // trusted to be well-formed or terminated.
        m_parsed.clear();
        osc_parse::parse_packet(buf, (size_t)nbytes, m_parsed);

        if (m_parsed.empty()) continue;

        // Hand the batch to the main thread.  One lock for the whole datagram
        // rather than one per message keeps the critical section short.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& parsed : m_parsed) m_queue.push(std::move(parsed));
        }
    }
}
