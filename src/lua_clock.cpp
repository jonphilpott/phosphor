#include "lua_clock.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
}

namespace {

// How many recent beat intervals to consider when estimating tempo.
// Eight is a couple of bars: long enough to be steady, short enough to follow
// a tempo change within a phrase.
constexpr int   HISTORY      = 8;

// Intervals outside this range are rejected as noise rather than folded into
// the estimate — 20 BPM to 400 BPM. A double-fired message or a several-second
// gap would otherwise wreck the average.
constexpr double MIN_PERIOD  = 60.0 / 400.0;
constexpr double MAX_PERIOD  = 60.0 / 20.0;

// If no beat arrives for this long, the clock is considered stale: it keeps
// free-running (so visuals do not freeze) but beat_active() reports false so a
// scene can fall back to wall-clock animation.
constexpr double STALE_AFTER = 4.0;

struct Clock {
    double period       = 0.5;    // seconds per beat; 120 BPM until told otherwise
    bool   have_tempo   = false;

    double last_beat_at = 0.0;    // wall time of the most recent /beat
    double now          = 0.0;    // wall time as of this frame
    long long beat_index = 0;     // beats counted since the first message

    int    beats_per_bar = 4;
    double latency       = 0.0;   // seconds to shift visual timing by

    std::vector<double> intervals;             // recent gaps between beats
    std::map<std::string, double> env_at;      // envelope name -> trigger time
};

Clock g;

// Median rather than mean.
//
// Beat messages arrive over UDP on a machine also doing graphics, so one of
// them being 30ms late is normal. A mean drags the tempo estimate toward every
// outlier; a median ignores a minority of bad samples entirely, which is
// exactly the behaviour wanted when the underlying tempo is steady.
double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// Position within the current beat, with the latency offset applied.
double phase_now() {
    if (g.period <= 0.0) return 0.0;
    double p = (g.now + g.latency - g.last_beat_at) / g.period;
    p -= std::floor(p);            // wrap into [0,1) and keep free-running
    return p;
}

// ── Lua API ───────────────────────────────────────────────────────────────────

int l_bpm(lua_State* L) {
    lua_pushnumber(L, g.have_tempo ? 60.0 / g.period : 0.0);
    return 1;
}

int l_beat_phase(lua_State* L) {
    lua_pushnumber(L, phase_now());
    return 1;
}

// Position within the bar, [0,1). Combines the whole beats counted so far with
// the fractional position inside the current one, so it advances smoothly
// across the bar rather than stepping once per beat.
int l_bar_phase(lua_State* L) {
    const int bpb = g.beats_per_bar > 0 ? g.beats_per_bar : 4;
    const double beats_in = (double)(g.beat_index % bpb) + phase_now();
    lua_pushnumber(L, beats_in / (double)bpb);
    return 1;
}

int l_beat_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g.beat_index);
    return 1;
}

// beats_per_bar([n]) — get, or set and get.
int l_beats_per_bar(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        const int n = (int)luaL_checkinteger(L, 1);
        luaL_argcheck(L, n >= 1 && n <= 64, 1, "beats_per_bar must be 1-64");
        g.beats_per_bar = n;
    }
    lua_pushinteger(L, g.beats_per_bar);
    return 1;
}

// True while beats are actually arriving. Lets a scene decide whether to follow
// musical time or fall back to elapsed().
int l_beat_active(lua_State* L) {
    lua_pushboolean(L, g.have_tempo && (g.now - g.last_beat_at) < STALE_AFTER);
    return 1;
}

// visual_latency([seconds]) — get, or set and get.
//
// Projectors, scalers and capture chains add delay. A positive value shifts the
// visual clock forward so that what you see lines up with what the room hears.
int l_visual_latency(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        g.latency = luaL_checknumber(L, 1);
    }
    lua_pushnumber(L, g.latency);
    return 1;
}

// env_trigger(name) — start (or restart) a named envelope at full level.
int l_env_trigger(lua_State* L) {
    g.env_at[luaL_checkstring(L, 1)] = g.now;
    return 0;
}

// env(name, half_life) -> 0..1
//
// Exponential decay from the moment of the last trigger, so the same name can
// be read from anywhere without the scene tracking any state. A name never
// triggered reads 0.
//
//     on("/kick", function() env_trigger("kick") end)
//     local flash = env("kick", 0.12)
int l_env(lua_State* L) {
    const char*  name = luaL_checkstring(L, 1);
    const double hl   = luaL_optnumber(L, 2, 0.15);

    auto it = g.env_at.find(name);
    if (it == g.env_at.end() || hl <= 0.0) { lua_pushnumber(L, 0.0); return 1; }

    const double age = g.now - it->second;
    lua_pushnumber(L, age < 0.0 ? 1.0 : std::pow(0.5, age / hl));
    return 1;
}

}  // namespace

namespace lua_clock {

void register_all(lua_State* L) {
    lua_register(L, "bpm",            l_bpm);
    lua_register(L, "beat_phase",     l_beat_phase);
    lua_register(L, "bar_phase",      l_bar_phase);
    lua_register(L, "beat_count",     l_beat_count);
    lua_register(L, "beats_per_bar",  l_beats_per_bar);
    lua_register(L, "beat_active",    l_beat_active);
    lua_register(L, "visual_latency", l_visual_latency);
    lua_register(L, "env_trigger",    l_env_trigger);
    lua_register(L, "env",            l_env);
}

void on_beat(double wall_seconds) {
    if (g.last_beat_at > 0.0) {
        const double gap = wall_seconds - g.last_beat_at;

        if (gap >= MIN_PERIOD && gap <= MAX_PERIOD) {
            g.intervals.push_back(gap);
            if ((int)g.intervals.size() > HISTORY) g.intervals.erase(g.intervals.begin());
            g.period     = median_of(g.intervals);
            g.have_tempo = true;
        } else if (gap > MAX_PERIOD) {
            // A long silence means the previous tempo estimate describes a
            // different passage of music. Start gathering again rather than
            // averaging across the gap.
            g.intervals.clear();
            g.have_tempo = false;
        }
        // A gap shorter than MIN_PERIOD is a duplicate or a stray message:
        // ignored entirely, and deliberately does not advance the beat count.
        if (gap < MIN_PERIOD) return;
    }

    g.last_beat_at = wall_seconds;
    g.beat_index++;
}

void update(double wall_seconds) {
    g.now = wall_seconds;
    if (g.last_beat_at == 0.0) g.last_beat_at = wall_seconds;  // first frame
}

float beat_phase() { return (float)phase_now(); }

}  // namespace lua_clock
