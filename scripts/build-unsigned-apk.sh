#!/usr/bin/env bash
# build-unsigned-apk.sh - the Android release APK, UNSIGNED, for the publisher to sign (builders
# never hold the signing key).
#   scripts/build-unsigned-apk.sh  ->  mobile/android/app/build/outputs/apk/release/app-release-unsigned.apk
# expo prebuild --clean regenerates android/ every time, so this edits only generated files.
# Run it in the foreground or with nohup (gradle needs the memory).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
LOG="${CREMA_APK_LOG:-$ROOT/mobile/build-unsigned-apk.log}"
cd "$ROOT/mobile"
: > "$LOG"
node scripts/vendor-map.mjs >> "$LOG" 2>&1   # MapLibre + pmtiles inlined into the map page (no CDN)
npx expo prebuild --platform android --clean --no-install >> "$LOG" 2>&1
echo "sdk.dir=$ANDROID_HOME" > android/local.properties     # --clean wipes it
python3 - <<'PY'
import re
p = "android/app/build.gradle"; s = open(p).read()
m = re.search(r"(buildTypes\s*\{.*?release\s*\{)(.*?)(\n\s*\})", s, re.S)
assert m, "release buildType not found"
body = m.group(2).replace("signingConfig signingConfigs.debug", "signingConfig null")
assert "signingConfig null" in body, "could not switch release signing off"
open(p, "w").write(s[:m.start(2)] + body + s[m.end(2):])
PY
(cd android && ./gradlew assembleRelease -PreactNativeArchitectures=arm64-v8a --no-daemon -x lintVitalRelease) >> "$LOG" 2>&1
APK=android/app/build/outputs/apk/release/app-release-unsigned.apk
AAPT2="$(ls -d "$ANDROID_HOME"/build-tools/*/aapt2 | sort -V | tail -1)"
"$AAPT2" dump badging "$APK" | grep -E "^package" | cut -c1-120
"$AAPT2" dump permissions "$APK" | grep -E "permission" | sed 's/^/  /'
if unzip -p "$APK" 'classes*.dex' | grep -aqE "com/google/firebase|com/google/android/gms|com/google/mlkit"; then echo "APK CONTAINS FIREBASE / PLAY SERVICES / ML KIT CODE"; exit 1; fi
echo "apk: no Firebase / Play services / ML Kit code"
sha256sum "$APK"
