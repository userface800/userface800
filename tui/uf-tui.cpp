// uf-tui — terminal front-end for the FF800's mixer and status.
//
// A CLIENT of the daemon, like every front-end. It never opens a dext user
// client, so it cannot collide with the daemon inside the dext's single AT context — the hazard that
// made a live UI unsafe before the control plane existed.
//
//   reads   /userface800.state shm  — matrix, gains, flags, pairing, meters, rate, clock, lock. One
//                               writer, N readers, and a UI that hangs cannot backpressure the
//                               realtime pump.
//   writes  the control socket — cells, faders, flags, pairing, submix copy/clear. The daemon folds
//                               these into the mixer model, so a route survives the next rate change.
//
// ## Layout: the Matrix, and why
//
// TotalMix has two views of one mixer (manual §26.1): a channel-strip mixer, and a Matrix that is a
// patchbay — sources down the side, hardware outputs across the top, a gain at each crosspoint. The
// Matrix is the one worth having in a terminal. It shows every routing at once, it is monaural so
// there is no stereo-pair bookkeeping in the way, and a grid of numbers is what a terminal is good
// at. The channel-strip view is a mouse-and-fader idea.
//
// The rows follow the manual's three-row model exactly (§25.2), which is also how the matrix RAM is
// laid out (spec/07 §7.2):
//
//   HARDWARE INPUTS    -> outputs      the first 0x80 of each destination's block
//   SOFTWARE PLAYBACK  -> outputs      the second 0x80
//   HARDWARE OUTPUTS                   the fader row at 0x1f80, plus mute and loopback
//
// The input half is what zero-latency monitoring means and what the FF800's DSP is for; the playback
// half is what the DAW sends. Showing only the first, as this did before, hid half the mixer — and
// hid the row whose defaults were writing themselves at every session start.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include "../platform/shared/uf_ctl_client.hpp"
#include "../platform/shared/uf_control.hpp"
#include "../platform/shared/uf_meters.hpp"
#include "../platform/shared/uf_mixer_model.hpp"
#include "../platform/shared/uf_state_shm.hpp"
#include "uf/protocol/channels.hpp"
#include "uf/protocol/mixer.hpp"

using namespace ftxui;

namespace {

constexpr int kCh = (int)uf::MixerModel::kCh;   // 28 at 1x

// The device's own channel names (channels.hpp). Using them rather than bare numbers matters for
// the same reason it mattered for CoreAudio: slots 9/10
// are the MIC inputs on capture but the PHONES sends on playback, so "9" alone is ambiguous in
// precisely the place a user is most likely to get it wrong.
std::vector<std::string> names(uf::Direction dir) {
    std::vector<std::string> v;
    for (auto sl : uf::channel_map(dir, uf::Speed::X1)) v.push_back(uf::channel_name(sl));
    v.resize(kCh, "?");
    return v;
}

// Column headers are vertical: "Analog 1" across 28 columns would need a 200-character terminal.
// Abbreviate to the group initial plus the number — A1, M9, P1, SL, D3 — with the full name shown
// for whatever is selected.
std::string short_name(const std::string& full) {
    if (full.rfind("Analog ", 0) == 0) return "A" + full.substr(7);
    if (full.rfind("Mic ", 0) == 0)    return "M" + full.substr(4);
    if (full.rfind("Phones ", 0) == 0) return "P" + full.substr(7);
    if (full.rfind("ADAT ", 0) == 0)   return "D" + full.substr(5);
    if (full == "SPDIF L") return "SL";
    if (full == "SPDIF R") return "SR";
    return full.substr(0, 3);
}

// Which row group the cursor is in. The output row is its own thing: it has no source, and its
// controls (fader, mute, loopback) belong to the channel rather than to a crosspoint.
enum class Row { Input, Playback, Output };

struct StateReader {
    const uf::state::Shared* map = nullptr;
    uf::state::Shared snap{};

    std::string err;

