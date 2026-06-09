#!/bin/bash

if [[ $# -eq 0 || "$1" != "vatican-cameos" ]]; then
    echo "This script clobbers the parent directory. It's mostly meant to be used from inside a GitHub workflow. If you're sure you know what you're doing, run it with a 'vatican-cameos' parameter." >&2
    exit 1
fi

set -x -e -u -o pipefail

west init -l .
west update -o=--depth=1 -n

mkdir artifacts

west build -d build-pico -b rpi_pico app
cp build-pico/zephyr/zephyr.uf2 artifacts/dongle-pico.uf2

west build -d build-pico2 -b rpi_pico2/rp2350a/m33 app
cp build-pico2/zephyr/zephyr.uf2 artifacts/dongle-pico2.uf2
