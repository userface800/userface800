#!/usr/bin/env bash
# install.sh — install (or remove) the uf-daemon LaunchAgent for the current user.
#
#   platform/launchd/install.sh            install + start
#   platform/launchd/install.sh uninstall  stop + remove
#   platform/launchd/install.sh status     is it loaded, is it running, last log lines
#
# No sudo: this is a per-user agent in ~/Library/LaunchAgents, and the daemon needs no privileges.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
label="com.userface800.daemon"
plist="$HOME/Library/LaunchAgents/$label.plist"
# ~/Library/Logs is where macOS expects per-user logs and where Console.app looks for them, so use
# that rather than inventing a directory in the home root. $HOME, never a literal path: this has to
# work for whoever installs it, not just whoever wrote it.
logdir="$HOME/Library/Logs/UserFace800"
daemon="$root/build/uf-daemon"
# launchctl in the modern (bootstrap/bootout) form needs the GUI domain of this user.
domain="gui/$(id -u)"

case "${1:-install}" in
uninstall)
    launchctl bootout "$domain/$label" 2>/dev/null || true
    rm -f "$plist"
    echo "removed $label"
    ;;

status)
    log="$logdir/uf-daemon.log"
    if launchctl print "$domain/$label" >/dev/null 2>&1; then
        launchctl print "$domain/$label" | grep -E '^\s*(state|pid|last exit code) ' || true
    else
        echo "agent: not loaded  (run '$0' to install)"
    fi
    pgrep -f "$daemon" >/dev/null && echo "process: running" || echo "process: not running"

    # A VERDICT, not raw log lines. The daemon is deliberately silent once it is happy, so "no recent
    # output" is the healthy case and reading the tail cannot distinguish that from a stall. Instead
    # find whichever state marker appears LAST and say what it means, in words, with its timestamp.
    if [ -f "$log" ]; then
        last_evt=$(grep -nE 'session up at|waiting for the FF800|device present but the session will not start|session started|RESTART \(' "$log" | tail -1)
        line=${last_evt#*:}
        case "$line" in
            *"will not start"*)
                echo "STATE: *** DEVICE WEDGED *** — power-cycle the FF800."
                echo "       It is retrying every 5 s and will recover on its own once you do." ;;
            *"waiting for the FF800"*)
                # "waiting" has three quite different causes and only one of them is a cable. The
                # daemon cannot tell them apart — with no adapter attached the dext has nothing to
                # match, so an uninstalled dext and an unplugged adapter look identical to it. Here
                # we can ask the system directly, which matters most on a FIRST RUN: "plug it in" is
                # useless advice when the real answer is "the driver was never loaded".
                if ! systemextensionsctl list 2>/dev/null \
                     | grep -q 'com.userface800.UFLoader.UFFireWireOHCI.*activated enabled'; then
                    echo "STATE: THE DEXT IS NOT LOADED — a setup problem, not a cable."
                    echo "       bash platform/loader/build-and-sign.sh"
                    echo "       /Applications/UFLoader.app/Contents/MacOS/UFLoader"
                    echo "       It also needs the security prerequisites in platform/loader/LOADING.md."
                elif ! system_profiler SPThunderboltDataType 2>/dev/null | grep -q 'FireWire Adapter'; then
                    echo "STATE: dext loaded, but NO THUNDERBOLT->FIREWIRE ADAPTER is attached."
                    echo "       Attach the adapter; the device is picked up within ~2 s."
                else
                    echo "STATE: adapter present, but the FF800 is not answering."
                    echo "       Check it is powered on and the FireWire cable is seated."
                fi ;;
            *"session up at"*|*"session started"*)
                echo "STATE: streaming. $(echo "$line" | sed 's/.*uf-daemon: //')" ;;
            *"RESTART ("*)
                echo "STATE: mid-restart (rate change or recovery) — check again in a few seconds." ;;
            *)  echo "STATE: unknown — see the log." ;;
        esac
        echo "       last event: $(echo "$line" | cut -c1-100)"
        echo "       log:        $log"
    else
        echo "STATE: no log yet at $log"
    fi
    ;;

install)
    [ -x "$daemon" ] || { echo "!! $daemon not built — run: cmake --build build"; exit 1; }
    mkdir -p "$HOME/Library/LaunchAgents" "$logdir"

    sed -e "s|@UF_DAEMON@|$daemon|g" -e "s|@UF_LOGDIR@|$logdir|g" \
        "$here/$label.plist.in" > "$plist"

    # bootout first so re-running this picks up a changed path or binary rather than silently
    # leaving the old definition loaded.
    # bootout RETURNS BEFORE THE JOB IS GONE, and bootstrapping a label that is still registered
    # fails with the distinctly unhelpful "Bootstrap failed: 5: Input/output error". Wait it out.
    launchctl bootout "$domain/$label" 2>/dev/null || true
    for _ in $(seq 1 50); do
        launchctl print "$domain/$label" >/dev/null 2>&1 || break
        sleep 0.2
    done
    # enable BEFORE bootstrap: a label on launchd's disabled list is refused outright, and enabling
    # it afterwards is too late.
    launchctl enable "$domain/$label"
    launchctl bootstrap "$domain" "$plist"

    echo "installed $plist"
    echo "  binary : $daemon"
    echo "  log    : $logdir/uf-daemon.log"
    echo
    echo "It starts at login and restarts if it exits (10 s throttle), so with no FF800 attached it"
    echo "will retry every 10 s — that is intended, and how it picks the device up when you plug in."
    echo
    echo "  status:    $0 status"
    echo "  stop:      launchctl bootout $domain/$label"
    echo "  uninstall: $0 uninstall"
    ;;
*)
    echo "usage: $0 [install|uninstall|status]"; exit 2 ;;
esac
