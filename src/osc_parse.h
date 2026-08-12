#pragma once

#include "osc.h"
#include <cstddef>
#include <vector>

// ── osc_parse ─────────────────────────────────────────────────────────────────
//
// A strict, bounds-checked reader for OSC 1.0 packets.
//
// Why we don't use a library for this:  the data being parsed arrives from the
// network, from any host that can reach our UDP port.  A parser that trusts the
// packet — that scans for a terminator without a length bound, or advances a
// read head by a size it took off the wire — will read memory it doesn't own the
// moment somebody sends a malformed datagram.  That is exactly the class of bug
// this module exists to avoid, so every single read here checks the remaining
// byte count *before* touching a byte.
//
// The wire format is simple enough that this is only ~150 lines:
//
//   Message:  <address string> <type-tag string> <argument data...>
//   Bundle:   "#bundle\0" <8-byte timetag> then repeated:
//             <int32 element size> <element bytes (a message or nested bundle)>
//
// Strings are NUL-terminated and then zero-padded so their total length is a
// multiple of 4.  All numbers are big-endian ("network byte order").

namespace osc_parse {

// Parse one received datagram into zero or more messages, appended to `out`.
//
// Returns true if the whole packet was understood.  On false, the packet was
// malformed and should be dropped — though `out` may already hold messages
// recovered from the valid leading part of a bundle, which is deliberate: one
// bad element at the end of a bundle shouldn't discard the good ones ahead of it.
//
// `data` need not be NUL-terminated; nothing is read beyond `len`.
bool parse_packet(const char* data, size_t len, std::vector<OscMessage>& out);

}  // namespace osc_parse
