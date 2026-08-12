#include "osc_parse.h"

#include <cstring>
#include <cstdint>

namespace {

// Maximum nesting depth for bundles-inside-bundles.  The spec permits arbitrary
// nesting; we don't, because unbounded recursion driven by network input is a
// stack-overflow waiting to happen.  Nobody nests bundles more than once or twice.
constexpr int  MAX_BUNDLE_DEPTH = 4;

// Upper bound on messages produced by one datagram.  A legitimate bundle from a
// sequencer might hold a few dozen; a hostile one could claim thousands and make
// us allocate for each.  256 is far above any real use.
constexpr size_t MAX_MESSAGES_PER_PACKET = 256;

// ── Reader ────────────────────────────────────────────────────────────────────
//
// A cursor over the packet with the remaining byte count attached.  Every read
// goes through `take()`, which is the single place where a length check happens
// — so it is impossible for a read to escape the buffer without that one
// function being wrong.  Concentrating the danger in one four-line function is
// the whole trick: there's exactly one thing to get right and to review.
class Reader {
public:
    Reader(const char* data, size_t len) : m_p(data), m_left(len) {}

    size_t left() const { return m_left; }

    // Consume `n` bytes and return a pointer to them, or nullptr if there
    // aren't that many left.  Callers MUST check for nullptr.
    const char* take(size_t n) {
        if (n > m_left) return nullptr;
        const char* p = m_p;
        m_p    += n;
        m_left -= n;
        return p;
    }

    // Read a big-endian 32-bit signed integer.
    // OSC is defined as network byte order, which is big-endian: the most
    // significant byte comes first.  We assemble it by hand rather than calling
    // ntohl on a cast pointer, because that cast would also assume the packet
    // happens to be 4-byte aligned in memory — an unaligned load is undefined
    // behaviour on some architectures and a fault on others.
    bool read_i32(int32_t& out) {
        const char* p = take(4);
        if (!p) return false;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
        out = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                        ((uint32_t)b[2] <<  8) |  (uint32_t)b[3]);
        return true;
    }

    // Read a big-endian 32-bit float.
    // The bits are read as an integer and then reinterpreted, via memcpy so we
    // don't break strict-aliasing rules (a `*(float*)&i` cast is technically
    // undefined behaviour and modern compilers do miscompile it).
    bool read_f32(float& out) {
        int32_t bits;
        if (!read_i32(bits)) return false;
        memcpy(&out, &bits, sizeof(out));
        return true;
    }

    // Read an OSC string: NUL-terminated, then zero-padded to a multiple of 4.
    //
    // The terminator is searched for only within the bytes we actually have —
    // this is the exact spot where the old tinyosc parser ran off the end,
    // because its scan loop had no bound at all.
    bool read_string(std::string& out) {
        size_t n = 0;
        while (n < m_left && m_p[n] != '\0') ++n;
        if (n == m_left) return false;   // no terminator inside the packet

        // Total consumed = the string, its NUL, and any padding that rounds
        // the whole lot up to the next multiple of 4.
        size_t padded = (n + 4) & ~(size_t)3;
        const char* p = take(padded);
        if (!p) return false;            // terminator found but padding missing

        out.assign(p, n);
        return true;
    }

    // Skip a fixed-width argument we don't surface to Lua (int64, double,
    // timetag).  Safe because the width is known from the type tag alone.
    bool skip(size_t n) { return take(n) != nullptr; }

    // Skip a blob: an int32 byte count followed by that many bytes, padded to
    // a multiple of 4.  The count comes off the wire, so it is checked against
    // the remaining length before being used.
    bool skip_blob() {
        int32_t size;
        if (!read_i32(size)) return false;
        if (size < 0)        return false;
        size_t padded = ((size_t)size + 3) & ~(size_t)3;
        return skip(padded);
    }

private:
    const char* m_p;
    size_t      m_left;
};

