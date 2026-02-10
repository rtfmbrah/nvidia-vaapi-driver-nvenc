# nvidia-vaapi-driver-nvenc

Fork of `nvidia-vaapi-driver` with integrated NVENC encode support for VA-API clients.

This project currently provides:
- NVDEC-based VA-API decode (upstream behavior).
- NVENC-based VA-API encode for H.264/HEVC entrypoints (`EncSlice`/`EncSliceLP`).

## Project status

- Decode path: usable and close to upstream behavior.
- Encode path: functional, but still under active tuning.
- Wayland + Electron/Chromium game-window capture can still show visual artifacts on some setups.

## Features

### Decode

Decode capabilities follow the upstream NVDEC implementation and GPU support matrix.

### Encode

Implemented VA-API encode profiles:
- H.264: `ConstrainedBaseline`, `Main`, `High`
- HEVC: `Main`, `Main10`

Implemented VA-API encode features:
- Entrypoints: `VAEntrypointEncSlice`, `VAEntrypointEncSliceLP`
- Rate control: `CBR`, `VBR`, `CQP`
- Packed headers: sequence/picture/slice
- Dynamic reconfigure support (resolution/rate-control changes where possible)

Current encode implementation uses host-memory staging (copy path), not full zero-copy.

## Requirements

- NVIDIA proprietary driver with NVENC/NVDEC support
- `libva`
- `meson` + `ninja`
- `egl`, `libdrm`, `ffnvcodec` headers
- Optional: `gstreamer-codecparsers-1.0` for additional decode profile handling (for example VP9 parsing)

Notes:
- The build uses `ffnvcodec` headers (`<ffnvcodec/nvEncodeAPI.h>`). No manual SDK include path edits are required.
- Meson will use your system `ffnvcodec` package when available, or fall back to `subprojects/ff-nvcodec-headers.wrap`.

## Build and install

```sh
meson setup build_noccache --wipe
meson compile -C build_noccache
sudo meson install -C build_noccache
```

The installed driver filename remains:
- `nvidia_drv_video.so`

And libva driver name remains:
- `LIBVA_DRIVER_NAME=nvidia`

## Testing

The repository now provides explicit test entrypoints:

```sh
# CI-safe static checks (no GPU required)
./test.sh ci

# Local runtime smoke test (checks VAAPI encode entrypoints with vainfo)
./test.sh smoke

# Local runtime smoke test + short ffmpeg h264_vaapi encode probe
LIBVA_DRM_DEVICE=/dev/dri/renderD128 ./test.sh smoke --ffmpeg
```

If `vainfo` cannot connect to X/Wayland in your shell/session, set `LIBVA_DRM_DEVICE` explicitly.

For quick media playback debugging with mpv:

```sh
./test.sh mpv /path/to/video.mp4
```

## Basic usage

### Global install usage

After installation:

```sh
vainfo
```

You should see `VA-API NVDEC/NVENC driver` and encode entrypoints for H.264/HEVC.

### Local/dev override usage

Use build output without installing:

```sh
LIBVA_DRIVERS_PATH=$PWD/build_noccache \
LIBVA_DRIVER_NAME=nvidia \
NVD_LOG=1 \
vesktop
```

## Runtime environment variables

### General

- `NVD_LOG`
  - `1`: log to stdout
  - any other value: append logs to the given file path
- `NVD_BACKEND`
  - `direct` (default) or `egl`
- `NVD_GPU`
  - select NVIDIA GPU index
- `NVD_MAX_INSTANCES`
  - limit concurrent driver instances per process
- `NVD_FORCE_INIT`
  - force init in environments where sandbox detection would normally skip it
- `NVD_ENC_IO_DEPTH`
  - number of NVENC input/bitstream buffer slots to use (default `1`, valid `1..4`)

### Encode debug/testing

- `NVD_ENC_DEBUG=1`
  - enables frame/hash dump diagnostics
- `NVD_ENC_DEBUG_DIR=/tmp/nvd-enc-debug`
  - output directory for debug dumps
- `NVD_ENC_DEBUG_START_FRAME=<N>`
  - start dumping at frame `N`
- `NVD_ENC_DEBUG_MAX_FRAMES=<N>`
  - dump at most `N` frames
- `NVD_ENC_STRICT=1`
  - stricter map/unmap staging path for debugging synchronization behavior
- `NVD_ENC_FORCE_IDR_EVERY=<N>`
  - force IDR every `N` frames (diagnostic only, not recommended for normal usage)
- `NVD_ENC_STARTUP_IDR_FRAMES=<N>`
  - force IDR for the first `N` successfully produced frames of each encode context (default `16`)
- `NVD_ENC_RECONFIG_MIN_MS=<N>`
  - minimum interval (ms) between non-resolution encoder reconfigurations (default `800`)
- `NVD_ENC_VISIBLE_RECONFIG=1`
  - enable visible-rect-based resolution reconfigure (disabled by default for stability)

## Known issues and expectations

- Wayland + Electron/Chromium game-window capture may show black horizontal tears on some GNOME/NVIDIA setups.
- Full desktop capture can behave differently from game-window capture.
- Some issues appear to be outside the driver (capture/import path), even when encode path is healthy.

## Troubleshooting quick checklist

1. Verify driver is loaded:
   - `vainfo`
2. Enable driver logs:
   - `NVD_LOG=1`
3. Verify fallback behavior:
   - check app logs for software encoder fallback (`OpenH264`, etc.)
4. Compare X11 vs Wayland behavior:
   - artifacts only on one platform usually indicate capture-path differences, not pure encoder failure

## License

This fork stays under the upstream MIT license. Keep `COPYING` in the repository and preserve upstream notices.

If you merge code from other repositories:
- confirm license compatibility first
- keep attribution in commit history and documentation
- add explicit notices if required by source licenses
