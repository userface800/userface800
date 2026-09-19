#!/usr/bin/env bash
# verify-mixer.sh — one pass through the mixer's hardware acceptance list.
#
# It batches every check that can be made automatic into a single run and stops to ask only where a
# human ear is genuinely the instrument. Run it with the device attached.
#
#   scripts/verify-mixer.sh            # everything
#   scripts/verify-mixer.sh --quick    # only the checks that need no device and no ears
#
# YOUR MIXER IS BACKED UP AND RESTORED. The checks set routes, mutes and loopback flags, so the
# state file is copied aside first and put back at the end, including on Ctrl-C. That matters
# because the state file IS your monitoring setup.
#
# What this can and cannot prove: the mixer RAM is WRITE-ONLY from our side, so nothing here reads a
# coefficient back off the device. The automated checks verify the daemon's model, its persistence,
# and that it re-applies at the right moments; that the device then sounds right is what the
# listening checks are for. Both halves are needed and neither substitutes for the other.
set -uo pipefail
cd "$(dirname "$0")/.."

MIX=./build/uf-mix
SET=./build/uf-set
STATUS=./build/uf-status
STATE="$HOME/Library/Application Support/UserFace800/state.bin"
BACKUP="$STATE.verify-backup"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

pass=0; fail=0; skip=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; pass=$((pass+1)); }
no()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=$((fail+1)); }
skipd(){ printf '  \033[33mSKIP\033[0m  %s\n' "$1"; skip=$((skip+1)); }
head2(){ printf '\n\033[1m%s\033[0m\n' "$1"; }

restore() {
    if [ -f "$BACKUP" ]; then
        printf '\nrestoring your mixer state...\n'
        # Stop the daemon FIRST: it saves on exit, and would otherwise write the test state back
        # over the file we just restored.
        #
        # WAIT FOR IT TO ACTUALLY DIE. `launchctl kill` returns as soon as the signal is delivered,
        # not when the process is gone, so a fixed sleep raced the daemon's exit-save: on one
        # run the save landed AFTER the mv and the test routes ended up merged back into
        # the user's file. Poll for the pid instead of guessing at a duration.
        launchctl kill TERM "gui/$(id -u)/com.userface800.daemon" 2>/dev/null
        for _ in $(seq 1 40); do
            pgrep -f 'build/uf-daemon' >/dev/null || break
            sleep 0.25
        done
        if pgrep -f 'build/uf-daemon' >/dev/null; then
            printf '  daemon did not exit — NOT restoring, your backup is at:\n    %s\n' "$BACKUP"
            return
        fi
        mv -f "$BACKUP" "$STATE"
        launchctl kickstart "gui/$(id -u)/com.userface800.daemon" >/dev/null 2>&1
        sleep 2
        printf 'restored:\n'
        $STATUS 2>/dev/null | sed -n '/mixer routing/,/^$/p'
    fi
}
trap restore EXIT INT TERM

# The daemon's own view of one crosspoint, as uf-status prints it: "input 9  -> out 1   0x08000  MUTED"
cell() { $STATUS 2>/dev/null | grep -E "^  input $1 +-> out $2 " | head -1; }
outrow() { $STATUS 2>/dev/null | grep -E "^  out $1 " | head -1; }

# ── preflight ────────────────────────────────────────────────────────────────────────────────────
head2 "preflight"
pgrep -f 'build/uf-daemon' >/dev/null || { echo "  uf-daemon is not running — start it first"; exit 1; }
echo "  daemon: running"

DEVICE=0
if $STATUS 2>/dev/null | grep -q 'Hz'; then DEVICE=1; fi
if [ $DEVICE = 1 ]; then
    echo "  device: streaming — $($STATUS 2>/dev/null | grep -oE '[0-9]+ Hz.*' | head -1)"
else
    echo "  device: NOT streaming (no adapter/FF800). Model checks still run; audible ones cannot."
fi

