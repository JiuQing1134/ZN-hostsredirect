#!/bin/bash
set -e

NDK=${NDK:-/opt/android-ndk-r29}
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
SRC="/mnt/d/win_root/tmp/zn-hostsredirect-audit/ZN-hostsredirect/module/src/main/cpp"
OUT="$SRC/libhostsredirect.so"

test -x "$CXX" || { echo "missing compiler: $CXX"; exit 1; }

"$CXX" \
  -O2 -shared -fPIC -fno-exceptions -fno-rtti -fvisibility=hidden \
  -nostdlib++ -Wl,--no-undefined -Wl,-soname,libhostsredirect.so \
  -I"$SRC" \
  "$SRC/hook.cpp" "$SRC/socket_utils.cpp" \
  -llog -o "$OUT"

file "$OUT"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$OUT" | grep -E 'NEEDED|SONAME' || true
