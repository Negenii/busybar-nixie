#!/bin/sh
# Build Nixie.fap against VeryBUSY.
#
# Clones the fork at the pinned commit (or uses VERYBUSY_DIR), applies the
# time-service export the app needs, drops the app into applications/external,
# and builds the FAPs. The result lands in dist/Nixie.fap.
#
# Needs: git, python3, curl, tar (the fork fetches its own toolchain).
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PIN="5394ce1f573e88e0d85b2d363e7a29e42691e62a"   # VeryBUSY main at r7
DIR="${VERYBUSY_DIR:-$HERE/build/VeryBUSY}"

if [ ! -d "$DIR/.git" ]; then
    mkdir -p "$(dirname "$DIR")"
    git clone --recursive https://code.mccullough.dev/christian/VeryBUSY.git "$DIR"
fi
cd "$DIR"
git fetch -q origin
git checkout -q "$PIN"
git submodule update --init --recursive -q

# The export, unless the fork already carries it.
if ! grep -q "time_get_local_time" targets/f21/api_symbols.csv || grep -q "^Function,-,time_get_local_time" targets/f21/api_symbols.csv; then
    git apply --check "$HERE/firmware/0001-export-the-time-service-to-native-apps.patch"
    git apply "$HERE/firmware/0001-export-the-time-service-to-native-apps.patch"
fi

rm -rf applications/external/nixie
mkdir -p applications/external/nixie
cp -R "$HERE/app/." applications/external/nixie/

python3 scripts/apply_custom_patches.py
./fbt TARGET_HW=22 COMPACT=1 FIRMWARE_ORIGIN=VeryBUSY INTERCOM_FORCE_VERSION=b315346d
./fbt TARGET_HW=22 COMPACT=1 FIRMWARE_ORIGIN=VeryBUSY INTERCOM_FORCE_VERSION=b315346d faps

mkdir -p "$HERE/dist"
cp fbt_layers/fbtng/build/f22-firmware-DC/.extapps/Nixie.fap "$HERE/dist/Nixie.fap"
echo "built: $HERE/dist/Nixie.fap"
