#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/grovedeck.c on this computer (tests/host.c): build/host/grovedeck.wav and the screens as PNG.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -Ifirmware/src -o build/host/grovedeck firmware/src/grovedeck.c
build/host/grovedeck build/host/grovedeck "$@"
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob("grovedeck-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
