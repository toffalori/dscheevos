#!/bin/sh
# Assemble the SD card folder for a DScheevos release from built parts.
#   package.sh OUT_DIR VERSION
# Expects, relative to the repo root:
#   dscheevos/launcher/dscheevos-launcher.nds
#   dscheevos/launcher/loader/out/load.bin
#   dscheevos/module/build/dscheevos9.bin
#   external/nds-bootstrap/bin/nds-bootstrap-nightly.nds
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
out=$1 version=$2
sd=$out/sd
rm -rf "$out"
mkdir -p "$sd/_nds/dscheevos/licenses"

cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-release.nds"
cp "$repo/dscheevos/launcher/dscheevos-launcher.nds" "$sd/_nds/nds-bootstrap-nightly.nds"
cp "$repo/external/nds-bootstrap/bin/nds-bootstrap-nightly.nds" "$sd/_nds/dscheevos/nds-bootstrap.nds"
cp "$repo/dscheevos/module/build/dscheevos9.bin" "$repo/dscheevos/launcher/loader/out/load.bin" "$sd/_nds/dscheevos/"
printf '%s\n' "$version" > "$sd/_nds/dscheevos/version.txt"

# Cheevotron streamer variant of the module (same launcher and nds-bootstrap ABI):
# a test card runs it by replacing dscheevos9.bin with this file.
if [ -f "$repo/dscheevos/module/build-stream/dscheevos9.bin" ]; then
  cp "$repo/dscheevos/module/build-stream/dscheevos9.bin" "$sd/_nds/dscheevos/dscheevos9-stream.bin"
fi

# Licenses of what the binaries contain
lic=$sd/_nds/dscheevos/licenses
cp "$repo/LICENSE" "$lic/dscheevos.txt"
cp "$repo/external/rcheevos/LICENSE" "$lic/rcheevos.txt"
cp "$repo/external/mbedtls/LICENSE" "$lic/mbedtls.txt"
cp "$repo/external/lwip/COPYING" "$lic/lwip.txt"
# nds-bootloader states GPL v2 or later in its source headers and ships no license file
# (release/GPL-2.txt is Debian's common-licenses/GPL-2, kept here so macOS builds the same zip)
{ echo "devkitPro nds-bootloader: GNU General Public License, version 2 or (at your option) any later version."; echo; cat "$repo/release/GPL-2.txt"; } > "$lic/nds-bootloader.txt"
cp "$repo/external/nds-bootstrap/LICENSE" "$lic/nds-bootstrap.txt"
# Toolkit runtime libraries linked into the binaries, vendored from the build images
# (release/licenses/, first line of each names the source) so packaging needs no network
cp "$repo"/release/licenses/*.txt "$lic/"
cat > "$lic/SOURCES.txt" <<SRC
DScheevos $version is built from:
  https://github.com/skierrz/dscheevos ($version)
  https://github.com/skierrz/nds-bootstrap (dscheevos branch, $(git -C "$repo/external/nds-bootstrap" rev-parse --short HEAD))
  https://github.com/devkitPro/nds-bootloader ($(git -C "$repo/external/nds-bootloader" rev-parse --short HEAD))
  https://github.com/RetroAchievements/rcheevos ($(git -C "$repo/external/rcheevos" rev-parse --short HEAD))
  https://github.com/lwip-tcpip/lwip ($(git -C "$repo/external/lwip" describe --tags 2>/dev/null || git -C "$repo/external/lwip" rev-parse --short HEAD))
  https://github.com/Mbed-TLS/mbedtls ($(git -C "$repo/external/mbedtls" describe --tags 2>/dev/null || git -C "$repo/external/mbedtls" rev-parse --short HEAD))
DScheevos itself is GPL-3.0 (dscheevos.txt).
Toolkit libraries linked into the binaries, licence texts in this folder:
  launcher (BlocksDS v1.24.0): libnds, DSWiFi (with lwIP), start-up code, picolibc
    blocksds-libnds.txt blocksds-dswifi.txt blocksds-crts.txt picolibc.txt
  module (devkitARM, devkitpro/devkitarm:20241104): newlib
    newlib.txt
  nds-bootstrap (same devkitARM): libnds 1.8, libfat, newlib
    devkitarm-libnds.txt libfat.txt newlib.txt
  load.bin (devkitARM, devkitpro/devkitarm:20260610): libnds 2, calico, newlib
    devkitarm-libnds2.txt calico.txt newlib.txt
SRC

cp "$repo/release/README.txt" "$out/README.txt"
cp "$repo/docs/privacy.md" "$out/privacy.txt"
cp "$repo/docs/privacy.md" "$sd/_nds/dscheevos/privacy.txt"
(cd "$out" && zip -qr "../dscheevos-$version.zip" README.txt privacy.txt sd)
echo "packed $(cd "$out/.." && pwd)/dscheevos-$version.zip"
