#!/usr/bin/env bash
# build.sh — rebuilds selinux-bridge/build/apk_final/selinux-bridge.apk
# from source, using raw SDK command-line tools (no gradle network fetch).
#
# Requires an Android SDK with build-tools + a platforms/android-34 jar.
# On this project's dev machine that's /opt/Android/sdk; override via
# ANDROID_SDK_ROOT if yours lives elsewhere. The *-2 (x86-64) build-tools
# jars for d8/apksigner are invoked directly via `java -jar`-style
# classpath, since they are plain Java tools and run fine under any JVM
# regardless of the build-tools directory's host arch. aapt2/aidl/
# zipalign must come from an aarch64-native build-tools directory when
# running this script inside an ARM64 Termux/PRoot container (the x86-64
# aapt2 binary SIGILLs there).
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_SDK_ROOT:-/opt/Android/sdk}"
BT_NATIVE="${BT_NATIVE:-$SDK/build-tools/34.0.0}"      # aapt2/zipalign (arch-native)
BT_JAVA="${BT_JAVA:-$SDK/build-tools/34.0.0-2}"        # d8.jar/apksigner.jar (arch-agnostic)
PLATFORM_JAR="${PLATFORM_JAR:-$SDK/platforms/android-34/android.jar}"

# The signing key lives OUTSIDE build/ on purpose. It used to be at
# build/debug.keystore, which `rm -rf build` below destroyed on every run,
# so each rebuild produced an APK with a different signing identity and
# Android refused to install it over the previous one ("App not installed"),
# forcing an uninstall and losing app state. Keeping it in keystore/ makes
# rebuilds true in-place updates. The directory is gitignored: this is a
# local debug key, never a release key, and must not be committed.
KEYSTORE="${KEYSTORE:-keystore/selinux-bridge.keystore}"

# Migrate a key from the old throwaway location BEFORE build/ is wiped, so
# an already-installed APK keeps updating in place instead of needing an
# uninstall (Android rejects an update signed by a different key).
mkdir -p "$(dirname "$KEYSTORE")"
if [ ! -f "$KEYSTORE" ] && [ -f build/debug.keystore ]; then
  echo "migrating signing key out of build/ so rebuilds stop changing identity"
  cp build/debug.keystore "$KEYSTORE"
fi

rm -rf build
mkdir -p build/res-compiled build/gen build/classes build/dex build/apk_unsigned build/apk_final

echo "[1/7] compiling resources"
"$BT_NATIVE/aapt2" compile --dir app/src/main/res -o build/res-compiled

echo "[2/7] linking base APK + generating R.java"
"$BT_NATIVE/aapt2" link -o build/apk_unsigned/base.apk \
  --manifest AndroidManifest.xml \
  -I "$PLATFORM_JAR" \
  --java build/gen \
  build/res-compiled/*.flat

echo "[3/7] compiling Java sources"
javac --release 8 -d build/classes -classpath "$PLATFORM_JAR" \
  app/src/main/java/com/selinuxbridge/app/*.java build/gen/com/selinuxbridge/app/R.java

echo "[4/7] dexing"
java -cp "$BT_JAVA/lib/d8.jar" com.android.tools.r8.D8 \
  --output build/dex --lib "$PLATFORM_JAR" \
  $(find build/classes -name "*.class")

echo "[5/7] assembling + aligning APK"
cp build/apk_unsigned/base.apk build/apk_unsigned/full.apk
(cd build/dex && zip -q ../apk_unsigned/full.apk classes.dex)
"$BT_NATIVE/zipalign" -f -p 4 build/apk_unsigned/full.apk build/apk_unsigned/aligned.apk

echo "[6/7] signing"
if [ ! -f "$KEYSTORE" ]; then
  echo "      no keystore at $KEYSTORE, generating a persistent debug key"
  keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=selinux-bridge,O=selinux-bridge,C=US"
fi
java -cp "$BT_JAVA/lib/apksigner.jar" com.android.apksigner.ApkSignerTool sign \
  --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey \
  --out build/apk_final/selinux-bridge.apk \
  build/apk_unsigned/aligned.apk

echo "[7/7] compiling loopback test client"
gcc -O2 -Wall bridge_client.c -o bridge_client -lpthread

echo "done: build/apk_final/selinux-bridge.apk and ./bridge_client"
