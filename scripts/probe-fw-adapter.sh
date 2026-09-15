#!/usr/bin/env bash
# probe-fw-adapter.sh — does the Thunderbolt→FireWire adapter's 1394 OHCI controller enumerate as a
# PCI device on this Mac? This is the hard gate for the userland-OHCI (PCIDriverKit) plan: a dext
# can only bind a device the OS enumerates as an IOPCIDevice. Plug in the TB→FW adapter (the FF800
# behind it is optional for this check) and run this. We want to see a PCI function with
# class-code 0x0C0010 (Serial Bus Controller / FireWire / OHCI) and capture its vendor/device IDs
# for the dext's IOPCIMatch dictionary.
set -u

echo "==================================================================="
echo " Thunderbolt bus (is the adapter seen at the TB layer at all?)"
echo "==================================================================="
system_profiler SPThunderboltDataType 2>/dev/null

echo
echo "==================================================================="
echo " PCI devices (IOPCIDevice) — vendor-id / device-id / class-code"
echo " Looking for a FireWire OHCI: class-code containing 0c 00 10."
echo "==================================================================="
ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | \
  grep -iE '"(IONameMatched|vendor-id|device-id|class-code|IOName|model|revision-id|pcidebug)"|IOPCIDevice|firewire|1394|ohci'

echo
echo "==================================================================="
echo " Anything FireWire/1394/OHCI anywhere in the IORegistry"
echo "==================================================================="
ioreg -l -w0 2>/dev/null | grep -iE 'firewire|1394|ohci' | head -40
echo "(empty above = the OS sees no FireWire/OHCI device — the userland-OHCI"
echo " plan is gated by Thunderbolt PCIe enumeration on this Mac.)"

echo
echo "Done. Key result: a PCI function with"
echo "class-code 0x0C0010 and its vendor-id/device-id → that's the dext target."
