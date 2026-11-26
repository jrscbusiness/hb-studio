#!/usr/bin/env bash
# Cheap local check for blind Windows edits: runs `g++ -fsyntax-only` (mingw-w64 cross compiler) over the
# Windows-only translation units whose headers mingw can handle. It catches wrong namespaces/includes,
# undeclared identifiers, use-before-declaration, bad lambdas/std::function targets... It is NOT a
# replacement for the MSVC build in CI (different compiler, no WebView2/WRL/Media Foundation virtual camera).
#
#   sudo apt-get install -y g++-mingw-w64-x86-64
#   tools/check_windows_syntax.sh                 # check every file in the list below
#   tools/check_windows_syntax.sh src/app/windows/app_main.cpp   # only these files
#
# webview_host.hpp / vcam_controller.hpp need headers mingw does not ship (WebView2.h, wrl.h,
# mfvirtualcamera.h): patched copies (those includes removed, COM members turned into void*) are generated
# into a temp dir at run time and put first on the include path, so they never drift from the real headers.
#
# NOT checkable here (skipped on purpose):
#   src/app/windows/webview_host.cpp      needs WebView2.h + WRL
#   src/app/windows/vcam_controller.cpp   needs mfvirtualcamera.h (Windows 11 SDK)
#   src/streaming/windows/mf_video_encoder.cpp   mingw's codecapi.h does not compile
#   src/virtual-camera/windows/vcam_sender.cpp   std::thread::native_handle is not a HANDLE under mingw
#   src/virtual-camera/windows/vcam_source.cpp   Media Foundation virtual camera SDK
#   src/vst3/windows/vst3_editor.cpp      needs the VST3 SDK include dir from FetchContent
set -u
cd "$(dirname "$0")/.."

CXX=""
for c in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++; do
  if command -v "$c" >/dev/null 2>&1; then CXX="$c"; break; fi
done
if [ -z "$CXX" ]; then
  echo "mingw-w64 not found. Install it: sudo apt-get install -y g++-mingw-w64-x86-64" >&2
  exit 2
fi

FILES=(
  src/app/windows/app_main.cpp
  src/app/windows/dshow_capture.cpp
  src/app/windows/output_window.cpp
  src/app/windows/presenter_window.cpp
  src/app/windows/setup_main.cpp
  src/app/windows/stream_sender.cpp
  src/app/windows/wic_jpeg.cpp
  src/app/windows/win_util.cpp
  src/net/http_win.cpp
  src/net/secret_store_win.cpp
  src/captions/windows/winhttp_client.cpp
  src/chat/net_win.cpp
  src/chroma/windows/wic_image.cpp
  src/chroma/windows/winhttp_client.cpp
  src/cloud/cloud_platform_win.cpp
  src/discovery/udp_socket_win.cpp
  src/marketplace/downloader_win.cpp
  src/ndi/ndi_backend_win.cpp
  src/onvif/http_client_win.cpp
  src/overlay/windows/gdi_glyphs.cpp
  src/ptz/udp_transport_win.cpp
  src/recording/windows/mf_writers.cpp
  src/remote/lan_win.cpp
  src/remote/random_win.cpp
  src/rtsp/tcp_stream_win.cpp
  src/streaming/windows/aac_asc.cpp
  src/streaming/windows/mf_audio_encoder.cpp
  src/streaming/windows/nal_utils.cpp
)
if [ "$#" -gt 0 ]; then FILES=("$@"); fi

STUBS="$(mktemp -d)"
trap 'rm -rf "$STUBS"' EXIT
mkdir -p "$STUBS/app/windows"
sed -e '/#include <mfidl.h>/d' -e '/#include <mfvirtualcamera.h>/d' \
    -e 's/IMFVirtualCamera\* vcam_ = nullptr;/void* vcam_ = nullptr;/' \
    src/app/windows/vcam_controller.hpp > "$STUBS/app/windows/vcam_controller.hpp"
sed -e '/#include <wrl.h>/d' -e '/#include "WebView2.h"/d' \
    -e 's/Microsoft::WRL::ComPtr<[A-Za-z0-9]*> /void* /' \
    src/app/windows/webview_host.hpp > "$STUBS/app/windows/webview_host.hpp"

fail=0
for f in "${FILES[@]}"; do
  if out="$("$CXX" -std=c++20 -fsyntax-only -w -DUNICODE -D_UNICODE -DNOMINMAX -DHB_VERSION='"0.0.0"' \
        -I"$STUBS" -Isrc "$f" 2>&1)"; then
    echo "ok    $f"
  else
    echo "FAIL  $f"
    printf '%s\n' "$out" | grep -E "error" | head -10
    fail=1
  fi
done
exit $fail