DEXT_META=0
if strings /Applications/UFLoader.app/Contents/Library/SystemExtensions/*.dext/UFFireWireOHCI 2>/dev/null \
   | grep -q read_block; then DEXT_META=1; fi
echo "  installed dext: $([ $DEXT_META = 1 ] && echo 'has ReadBlock' || echo 'PRE-ReadBlock (an older build) — device meter poll unavailable')"

cp -f "$STATE" "$BACKUP" 2>/dev/null && echo "  mixer state backed up"

# ── A. the property the whole model exists for ───────────────────────────────────────────────────
head2 "A. mute preserves the gain underneath it"
$MIX in 9 1 -6 >/dev/null; sleep 0.6
before=$(cell 9 1)
echo "    set -6 dB:      $before"
case "$before" in *0x04027*) ok "gain set to -6 dB (0x04027)";; *) no "expected 0x04027, got: $before";; esac

$MIX mute in 9 1 on >/dev/null; sleep 0.6
muted=$(cell 9 1)
echo "    muted:          $muted"
case "$muted" in
    *0x04027*MUTED*) ok "MUTED, and the -6 dB gain is still recorded — un-mute can restore it";;
    *MUTED*)         no "muted, but the gain underneath was LOST: $muted";;
    *)               no "mute did not take: $muted";;
esac

$MIX mute in 9 1 off >/dev/null; sleep 0.6
after=$(cell 9 1)
echo "    un-muted:       $after"
case "$after" in
    *0x04027*MUTED*) no "still muted: $after";;
    *0x04027*)       ok "restored to exactly -6 dB, not to unity and not to silence";;
    *)               no "un-mute did not restore the gain: $after";;
esac

# ── B. phase ─────────────────────────────────────────────────────────────────────────────────────
head2 "B. phase invert is a negative coefficient"
$MIX phase in 9 1 on >/dev/null; sleep 0.6
ph=$(cell 9 1)
echo "    inverted:       $ph"
case "$ph" in *PHASE-INV*) ok "flag recorded (the daemon renders it as a negative quadlet)";;
              *)           no "phase flag did not take: $ph";; esac
$MIX phase in 9 1 off >/dev/null; sleep 0.6

# ── C. loopback ──────────────────────────────────────────────────────────────────────────────────
head2 "C. loopback (output-record mask, 0x801c0080)"
$MIX loopback 5 on >/dev/null; sleep 0.6
lb=$(outrow 5)
echo "    out 5:          $lb"
case "$lb" in *LOOPBACK*) ok "loopback flag recorded for output 5";;
              *)          no "loopback did not take: $lb";; esac

# ── D. survival across a rate change ─────────────────────────────────────────────────────────────
head2 "D. a route AND its loopback survive a sample-rate change"
if [ $DEVICE = 0 ]; then
    skipd "no device — a rate change needs a live session"
else
    rate_before=$($STATUS 2>/dev/null | grep -oE '^[0-9]+ Hz' | head -1)
    target=96000; [ "${rate_before%% *}" = "96000" ] && target=48000
    echo "    $rate_before -> $target Hz ..."
    $SET --rate=$target --master >/dev/null 2>&1
    sleep 6
    r=$(cell 9 1); l=$(outrow 5)
    echo "    route:          $r"
    echo "    loopback:       $l"
    case "$r" in *0x04027*) ok "the -6 dB route survived the rate change";;
                 *)         no "route lost across the rate change: $r";; esac
    case "$l" in *LOOPBACK*) ok "loopback survived the rate change (re-applied at session start)";;
                 *)          no "loopback lost across the rate change: $l";; esac
fi

# ── E. persistence ───────────────────────────────────────────────────────────────────────────────
head2 "E. the model survives a daemon restart"
launchctl kickstart -k "gui/$(id -u)/com.userface800.daemon" >/dev/null 2>&1
sleep 4
r=$(cell 9 1); l=$(outrow 5)
echo "    route:          $r"
echo "    loopback:       $l"
case "$r" in *0x04027*) ok "route restored from disk";;
             *)         no "route lost across the restart: $r";; esac
case "$l" in *LOOPBACK*) ok "loopback restored from disk";;
             *)          no "loopback lost across the restart: $l";; esac

# ── F/G. what only ears and signal can settle ────────────────────────────────────────────────────
if [ $QUICK = 1 ]; then
    head2 "listening checks skipped (--quick)"
elif [ $DEVICE = 0 ]; then
    head2 "listening checks"
    skipd "no device attached"
    skipd "meters need signal"
else
    head2 "F. meters"
    echo "    Feed signal to an input, then watch tab 1 of:  ./build/uf-tui"
    echo "    Expect: the bar tracks the signal, red at full scale, and silence reads as nothing."
    read -r -p "    Do the meters look right? [y/N] " a
    case "$a" in [yY]*) ok "meters track signal";; *) no "meters wrong or not checked";; esac

    head2 "G. audible checks"
    echo "    1) uf-mix in 9 1        # input 9 to output 1 at unity — should be audible"
    read -r -p "    Audible? [y/N] " a
    case "$a" in [yY]*) ok "zero-latency monitoring audible";; *) no "not audible";; esac

    echo "    2) Route one source to BOTH halves of a pair, then phase-invert one:"
    echo "         ./build/uf-mix in 9 1 ; ./build/uf-mix in 9 2 ; ./build/uf-mix phase in 9 2 on"
    echo "       Summed to mono this should thin out or cancel."
    read -r -p "    Cancellation heard? [y/N] " a
    case "$a" in [yY]*) ok "phase invert is real";; *) no "no cancellation";; esac

    echo "    3) Loopback: with 'uf-mix loopback 5 on', record INPUT 5 in a DAW."
    echo "       You should capture output 5's MIX, not the physical input 5."
    read -r -p "    Loopback records the mix? [y/N] " a
    case "$a" in [yY]*) ok "loopback works end to end";; *) no "loopback not confirmed";; esac
fi

head2 "summary"
printf '  %d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ] || printf '  \033[31mfailures above — do not tick the acceptance list\033[0m\n'
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
