#!/usr/bin/env bash
# Fetch the read-only GPL reference drivers we port/adapt from (see COPYRIGHT).
# These are NOT versioned in this repo (.gitignore /ref/).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ref="$here/ref"
mkdir -p "$ref"

# FFADO 2.4.9 (GPL) — RME userspace driver; best reference for settings + mixer facts.
if [ ! -d "$ref/libffado-2.4.9" ]; then
  echo "Fetching FFADO 2.4.9 ..."
  curl -fsSL https://ffado.org/files/libffado-2.4.9.tgz | tar xz -C "$ref"
fi

# snd-fireface (GPL-2.0, mainline Linux kernel) — the PRIMARY reference: a complete
# working FF800 driver (PCM audio + MIDI + clock). We fetch just the driver subtree.
sf="$ref/snd-fireface"
if [ ! -d "$sf" ]; then
  echo "Fetching snd-fireface ..."
  mkdir -p "$sf/firewire-core"
  ff="https://raw.githubusercontent.com/torvalds/linux/master/sound/firewire/fireface"
  core="https://raw.githubusercontent.com/torvalds/linux/master/sound/firewire"
  for f in Makefile ff.h ff.c ff-proc.c amdtp-ff.c ff-midi.c ff-pcm.c ff-stream.c \
           ff-transaction.c ff-hwdep.c ff-protocol-former.c ff-protocol-latter.c; do
    curl -fsSL -o "$sf/$f" "$ff/$f"
  done
  for f in amdtp-stream.c amdtp-stream.h amdtp-stream-trace.h iso-resources.c \
           iso-resources.h packets-buffer.c packets-buffer.h lib.c lib.h Kconfig; do
    curl -fsSL -o "$sf/firewire-core/$f" "$core/$f"
  done
fi

# firewire-ohci (GPL-2.0, mainline Linux) — the PRIMARY reference for the userland OHCI transport
# (PCIDriverKit dext): controller init, self-ID/bus-reset, async (AT/AR) + isoch (IT/IR) DMA
# contexts, and the 1394 packet-header/phy-packet field layouts. We port this into platform/ohci-dext.
fo="$ref/firewire-ohci"
if [ ! -d "$fo" ]; then
  echo "Fetching firewire-ohci (Linux drivers/firewire) ..."
  mkdir -p "$fo"
  fw="https://raw.githubusercontent.com/torvalds/linux/master/drivers/firewire"
  inc="https://raw.githubusercontent.com/torvalds/linux/master/include/linux"
  uapi="https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux"
  for f in ohci.c ohci.h core.h core-transaction.c core-topology.c core-card.c core-iso.c \
           packet-header-definitions.h phy-packet-definitions.h packet-serdes-test.c; do
    curl -fsSL -o "$fo/$f" "$fw/$f" || echo "  (skip $f)"
  done
  curl -fsSL -o "$fo/firewire.h" "$inc/firewire.h" || true
  curl -fsSL -o "$fo/firewire-constants.h" "$uapi/firewire-constants.h" || true
fi

echo "References ready in $ref:"; ls "$ref"
