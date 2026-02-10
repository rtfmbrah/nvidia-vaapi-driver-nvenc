#!/bin/sh
set -eu

print_help() {
    cat <<'EOF'
Usage:
  ./tests/runtime-smoke.sh [--driver-path <path>] [--ffmpeg]

Options:
  --driver-path <path>   Set LIBVA_DRIVERS_PATH for local build testing.
  --ffmpeg               Also run a short h264_vaapi encode probe via ffmpeg.

Environment:
  LIBVA_DRIVER_NAME      Defaults to "nvidia".
  LIBVA_DRM_DEVICE       Optional DRM render node path (for example /dev/dri/renderD128).
EOF
}

DRIVER_PATH=""
RUN_FFMPEG=0

while [ $# -gt 0 ]; do
    case "$1" in
        --driver-path)
            [ $# -ge 2 ] || {
                echo "error: --driver-path requires a value"
                exit 2
            }
            DRIVER_PATH="$2"
            shift 2
            ;;
        --ffmpeg)
            RUN_FFMPEG=1
            shift
            ;;
        -h|--help|help)
            print_help
            exit 0
            ;;
        *)
            echo "error: unknown option '$1'"
            print_help
            exit 2
            ;;
    esac
done

command -v vainfo >/dev/null 2>&1 || {
    echo "error: vainfo not found"
    exit 1
}

if [ -n "$DRIVER_PATH" ]; then
    export LIBVA_DRIVERS_PATH="$DRIVER_PATH"
fi
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-nvidia}"

TMP_OUT=$(mktemp)
cleanup() {
    rm -f "$TMP_OUT"
}
trap cleanup EXIT INT TERM

VAINFO_CMD="vainfo"
if [ -n "${LIBVA_DRM_DEVICE:-}" ] && [ -e "${LIBVA_DRM_DEVICE}" ]; then
    VAINFO_CMD="vainfo --display drm --device ${LIBVA_DRM_DEVICE}"
fi

echo "==> $VAINFO_CMD"
if ! sh -c "$VAINFO_CMD" >"$TMP_OUT" 2>&1; then
    if [ -z "${LIBVA_DRM_DEVICE:-}" ] && [ -e /dev/dri/renderD128 ]; then
        VAINFO_CMD="vainfo --display drm --device /dev/dri/renderD128"
        echo "==> retrying with DRM device: $VAINFO_CMD"
        if ! sh -c "$VAINFO_CMD" >"$TMP_OUT" 2>&1; then
            cat "$TMP_OUT"
            echo "FAIL: vainfo failed"
            exit 1
        fi
    else
        cat "$TMP_OUT"
        echo "FAIL: vainfo failed"
        exit 1
    fi
fi

if ! grep -Eq 'VAEntrypointEncSlice(LP)?' "$TMP_OUT"; then
    cat "$TMP_OUT"
    echo "FAIL: no VAAPI encode entrypoints found"
    exit 1
fi

if ! grep -Eq 'VAProfileH264[^:]*:[[:space:]]*VAEntrypointEncSlice(LP)?' "$TMP_OUT"; then
    cat "$TMP_OUT"
    echo "FAIL: H.264 VAAPI encode entrypoint not found"
    exit 1
fi

if grep -Eq 'VAProfileHEVC[^:]*:[[:space:]]*VAEntrypointEncSlice(LP)?' "$TMP_OUT"; then
    echo "PASS: HEVC encode entrypoint detected"
else
    echo "WARN: HEVC encode entrypoint not detected"
fi

echo "PASS: VAAPI/NVENC capability smoke test"

if [ "$RUN_FFMPEG" -eq 1 ]; then
    command -v ffmpeg >/dev/null 2>&1 || {
        echo "FAIL: --ffmpeg requested but ffmpeg is not installed"
        exit 1
    }
    DEVICE="${LIBVA_DRM_DEVICE:-/dev/dri/renderD128}"
    [ -e "$DEVICE" ] || {
        echo "FAIL: ffmpeg probe needs a DRM device, missing: $DEVICE"
        exit 1
    }
    echo "==> ffmpeg h264_vaapi encode probe on $DEVICE"
    ffmpeg -hide_banner -loglevel error \
        -f lavfi -i testsrc2=size=1280x720:rate=30 \
        -frames:v 30 \
        -vaapi_device "$DEVICE" \
        -vf 'format=nv12,hwupload' \
        -c:v h264_vaapi \
        -f null -
    echo "PASS: ffmpeg h264_vaapi encode probe"
fi
