#!/bin/bash
# build.sh — 手工 APK 流水线（无 gradle）
set -e
BT=/opt/android/bt/android-14
JAR=/opt/android/plat/android-34/android.jar
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC=$ROOT/app/src/main
OUT=$ROOT/out
rm -rf "$OUT"; mkdir -p "$OUT/gen" "$OUT/classes" "$OUT/apk"

echo "[1/6] aapt2 link"
$BT/aapt2 link -o "$OUT/base.apk" -I "$JAR" \
  --manifest "$SRC/AndroidManifest.xml" \
  -A "$SRC/assets" \
  --min-sdk-version 29 --target-sdk-version 34 \
  --auto-add-overlay

echo "[2/6] javac"
javac -source 11 -target 11 -nowarn -classpath "$JAR" \
  -d "$OUT/classes" \
  "$SRC/java/com/dvmw/builder/MainActivity.java"

echo "[3/6] d8 dex"
$BT/d8 --release --lib "$JAR" \
  --output "$OUT/apk" \
  $(find "$OUT/classes" -name '*.class')

echo "[4/6] pack"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
cd "$OUT/apk" && zip -qj ../unsigned.apk classes.dex && cd "$ROOT"
# native libs must be at lib/<abi>/ in APK
mkdir -p "$OUT/nativelibs/lib/arm64-v8a"
for f in "$SRC"/jniLibs/arm64-v8a/*.so; do
  cp "$f" "$OUT/nativelibs/lib/arm64-v8a/"
done
cd "$OUT/nativelibs" && zip -qr ../unsigned.apk lib && cd "$ROOT"

echo "[5/6] zipalign"
$BT/zipalign -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "[6/6] sign"
if [ ! -f "$ROOT/debug.keystore" ]; then
  keytool -genkeypair -keystore "$ROOT/debug.keystore" -storepass dvmw1234 \
    -keypass dvmw1234 -alias dvmw -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=DroidVM Builder, O=dvmw, C=CN" >/dev/null 2>&1
fi
$BT/apksigner sign --ks "$ROOT/debug.keystore" --ks-pass pass:dvmw1234 \
  --out "$OUT/DroidVM-Win11-Builder.apk" "$OUT/aligned.apk"
$BT/apksigner verify "$OUT/DroidVM-Win11-Builder.apk" && echo "OK: $OUT/DroidVM-Win11-Builder.apk"
ls -lh "$OUT/DroidVM-Win11-Builder.apk"
