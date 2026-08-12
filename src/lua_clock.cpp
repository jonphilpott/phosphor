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

// If no beat arrives for this long, an inferred clock is considered stale: it
// keeps free-running so the visuals do not freeze, but beat_active() reports
// false so a scene can fall back to wall-clock animation. A manually set tempo
// is never stale — it has no source to lose contact with.
constexpr double STALE_AFTER = 4.0;

struct Clock {
    double period       = 0.5;    // seconds per beat; 120 BPM until told otherwise
    bool   have_tempo   = false;  // inferred a tempo from /beat timings
    double manual_bpm   = 0.0;    // non-zero: tempo set by the scene, not inferred

    double last_beat_at = 0.0;    // wall time of the most recent beat boundary
    double now          = 0.0;    // wall time as of this frame
    long long beat_index = 0;     // whole beats counted at the last beat boundary

    // Is last_beat_at the time of a real /beat message, or an anchor we
    // invented? Only the gap between two REAL beats measures anything. An
    // anchor placed by startup, by a tempo change or by beat_reset() is at an
    // arbitrary point in the bar, so treating the next gap as an interval
    // feeds the estimator a number that means nothing — which showed up as a
    // wild first reading (39 BPM from a 100 BPM source) before the median
    // washed it out.
    bool real_anchor = false;

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

// ── The one quantity everything else is derived from ─────────────────────────
//
// Position on a continuous beat timeline: whole beats counted at the last beat
// boundary, plus however far time has moved since, measured in beats.
//
// Deriving phase, count and bar position from a single continuous value is what
// makes free-running work. Tracking them separately meant the beat *count* only
// advanced when a message arrived, so with no source connected the bar position
// was stuck cycling inside one beat slot and never swept the bar.
double beat_pos() {
    if (g.period <= 0.0) return 0.0;
    return (double)g.beat_index + (g.now + g.latency - g.last_beat_at) / g.period;
}

double frac(double v) { return v - std::floor(v); }

// Change the period without moving the phase.
//
// Phase is computed backwards from last_beat_at, so changing the period alone
// would make the current position jump. Re-anchoring keeps the visual exactly
// where it is and only alters the rate from here on — so a tempo change mid-set
// is a smooth acceleration rather than a snap.
void set_period_preserving_phase(double new_period) {
    if (new_period <= 0.0) return;
    const double pos = beat_pos();
    g.beat_index   = (long long)std::floor(pos);
    g.period       = new_period;
    g.last_beat_at = g.now + g.latency - frac(pos) * new_period;
    g.real_anchor  = false;      // invented anchor — see Clock::real_anchor
}

// ── Lua API ───────────────────────────────────────────────────────────────────

// bpm([n]) -> current tempo
//
// With no argument: the tempo in force — manually set if there is one, else
// inferred from /beat, else 0 when nothing is known yet.
//
// With a number: set the tempo directly, so beat_phase() and bar_phase() run
// without any OSC source at all. Useful for a tempo-synced piece that has no
// clock to follow, or for pinning the tempo you already know while letting
// /beat handle only the downbeat alignment.
//
// bpm(0) hands control back to inference from /beat.
int l_bpm(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        const double n = luaL_checknumber(L, 1);

        if (n <= 0.0) {
            // Back to following the messages. The inferred period is whatever
            // was last measured; if nothing has been measured the clock keeps
            // free-running at its current rate until beats arrive.
            g.manual_bpm = 0.0;
        } else {
            luaL_argcheck(L, n >= 1.0 && n <= 1000.0, 1, "bpm must be 1-1000");
            g.manual_bpm = n;
            set_period_preserving_phase(60.0 / n);
        }
    }

    if (g.manual_bpm > 0.0)  lua_pushnumber(L, g.manual_bpm);
    else if (g.have_tempo)   lua_pushnumber(L, 60.0 / g.period);
    else                     lua_pushnumber(L, 0.0);
    return 1;
}

int l_beat_phase(lua_State* L) {
    lua_pushnumber(L, frac(beat_pos()));
    return 1;
}

// Position within the bar, [0,1). Derived from the same continuous position, so
// it sweeps across the whole bar whether beats are arriving or the clock is
// free-running.
int l_bar_phase(lua_State* L) {
    const int bpb = g.beats_per_bar > 0 ? g.beats_per_bar : 4;
    lua_pushnumber(L, frac(beat_pos() / (double)bpb));
    return 1;
}

// Whole beats since the clock started. Advances while free-running too.
int l_beat_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)std::floor(beat_pos()));
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

// True when the clock means something: a tempo was set by hand, or beats are
// actually arriving. Lets a scene decide whether to follow musical time or fall
// back to elapsed().
int l_beat_active(lua_State* L) {
    const bool live = g.have_tempo && (g.now - g.last_beat_at) < STALE_AFTER;
    lua_pushboolean(L, g.manual_bpm > 0.0 || live);
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

// beat_reset() — put the downbeat here.
//
// With a manual tempo and no /beat to align to, this is how a scene says "bar
// starts now" — on a key, on an OSC message, or at the top of a section.
int l_beat_reset(lua_State* L) {
    (void)L;
    g.beat_index   = 0;
    g.last_beat_at = g.now + g.latency;
    g.real_anchor  = false;
    return 0;
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
    lua_register(L, "beat_reset",     l_beat_reset);
    lua_register(L, "visual_latency", l_visual_latency);
    lua_register(L, "env_trigger",    l_env_trigger);
    lua_register(L, "env",            l_env);
}

void on_beat(double wall_seconds) {
    // Measure only between two real beats. The first beat after startup, after
    // a tempo change or after beat_reset() has nothing meaningful to measure
    // against, so it just becomes the new anchor.
    if (g.real_anchor) {
        const double gap = wall_seconds - g.last_beat_at;

        // A gap shorter than MIN_PERIOD is a duplicate or a stray message:
        // ignored entirely, and deliberately does not advance the beat count.
        if (gap < MIN_PERIOD) return;

        // With a manual tempo the period is the scene's to decide, so incoming
        // beats are used only to re-align the downbeat — the message says
        // *where* the beat is, the scene says how fast they go.
        if (g.manual_bpm <= 0.0) {
            if (gap <= MAX_PERIOD) {
                g.intervals.push_back(gap);
                if ((int)g.intervals.size() > HISTORY)
                    g.intervals.erase(g.intervals.begin());
                g.period     = median_of(g.intervals);
                g.have_tempo = true;
            } else {
                // A long silence means the previous tempo estimate describes a
                // different passage of music. Start gathering again rather than
                // averaging across the gap.
                g.intervals.clear();
                g.have_tempo = false;
            }
        }
    }

    // Snap the timeline to this beat. Taking the count from the free-running
    // position rather than simply incrementing means a clock that has drifted
    // over several missed beats resumes counting from where it actually is.
    const double pos = beat_pos();
    g.beat_index   = (long long)std::llround(pos);
    g.last_beat_at = wall_seconds;
    g.real_anchor  = true;
}

void update(double wall_seconds) {
    g.now = wall_seconds;
    if (g.last_beat_at == 0.0) g.last_beat_at = wall_seconds;  // first frame
}

float beat_phase() { return (float)frac(beat_pos()); }

}  // namespace lua_clock
