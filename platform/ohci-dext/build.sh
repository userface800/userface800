#!/usr/bin/env bash
# build.sh — compile-check the UFFireWireOHCI dext against the DriverKit SDK.
#
# This validates the code + toolchain. It does NOT produce a *loadable* dext: loading needs
# (1) the Apple-managed `com.apple.developer.driverkit.transport.pci` entitlement, (2) a real
# code-signing identity, (3) `systemextensionsctl developer on`, and normally an Xcode dext target
# to assemble the .dext bundle. Those are the next gate; this just proves the source compiles.
set -euo pipefail
cd "$(dirname "$0")"

DKSDK="$(xcrun --sdk driverkit --show-sdk-path)"
IIG="$(xcrun --sdk driverkit --find iig)"
CXX="$(xcrun --sdk driverkit --find clang++)"
OUT="build"
mkdir -p "$OUT"

echo "== iig: generate headers + dispatch from the .iig interface =="
"$IIG" \
    --def UFFireWireOHCI.iig \
    --header "$OUT/UFFireWireOHCI.h" \
    --impl "$OUT/UFFireWireOHCI.iig.cpp" \
    -- \
    -I"$DKSDK/System/DriverKit/System/Library/Frameworks/DriverKit.framework/Headers" \
    -I"$DKSDK/System/DriverKit/System/Library/Frameworks/PCIDriverKit.framework/Headers"

echo "== clang++: compile the driver + generated dispatch (no link) =="
COMMON_FLAGS=(-x c++ -std=gnu++17 -fno-exceptions -fno-rtti
    -isysroot "$DKSDK"
    -I"$OUT" -I.
    -I"$DKSDK/System/DriverKit/System/Library/Frameworks/DriverKit.framework/Headers"
    -I"$DKSDK/System/DriverKit/System/Library/Frameworks/PCIDriverKit.framework/Headers"
    -target arm64-apple-driverkit24.0)

"$CXX" "${COMMON_FLAGS[@]}" -c UFFireWireOHCI.cpp        -o "$OUT/UFFireWireOHCI.o"
"$CXX" "${COMMON_FLAGS[@]}" -c "$OUT/UFFireWireOHCI.iig.cpp" -o "$OUT/UFFireWireOHCI.iig.o"

echo "== OK: dext sources compile against DriverKit SDK =="
ls -la "$OUT"
