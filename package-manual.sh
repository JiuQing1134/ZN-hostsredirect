#!/bin/bash
set -e

ROOT="/mnt/d/win_root/tmp/zn-hostsredirect-audit/ZN-hostsredirect"
STAGE="$ROOT/build-manual-module"
OUT="$ROOT/release/ZN-Hostsredirect-v1.1-aap-an00-arm64.zip"
TPL="$ROOT/module/template"

rm -rf "$STAGE"
mkdir -p "$STAGE/lib/arm64" "$STAGE/META-INF/com/google/android" "$ROOT/release"

cp "$ROOT/README.md" "$STAGE/README.md"
cp "$TPL/verify.sh" "$STAGE/verify.sh"
cp "$TPL/sepolicy.rule" "$STAGE/sepolicy.rule"
cp "$TPL/post-fs-data.sh" "$STAGE/post-fs-data.sh"
cp "$TPL/META-INF/com/google/android/update-binary" "$STAGE/META-INF/com/google/android/update-binary"
cp "$TPL/META-INF/com/google/android/updater-script" "$STAGE/META-INF/com/google/android/updater-script"

sed 's/@DEBUG@/false/g' "$TPL/service.sh" > "$STAGE/service.sh"
sed -e 's/@DEBUG@/false/g' \
    -e 's/@SONAME@/hostsredirect/g' \
    -e 's/@SUPPORTED_ABIS@/arm64/g' \
    -e 's/@MIN_SDK@/29/g' \
    "$TPL/customize.sh" > "$STAGE/customize.sh"
sed 's/${moduleId}/hostsredirect/g' "$TPL/zn_modules.txt" > "$STAGE/zn_modules.txt"

cat > "$STAGE/module.prop" <<'EOF'
id=hostsredirect
name=Hosts Redirect
version=v1.1-aap-an00
versionCode=9999
author=aviraxp (local hardened build)
description=Redirect /system/etc/hosts to /data/adb/hostsredirect/hosts by injecting netd
#updateJson=
EOF

cp "$ROOT/module/src/main/cpp/libhostsredirect.so" "$STAGE/lib/arm64/libhostsredirect.so"

# verify.sh expects a bare hash file next to each extracted file
for f in customize.sh verify.sh sepolicy.rule module.prop post-fs-data.sh service.sh zn_modules.txt; do
    sha256sum "$STAGE/$f" | cut -d' ' -f1 > "$STAGE/$f.sha256"
done
sha256sum "$STAGE/lib/arm64/libhostsredirect.so" | cut -d' ' -f1 > "$STAGE/lib/arm64/libhostsredirect.so.sha256"
sha256sum "$STAGE/META-INF/com/google/android/update-binary" | cut -d' ' -f1 > "$STAGE/META-INF/com/google/android/update-binary.sha256"

(cd "$STAGE" && zip -9 -r "$OUT" . >/dev/null)
echo "built: $OUT"
ls -lh "$OUT"
unzip -l "$OUT"
