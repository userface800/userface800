// txnlog.hpp — register-transaction formatting in the spec/09 notation (portable).
//
// The key hardware-debug aid: render each UFAsyncIO op as text so a captured session can be
// diffed against the spec/09 worked-example sequences (`wq/rq/wb`). The formatting is pure logic and
// lives here; the host wraps UFAsyncIO to emit these strings behind a debug flag (zero-cost when
// off). Reuses the RegWrite planner records (control.hpp) so a planned sequence
// (e.g. UFControl::full_rewrite, stream_start_writes) formats identically to what the device sees.
#pragma once
#include <string>
#include <vector>
#include "uf/protocol/control.hpp"
#include "uf/protocol/endian.hpp"

namespace uf {

namespace detail {
// 0x-prefixed hex, `width` nibbles (matches spec/09: addresses bare, quadlets 8-wide).
inline std::string hex(u64 v, int width) {
    static const char* d = "0123456789abcdef";
    std::string s(static_cast<size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i) { s[static_cast<size_t>(i)] = d[v & 0xf]; v >>= 4; }
    return "0x" + s;
}
}  // namespace detail

inline std::string fmt_addr(Addr a) { return detail::hex(a, a > 0xffffffffull ? 9 : 8); }
inline std::string fmt_quad(u32 v) { return detail::hex(v, 8); }

// A write-quadlet in spec/09 notation: `wq(<addr>, <val>)`.
inline std::string fmt_wq(Addr a, u32 v) { return "wq(" + fmt_addr(a) + ", " + fmt_quad(v) + ")"; }

// A read-quadlet with its result: `rq(<addr>) -> <val>`.
inline std::string fmt_rq(Addr a, u32 v) { return "rq(" + fmt_addr(a) + ") -> " + fmt_quad(v); }

// A write-block: `wb(<addr>, [<q0>, <q1>, ...])`.
inline std::string fmt_wb(Addr a, const std::vector<u32>& quads) {
    std::string s = "wb(" + fmt_addr(a) + ", [";
    for (size_t i = 0; i < quads.size(); ++i) { if (i) s += ", "; s += fmt_quad(quads[i]); }
    return s + "])";
}

// Format a planned RegWrite (control.hpp) — a quadlet becomes wq(), a block becomes wb().
inline std::string fmt_regwrite(const RegWrite& w) {
    return w.kind == RegWrite::Kind::Quadlet ? fmt_wq(w.addr, w.quad) : fmt_wb(w.addr, w.quads);
}

// Format a whole planned sequence, one transaction per line.
inline std::string fmt_sequence(const std::vector<RegWrite>& writes) {
    std::string s;
    for (const auto& w : writes) { s += fmt_regwrite(w); s += '\n'; }
    return s;
}

}  // namespace uf