    bool attach() {
        if (map) return true;
        int fd = shm_open(uf::state::kName, O_RDONLY, 0);
        if (fd < 0) { err = "no daemon state — is uf-daemon running?"; return false; }
        void* p = mmap(nullptr, sizeof(uf::state::Shared), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { err = "state segment could not be mapped"; return false; }
        const auto* s = static_cast<const uf::state::Shared*>(p);
        // Version, not just magic. The magic is unchanged across layout revisions, so attaching to
        // an older daemon's segment would map fields that have moved and draw the result as if it
        // were the mixer.
        if (!s->compatible()) {
            err = "state layout v" + std::to_string(s->version) + ", this build reads v" +
                  std::to_string(uf::state::kVersion) + " — restart uf-daemon from this tree";
            munmap((void*)s, sizeof *s);
            return false;
        }
        map = s;
        return true;
    }

    // Retry a torn read rather than showing a half-updated matrix. The publisher never blocks for
    // us, which is the whole point of the seqlock — and it now publishes meters at ~30 Hz, so torn
    // reads are ordinary rather than exceptional.
    bool refresh() {
        if (!attach()) return false;
        for (int i = 0; i < 16; ++i)
            if (uf::state::read(map, &snap)) return true;
        return false;
    }

    // The LOGICAL gain and flags, not the rendered cell. A muted crosspoint renders to 0, so reading
    // the matrix alone could not tell "muted at -6 dB" from "faded to nothing".
    uint32_t gain(Row r, int src, int dest) const {
        const int i = src * kCh + dest;
        return r == Row::Playback ? snap.playbackGain[i] : snap.inputGain[i];
    }
    uint8_t flags(Row r, int src, int dest) const {
        const int i = src * kCh + dest;
        return r == Row::Playback ? snap.playbackFlags[i] : snap.inputFlags[i];
    }
    bool stereo(Row r, int ch) const {
        const uint32_t b = (r == Row::Playback) ? snap.stereoPb : snap.stereoIn;
        return (b >> (ch / 2)) & 1u;
    }
};

std::string coeff_label(uint32_t gain, uint8_t flags) {
    if (flags & uf::kMfMuted) return " MUTE";
    if (gain == 0) return "  ·  ";
    if (gain == uf::kMixerUnity) return (flags & uf::kMfInverted) ? " ø0.0" : "  0  ";
    const double db = 20.0 * std::log10((double)gain / 32768.0);
    char buf[12];
    // An inverted crosspoint spends its sign column on the phase marker instead. A routing that is
    // silently out of phase is the kind of thing you stare at for an hour, and the '+' it displaces
    // is redundant — every gain here is at or below +6 dB and the number carries its own minus.
    if (flags & uf::kMfInverted) std::snprintf(buf, sizeof buf, "ø%4.0f", db);
    else                         std::snprintf(buf, sizeof buf, "%+5.0f", db);
    return buf;
}

// A five-character bar. Peak is what lights it, RMS fills it — the same split the manual describes
// (a yellow zero-attack line over a green averaged bar), reduced to what a terminal cell can carry.
Element meter_bar(const uf::MeterValue& m) {
    // -60 dB at the left, 0 dBFS at the right. Linear in dB, because linear in amplitude puts
    // everything you actually listen to in the last two characters.
    auto pos = [](float norm) {
        const float db = uf::meter_db(norm);
        const float t = (db + 60.0f) / 60.0f;
        return std::clamp((int)(t * 5.0f + 0.5f), 0, 5);
    };
    const int rms = pos(m.rms), peak = pos(m.peak);
    std::string s;
    for (int i = 0; i < 5; ++i) s += (i < rms) ? "=" : (i + 1 == peak ? "|" : " ");
    auto e = text(s);
    if (m.peak >= 0.999f) return e | color(Color::Red);       // over
    if (peak >= 5) return e | color(Color::Yellow);
    return e | color(Color::Green);
}

}  // namespace

int main() {
    StateReader st;
    uf::ctl::Client cli;
    std::string connErr;
    bool connected = cli.connect(&connErr, /*clientKind=*/2 /*TUI*/);

    const auto inNames  = names(uf::Direction::Capture);    // slots 9/10 are Mic here
    const auto outNames = names(uf::Direction::Playback);   // ...and Phones here

    // The manual's three rows as three TABS, on 1/2/3.
    //
    // Stacking them made one 57-row list, which scrolled on any normal terminal and put the output
    // controls somewhere you had to go looking for. Each group is at most 28 rows, so a tab fits a
    // screen and the mixer stops moving under you.
    //
    // `selOut` is deliberately SHARED across all three: it is the submix being edited. Pick an
    // output on tab 3, switch to tab 1, and you are editing that output's monitor mix — which is
    // TotalMix's Submix view (§25.5.1) expressed as navigation rather than as a mode.
    //
    // The two source tabs keep their own cursor, so switching back returns to where you were rather
    // than to the top.
    Row row = Row::Input;                 // which tab
    int selIn = 0, selPb = 0, selOut = 0;
    auto selSrcRef = [&]() -> int& { return row == Row::Playback ? selPb : selIn; };
    int selSrc = 0;                       // refreshed from selSrcRef() on every pass
    bool submixView = true;    // dim everything but the selected output, as TotalMix does by default
    std::string message = connected ? "" : ("not connected: " + connErr);

    auto screen = ScreenInteractive::Fullscreen();

    // Sending is synchronous on the UI thread, which is fine here: the daemon's socket thread
    // accepts immediately and the pump applies later, so nothing blocks for long.
    auto send = [&](auto&& fn, const char* what) {
        if (!connected) { message = "not connected: " + connErr; return; }
        if (!fn()) { connected = false; message = "daemon went away"; return; }
        message = what;
    };

    auto kind = [&] {
        return row == Row::Playback ? uf::MixerSrcKind::Playback : uf::MixerSrcKind::Input;
    };

    // Optimistic echo of the last gain we sent for the cell under the cursor.
    //
    // Stepping reads the DAEMON's published value, which is the right source of truth but lags by
    // up to a publish interval. A held key repeats far faster than that, so three presses all read
    // the same starting value and the gain moved one step instead of three — it felt stuck. So the
    // value we just sent is used as the base until the daemon's own value agrees with it.
    struct Pending { bool valid = false; Row row{}; int src = 0, out = 0; uint32_t coeff = 0; };
    Pending pending;

    // Set a crosspoint gain, or an output fader when the cursor is on the output row.
    auto set_gain = [&](uint32_t coeff) {
        pending = {true, row, selSrc, selOut, coeff};
        if (row == Row::Output)
            send([&] { return cli.set_fader((uint16_t)selOut, coeff); }, "fader set");
        else
            send([&] { return cli.set_mixer(kind(), (uint16_t)selSrc, (uint16_t)selOut, coeff); },
                 "gain set");
    };

    // Toggle one flag bit, reading its current value out of the published state. Mask + value means
    // we only ever speak for the bit we are toggling, so this cannot clobber another client's change
    // to a different flag.
    auto toggle_flag = [&](uint8_t bit, const char* what) {
        if (row == Row::Output) {
            const bool on = (st.snap.outputFlags[selOut] & bit) != 0;
            send([&] { return cli.set_out_flags((uint16_t)selOut, bit, on ? 0 : bit); }, what);
        } else {
            if (bit == uf::kMfRec) { message = "loopback is an output setting"; return; }
            const bool on = (st.flags(row, selSrc, selOut) & bit) != 0;
            send([&] { return cli.set_cell_flags(kind(), (uint16_t)selSrc, (uint16_t)selOut,
                                                 bit, on ? 0 : bit); }, what);
        }
    };

    int copyFrom = -1;

    auto renderer = Renderer([&] {
        selSrc = selSrcRef();
        const bool live = st.refresh();
        // Drop the optimistic value once the daemon has caught up, or the cursor has moved off the
        // cell it belonged to — after that the published state is authoritative again, which is what
        // lets another client's change to this cell be picked up.
        if (pending.valid) {
            const bool sameCell = pending.row == row && pending.src == selSrc && pending.out == selOut;
            const uint32_t pub = row == Row::Output ? st.snap.outputGain[selOut]
                                                    : st.gain(row, selSrc, selOut);
            if (!sameCell || (live && pub == pending.coeff)) pending.valid = false;
        }

        std::string hdr;
        if (!live) {
            hdr = st.err;
        } else if (!st.snap.deviceRate) {
            hdr = "daemon up, not streaming (no device?)";
        } else {
            hdr = std::to_string(st.snap.deviceRate) + " Hz   clock " +
                  uf::ctl::clock_source_name((uf::ctl::ClockSource)st.snap.clockSource) +
                  (st.snap.clockLocked ? " (locked)" : " (NOT locked)") +
                  "   " + std::to_string(st.snap.dbq) + " ch";
        }

        auto chip = [&](const char* label, Row t) {
            auto e = text(std::string("  ") + label + "  ");
            return (t == row) ? (e | inverted | bold) : (e | dim);
        };
        // The selected output is named in the tab bar because it is shared state: on tabs 1 and 2 it
        // is the column being edited, and it is the only thing tab 3 selects.
        // Submix view has to SHOW that it is on. It is only a dim attribute on the columns you are
        // not editing, which on some terminal themes is barely a change — so pressing 'v' and
        // seeing nothing obvious was indistinguishable from it not working. It gets an indicator in
        // the same visual language as the tabs, beside the submix it applies to.
        auto vchip = text(" v focus ");
        Element tabs = hbox({chip("1 inputs", Row::Input), text(" "),
                             chip("2 playback", Row::Playback), text(" "),
                             chip("3 outputs", Row::Output),
                             filler(),
                             text("submix: " + outNames[selOut] + "  ") | bold,
                             submixView ? (vchip | inverted) : (vchip | dim)});

        Element page;
        if (row == Row::Output) {
            // Tab 3 is a LIST of outputs, not the single fader row it was when everything shared one
            // scroll. A whole tab has room to say what each output is doing — level, mute, loopback,
            // and how many sources feed it, which is the quickest way to find the submix you meant.
            //
            // No meter column: the hardware outputs are summed by the FF800's own DSP and never come
            // back to us, so there is nothing honest to draw (spec/10 §10.3b).
            Elements rows;
            for (int o = 0; o < kCh; ++o) {
                const uint32_t g = live ? st.snap.outputGain[o] : 0;
                const uint8_t f  = live ? st.snap.outputFlags[o] : 0;
                int feeds = 0;
                if (live)
                    for (int sc = 0; sc < kCh; ++sc) {
                        if (st.gain(Row::Input, sc, o) && !(st.flags(Row::Input, sc, o) & uf::kMfMuted))
                            ++feeds;
                        if (st.gain(Row::Playback, sc, o) && !(st.flags(Row::Playback, sc, o) & uf::kMfMuted))
                            ++feeds;
                    }
                std::string nm = outNames[o];
                nm.resize(11, ' ');
                char lvl[16];
                std::snprintf(lvl, sizeof lvl, "%7s", coeff_label(g, f).c_str());
                Elements cells{text(nm), text(lvl)};
                cells.push_back((f & uf::kMfMuted) ? text("  MUTE") | color(Color::Blue)
                                                   : text("      "));
                cells.push_back((f & uf::kMfRec) ? text("  LOOPBACK") | color(Color::Magenta)
                                                 : text("          "));
                char fd[24];
                std::snprintf(fd, sizeof fd, "   %2d source%s", feeds, feeds == 1 ? "" : "s");
                cells.push_back(feeds ? text(fd) | color(Color::Green) : text("           ") | dim);
                auto line = hbox(std::move(cells));
                if (o == selOut) line = line | inverted | focus;
                rows.push_back(line);
            }
            page = vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
        } else {
            // Tabs 1 and 2 are the Matrix: sources down the side, hardware outputs across the top,
            // a gain at each crosspoint (§26.1).
            //
            // Two columns scrolled independently. Something must be FOCUSED or ftxui's frame never
            // scrolls at all; and the labels have to stay put, because at 28 columns scrolling right
            // would leave a grid of anonymous numbers. TotalMix has the same problem and the same
            // answer (§26.2: the labels are floating). The header and the grid are separate xframes
            // that stay in step because both carry a focus on the selected output column.
            constexpr int kGutter = 16;      // 9 label + 5 meter + a space, plus slack
            Elements gutter, grid;
            const auto& nm = (row == Row::Playback) ? outNames : inNames;
            for (int sc = 0; sc < kCh; ++sc) {
                const bool onRow = (sc == selSrc);
                // 9, because "Analog 10" is the longest name RME uses. Clipping to 7 silently turned
                // every "Analog N" into "Analog " — the number, which is the only part that
                // distinguishes them, was the bit that got cut.
                std::string rl = nm[sc];
                rl.resize(9, ' ');
                // A paired channel is marked on its odd half, so the pair reads as one thing.
                if (live && st.stereo(row, sc)) rl[8] = (sc & 1) ? ']' : '[';
                auto lbl = text(rl);
                Element g = hbox({onRow ? (lbl | inverted) : lbl,
                                  live ? meter_bar(row == Row::Playback ? st.snap.playbackMeters[sc]
                                                                        : st.snap.inputMeters[sc])
                                       : text("     "),
                                  text(" ")});
                if (onRow) g = g | focus;              // the gutter carries the vertical focus

                Elements cells;
                for (int o = 0; o < kCh; ++o) {
                    const uint32_t gn = live ? st.gain(row, sc, o) : 0;
                    const uint8_t f   = live ? st.flags(row, sc, o) : 0;
                    auto e = text(coeff_label(gn, f));
                    if (f & uf::kMfMuted)         e = e | color(Color::Blue);
                    else if (f & uf::kMfInverted) e = e | color(Color::Red);
                    else if (gn)                  e = e | color(Color::Green);
                    if (submixView && o != selOut) e = e | dim;
                    if (onRow && o == selOut) e = e | inverted | focus;   // horizontal focus
                    cells.push_back(e);
                }
                gutter.push_back(std::move(g));
                grid.push_back(hbox(std::move(cells)));
            }

            Elements head;
            for (int o = 0; o < kCh; ++o) {
                const std::string sn = short_name(outNames[o]);
                auto e = text(std::string(5 - std::min<size_t>(4, sn.size()), ' ') + sn.substr(0, 4));
                head.push_back(o == selOut ? (e | inverted | focus) : e);
            }

            page = vbox({
                hbox({text("src") | bold | size(WIDTH, EQUAL, kGutter),
                      hbox(std::move(head)) | xframe | flex}),
                hbox({vbox(std::move(gutter)) | size(WIDTH, EQUAL, kGutter),
                      vbox(std::move(grid)) | xframe | flex})
                    | vscroll_indicator | yframe | flex,
            }) | flex;
        }

        // What the cursor is on, spelled out — the grid is abbreviated by necessity.
        std::string where;
        if (row == Row::Output) where = "output " + outNames[selOut];
        else if (row == Row::Playback)
            where = "playback " + outNames[selSrc] + "  ->  " + outNames[selOut];
        else
            where = inNames[selSrc] + "  ->  " + outNames[selOut];

        return vbox({
            text("userface800 — TotalMix") | bold | center,
            text(hdr) | center,
            tabs,
            separator(),
            page,
            separator(),
            text(where + (message.empty() ? "" : "     " + message)),
            text("1/2/3 or tab switch   arrows move   pgup/pgdn page   home/end ends   q quit") | dim,
            text(", . gain +/-1 dB   u or 0 unity   m mute   p phase   l loopback   s stereo   "
                 "c copy submix   x clear   v submix view") | dim,
        }) | border;
    });

    auto app = CatchEvent(renderer, [&](Event e) {
        selSrc = selSrcRef();
        // On tabs 1 and 2 the cursor walks the SOURCES; on tab 3 it walks the outputs, which is the
        // same thing the left/right keys do everywhere — tab 3 exists to pick a submix.
        auto move = [&](int delta) {
            if (row == Row::Output) selOut = std::clamp(selOut + delta, 0, kCh - 1);
            else                    selSrcRef() = std::clamp(selSrc + delta, 0, kCh - 1);
            selSrc = selSrcRef();
        };
        auto go = [&](Row t) { row = t; selSrc = selSrcRef(); };

        if (e == Event::Character('q') || e == Event::Escape) { screen.ExitLoopClosure()(); return true; }

        if (e == Event::Character('1')) { go(Row::Input);    return true; }
        if (e == Event::Character('2')) { go(Row::Playback); return true; }
        if (e == Event::Character('3')) { go(Row::Output);   return true; }
        if (e == Event::Tab) {
            go(row == Row::Input ? Row::Playback : (row == Row::Playback ? Row::Output : Row::Input));
            return true;
        }

        if (e == Event::ArrowUp)    { move(-1);  return true; }
        if (e == Event::ArrowDown)  { move(1);   return true; }
        if (e == Event::PageUp)     { move(-12); return true; }
        if (e == Event::PageDown)   { move(12);  return true; }
        if (e == Event::Home)       { move(-kCh); return true; }
        if (e == Event::End)        { move(kCh);  return true; }

        // Left/right always move the submix, on every tab. On tab 3 that duplicates up/down, which
        // is fine: it means the key you reach for works wherever you are.
        if (e == Event::ArrowLeft)  { selOut = std::max(0, selOut - 1); return true; }
        if (e == Event::ArrowRight) { selOut = std::min(kCh - 1, selOut + 1); return true; }

        // '0' as well as 'u': the value it sets is 0 dB, and 1/2/3 being the tabs leaves 0 free —
        // so the digit row reads as "tabs, and the one that means unity".
        if (e == Event::Character('u') || e == Event::Character('0')) {
            set_gain(uf::kMixerUnity);
            return true;
        }
        if (e == Event::Character('m')) { toggle_flag(uf::kMfMuted, "mute toggled"); return true; }
        if (e == Event::Character('p')) { toggle_flag(uf::kMfInverted, "phase toggled"); return true; }
        if (e == Event::Character('l')) { toggle_flag(uf::kMfRec, "loopback toggled"); return true; }
        if (e == Event::Character('v')) {
            submixView = !submixView;
            // ...and say so, because the indicator answers "what state am I in" but not "did my
            // keypress do anything", which is the question actually being asked at the moment of
            // pressing it.
            message = submixView ? "submix view: only the selected output is highlighted"
                                 : "submix view off: every output shown equally";
            return true;
        }

        if (e == Event::Character('s')) {
            if (row == Row::Output) { message = "hardware outputs are always stereo"; return true; }
            const bool on = st.stereo(row, selSrc);
            send([&] { return cli.set_stereo(kind(), (uint16_t)selSrc, !on); }, "stereo toggled");
            return true;
        }

        // Copy a submix: press c on the source column, then c again on the destination (§27.2).
        if (e == Event::Character('c')) {
            if (copyFrom < 0) { copyFrom = selOut; message = "copy from " + outNames[selOut] +
                                                             " — press c on the destination"; }
            else {
                const int from = copyFrom;
                copyFrom = -1;
                send([&] { return cli.copy_submix((uint16_t)from, (uint16_t)selOut); },
                     "submix copied");
            }
            return true;
        }
        if (e == Event::Character('x')) {
            send([&] { return cli.clear_submix((uint16_t)selOut); }, "submix cleared");
            return true;
        }

        if (e == Event::Character('+') || e == Event::Character('=') || e == Event::Character('.') ||
            e == Event::Character('-') || e == Event::Character('_') || e == Event::Character(',')) {
            const bool up = (e == Event::Character('+') || e == Event::Character('=') ||
                             e == Event::Character('.'));
            // Step from what the DAEMON currently has, not from a value we cached — it is the source
            // of truth and something else may have changed this cell.
            uint32_t cur = row == Row::Output ? st.snap.outputGain[selOut]
                                              : st.gain(row, selSrc, selOut);
            if (pending.valid && pending.row == row && pending.src == selSrc && pending.out == selOut)
                cur = pending.coeff;
            // Silence is not "-65 dB", it is off — so the ends of the range are handled as such.
            // Stepping UP from a silent crosspoint goes straight to unity, because otherwise
            // turning a route on would take sixty-five keypresses to become audible; stepping DOWN
            // off the bottom turns it off, so the two are inverses and a cell can be toggled from
            // the keyboard without reaching for mute.
            if (up && cur == 0)     { set_gain(uf::kMixerUnity); return true; }
            // Quantise to the 1 dB grid before stepping. Without it the dB round-trip drifts —
            // five steps down and five back up landed on 0x8001 rather than unity, which then
            // displays as "+0" instead of "0".
            const double db = std::round(20.0 * std::log10((double)cur / 32768.0));
            if (!up && db <= -65.0) { set_gain(uf::kMixerMute); return true; }
            set_gain(uf::db_to_coeff(std::clamp(db + (up ? 1.0 : -1.0), -65.0, 6.0)));
            return true;
        }
        return false;
    });

    // Redraw on a timer so the display follows changes made elsewhere (another client, a rate
    // change, the device locking) and so the meters move at all. 20 Hz against the daemon's ~30 Hz
    // publish: fast enough to look continuous, slow enough not to spin a core on a mixer nobody is
    // looking at.
    std::atomic<bool> run{true};
    std::thread ticker([&] {
        while (run.load()) {
            usleep(50 * 1000);
            screen.PostEvent(Event::Custom);
        }
    });

    screen.Loop(app);
    run.store(false);
    ticker.join();
    return 0;
}
