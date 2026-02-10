#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

fail() {
    echo "FAIL: $1"
    exit 1
}

require_file() {
    [ -f "$1" ] || fail "missing required file: $1"
}

require_contains() {
    pattern="$1"
    file="$2"
    if ! grep -En "$pattern" "$file" >/dev/null 2>&1; then
        fail "expected pattern '$pattern' in $file"
    fi
}

require_not_contains() {
    pattern="$1"
    file="$2"
    if grep -En "$pattern" "$file" >/dev/null 2>&1; then
        fail "unexpected pattern '$pattern' found in $file"
    fi
}

cd "$ROOT_DIR"

require_file "meson.build"
require_file "src/vabackend.h"
require_file "subprojects/ff-nvcodec-headers.wrap"

require_contains "dependency\\('ffnvcodec'" "meson.build"
require_contains "#include <ffnvcodec/nvEncodeAPI.h>" "src/vabackend.h"
require_not_contains "Video_Codec_SDK|Video_Codec_Interface" "meson.build"
require_not_contains "Video_Codec_SDK|Video_Codec_Interface" "src/vabackend.h"

echo "PASS: CI sanity checks"