// ── parse_message() ───────────────────────────────────────────────────────────
//
// Reads one OSC message from `r`.  Returns false if the message is malformed,
// in which case `msg` must be discarded.
bool parse_message(Reader& r, OscMessage& msg) {
    // Step 1: the address pattern, e.g. "/beat".  It must start with '/'.
    if (!r.read_string(msg.address)) return false;
    if (msg.address.empty() || msg.address[0] != '/') return false;

    // Step 2: the type-tag string, e.g. ",ifs".
    //
    // OSC 1.0 requires this, but a few older senders omit it entirely for
    // messages with no arguments.  We accept that (nothing follows, so there's
    // nothing unsafe about it) but anything present must be a well-formed tag
    // string starting with a comma.
    if (r.left() == 0) return true;

    std::string tags;
    if (!r.read_string(tags)) return false;
    if (tags.empty() || tags[0] != ',') return false;

    // Step 3: one argument per tag, in order.  The tag tells us the width, so
    // we always know how far to advance — provided we recognise the tag.
    for (size_t i = 1; i < tags.size(); ++i) {
        char tag = tags[i];
        OscArg arg;
        arg.type = tag;

        switch (tag) {
            case 'i':
                if (!r.read_i32(arg.i)) return false;
                msg.args.push_back(std::move(arg));
                break;

            case 'f':
                if (!r.read_f32(arg.f)) return false;
                msg.args.push_back(std::move(arg));
                break;

            case 's':
            case 'S':   // "symbol" — same wire format as a string
                if (!r.read_string(arg.s)) return false;
                arg.type = 's';
                msg.args.push_back(std::move(arg));
                break;

            // ── Types we understand but don't pass to Lua ──────────────────
            // These have a known width, so skipping them keeps the read head
            // correctly positioned for the arguments that follow.
            case 'h':   // int64
            case 't':   // timetag
            case 'd':   // float64
                if (!r.skip(8)) return false;
                break;

            case 'c':   // ASCII character, sent padded to 4 bytes
            case 'r':   // RGBA colour
            case 'm':   // 4-byte MIDI message
                if (!r.skip(4)) return false;
                break;

            case 'b':   // blob
                if (!r.skip_blob()) return false;
                break;

            // Zero-width tags: the tag itself carries the whole value, so
            // there is nothing to read or skip.
            case 'T': case 'F': case 'N': case 'I':
                break;

            // ── Anything else ─────────────────────────────────────────────
            // An unrecognised tag has an unknown width, so we cannot know
            // where the next argument begins.  Carrying on would mean parsing
            // the remainder at a wrong offset, so the message is rejected.
            default:
                return false;
        }
    }

    return true;
}

// ── parse_bundle() ────────────────────────────────────────────────────────────
//
// A bundle is "#bundle\0", an 8-byte timetag, then a sequence of length-prefixed
// elements.  Each element is itself either a message or another bundle.
bool parse_bundle(Reader& r, std::vector<OscMessage>& out, int depth) {
    if (depth > MAX_BUNDLE_DEPTH) return false;

    // "#bundle\0" (8 bytes) + timetag (8 bytes).  We don't schedule on the
    // timetag — phosphor dispatches everything on the next frame — so it's
    // skipped rather than decoded.
    if (!r.skip(16)) return false;

    while (r.left() > 0) {
        // Element size, straight off the wire — the value that must never be
        // trusted.  take() inside the sub-reader bounds every use of it.
        int32_t size;
        if (!r.read_i32(size)) return false;
        if (size < 0)          return false;

        const char* elem = r.take((size_t)size);
        if (!elem) return false;   // claimed more bytes than the packet holds

        Reader sub(elem, (size_t)size);

        // Nested bundle or plain message?  Same check as the top level.
        if ((size_t)size >= 8 && memcmp(elem, "#bundle", 8) == 0) {
            if (!parse_bundle(sub, out, depth + 1)) return false;
        } else {
            if (out.size() >= MAX_MESSAGES_PER_PACKET) return false;
            OscMessage msg;
            if (!parse_message(sub, msg)) return false;
            out.push_back(std::move(msg));
        }
    }

    return true;
}

}  // namespace

namespace osc_parse {

bool parse_packet(const char* data, size_t len, std::vector<OscMessage>& out) {
    if (!data || len == 0) return false;

    Reader r(data, len);

    // A bundle is identified by the literal 8 bytes "#bundle\0" at the start.
    // The length check matters: without it we'd compare against bytes past the
    // end of a short datagram.
    if (len >= 8 && memcmp(data, "#bundle", 8) == 0) {
        return parse_bundle(r, out, 0);
    }

    if (out.size() >= MAX_MESSAGES_PER_PACKET) return false;

    OscMessage msg;
    if (!parse_message(r, msg)) return false;
    out.push_back(std::move(msg));
    return true;
}

}  // namespace osc_parse
