#!/bin/sh
# Build GatedPulse.apk with the bare SDK tools -- no Gradle, no AndroidX.
#
#   android/build.sh            build + sign  ->  android/GatedPulse.apk
#   android/build.sh install    ... then adb install -r onto a connected phone
#   DEBUG=1 android/build.sh    debuggable build (WebView inspectable) ->
#                               build/GatedPulse-debug.apk; never committed
#
# One-time setup (see android/README.md):
#   JDK 21, and the Android SDK at ~/Library/Android/sdk with
#   platforms;$PLATFORM  build-tools;$BUILD_TOOLS  platform-tools
#
# Bump VERSION_CODE on every release you commit: Android refuses to install
# over an installed copy with a higher code (a downgrade).
set -eu

VERSION_CODE=5
VERSION_NAME=1.4
MIN_SDK=24            # Android 7.0
TARGET_SDK=37         # Android 17 -- local-network permission enforced from here

PLATFORM=${PLATFORM:-android-37.1}
BUILD_TOOLS=${BUILD_TOOLS:-37.0.0}
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}
# The signing key lives OUTSIDE the repo. Keep it: an APK signed with a
# different key can't update the installed app, it has to be uninstalled first.
KEYSTORE=${KEYSTORE:-$HOME/.android/gatedpulse-release.jks}
KS_PASS=${KS_PASS:-gatedpulse}

cd "$(dirname "$0")"

# The system default here may be an ancient JDK; ask for 21+ explicitly.
JAVA_HOME=$(/usr/libexec/java_home -v 21+ 2>/dev/null || true)
[ -n "$JAVA_HOME" ] || { echo "need JDK 21+ (see android/README.md)"; exit 1; }
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"

BT=$SDK/build-tools/$BUILD_TOOLS
JAR=$SDK/platforms/$PLATFORM/android.jar
[ -f "$JAR" ] || { echo "missing $JAR -- run: sdkmanager \"platforms;$PLATFORM\""; exit 1; }
[ -x "$BT/aapt2" ] || { echo "missing $BT -- run: sdkmanager \"build-tools;$BUILD_TOOLS\""; exit 1; }

rm -rf build
mkdir -p build/gen build/classes build/dex

echo "== resources"
"$BT/aapt2" compile --dir res -o build/res.zip
"$BT/aapt2" link -I "$JAR" --manifest AndroidManifest.xml \
  --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK \
  --version-code $VERSION_CODE --version-name $VERSION_NAME \
  ${DEBUG:+--debug-mode} --java build/gen -o build/base.apk build/res.zip

echo "== java"
javac --release 11 -Xlint:deprecation -classpath "$JAR" -d build/classes \
  $(find src build/gen -name '*.java')

echo "== dex"
"$BT/d8" --release --min-api $MIN_SDK --lib "$JAR" --output build/dex \
  $(find build/classes -name '*.class')

echo "== package + sign"
cp build/base.apk build/unsigned.apk
(cd build/dex && zip -q -X ../unsigned.apk classes.dex)
"$BT/zipalign" -f 4 build/unsigned.apk build/aligned.apk

if [ ! -f "$KEYSTORE" ]; then
  echo "== new signing key: $KEYSTORE  (back it up)"
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -keystore "$KEYSTORE" -alias gatedpulse -keyalg RSA \
    -keysize 3072 -validity 10000 -dname "CN=GatedPulse" \
    -storepass "$KS_PASS" -keypass "$KS_PASS" >/dev/null
fi
OUT=GatedPulse.apk
[ -n "${DEBUG:-}" ] && OUT=build/GatedPulse-debug.apk
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$KS_PASS" \
  --out "$OUT" build/aligned.apk
"$BT/apksigner" verify "$OUT"
rm -f "$OUT.idsig"

echo "== built android/$OUT  ($(wc -c < "$OUT" | tr -d ' ') bytes, v$VERSION_NAME/$VERSION_CODE${DEBUG:+, DEBUG})"

if [ "${1:-}" = install ]; then
  "$SDK/platform-tools/adb" install -r "$OUT"
fi
