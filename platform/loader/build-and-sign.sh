#!/usr/bin/env bash
# build-and-sign.sh — build the OHCI dext + the UFLoader app, embed + ad-hoc-sign, install to
# /Applications, and run it from there.
#
# sysextd will NOT activate a dext whose host app lives outside /Applications ("no policy, cannot
# allow apps outside /Applications" in the log, surfaced to the app as the misleading
# OSSystemExtensionError code 4 "Extension not found in App bundle"). So the app is staged in
# ./build, then installed — and the staged copy is dropped from the LaunchServices database, or LS
# can still resolve com.userface800.UFLoader to the build dir and sysextd rejects it again.
#
# Other prereqs to actually LOAD (see LOADING.md): Reduced/Permissive Security, SIP off,
# `systemextensionsctl developer on`, and NO `amfi_get_out_of_my_way` boot-arg (it prevents the
# dext from executing).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dext_dir="$here/../ohci-dext"
out="$here/build"
app="$out/UFLoader.app"
installed="/Applications/UFLoader.app"
dext_id="com.userface800.UFLoader.UFFireWireOHCI"   # must stay prefixed by the app's id (com.userface800.UFLoader)
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
rm -rf "$out"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Library/SystemExtensions"

echo "== 1/5  build the dext =="
xcodebuild -project "$dext_dir/UFFireWireOHCI.xcodeproj" -scheme UFFireWireOHCI \
    -configuration Debug -derivedDataPath "$out/dd" \
    build CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO >/dev/null
dext="$out/dd/Build/Products/Debug-driverkit/UFFireWireOHCI.dext"
# sysextd only finds an extension whose bundle DIRECTORY is named after its CFBundleIdentifier —
# xcodebuild names it after the target, so rename on the way in. (Getting this wrong is the other
# source of "Extension not found in App bundle".)
cp -R "$dext" "$app/Contents/Library/SystemExtensions/$dext_id.dext"

echo "== 2/5  compile the loader (Swift) =="
xcrun --sdk macosx swiftc -O "$here/main.swift" -o "$app/Contents/MacOS/UFLoader" \
    -framework SystemExtensions -framework IOKit -framework Foundation
cp "$here/UFLoader-Info.plist" "$app/Contents/Info.plist"

echo "== 3/5  ad-hoc sign the dext then the app =="
# The dext gets its own (restricted) entitlements; the app gets the system-extension.install one.
codesign --force --sign - --entitlements "$dext_dir/UFFireWireOHCI.entitlements" \
    --timestamp=none "$app/Contents/Library/SystemExtensions/$dext_id.dext"
codesign --force --sign - --entitlements "$here/UFLoader.entitlements" \
    --timestamp=none "$app"

echo "== 4/5  install to /Applications =="
rm -rf "$installed"
cp -R "$app" /Applications/
"$lsregister" -u "$app" 2>/dev/null || true
"$lsregister" -f "$installed"
codesign --verify --strict "$installed"

echo "== 5/5  done =="
echo "Installed: $installed"
echo "Run it (after the LOADING.md security steps):  \"$installed/Contents/MacOS/UFLoader\""
