#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

print_help() {
    cat <<'EOF'
Usage:
  ./test.sh ci
  ./test.sh smoke [--driver-path <path>] [--ffmpeg]
  ./test.sh mpv <video-file>
  ./test.sh <video-file>

Commands:
  ci      Run CI-safe static sanity checks (no GPU required).
  smoke   Run runtime VA-API/NVENC smoke checks on local hardware.
  mpv     Run mpv with VA-API decode debug flags.

Notes:
  - Passing a plain file path keeps legacy behavior and launches mpv.
EOF
}

run_mpv() {
    if ! command -v mpv >/dev/null 2>&1; then
        echo "error: mpv not found"
        exit 1
    fi

    # Make sure we use this VA-API driver implementation.
    export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-nvidia}"
    export NVD_LOG="${NVD_LOG:-1}"

    # --hwdec-codecs=all and --vd-lavc-check-hw-profile=no force hardware decode probing.
    exec mpv -v --msg-color=no --msg-level=all=debug --hwdec=vaapi --gpu-debug \
        --hwdec-codecs=all --vd-lavc-check-hw-profile=no "$@"
}

COMMAND="${1:-}"
case "$COMMAND" in
    ci)
        shift
        exec "$SCRIPT_DIR/tests/ci-sanity.sh" "$@"
        ;;
    smoke)
        shift
        exec "$SCRIPT_DIR/tests/runtime-smoke.sh" "$@"
        ;;
    mpv)
        shift
        run_mpv "$@"
        ;;
    -h|--help|help)
        print_help
        ;;
    "")
        print_help
        exit 2
        ;;
    *)
        # Legacy behavior: ./test.sh <video-file>
        run_mpv "$@"
        ;;
esac
