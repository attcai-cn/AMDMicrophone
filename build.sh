#!/bin/bash
# Build AMDMicrophone.kext using Command Line Tools only.
# No Xcode.app / xcodebuild required; Xcode's kext build settings are
# reproduced explicitly here.
set -euo pipefail

cd "$(dirname "$0")"

MKS="$PWD/MacKernelSDK/Headers"
SDK="$(xcrun --show-sdk-path)"
SDKVER="$(xcrun --show-sdk-version)"
DEPLOY=12.6
OUT="$PWD/build/Release/AMDMicrophone.kext"
OBJ="$PWD/build/obj"

if [ ! -d "$MKS" ]; then
    echo "MacKernelSDK headers missing. Run: git submodule update --init MacKernelSDK" >&2
    exit 1
fi

mkdir -p "$OUT/Contents/MacOS" "$OBJ"

# Xcode's kext target uses these flags with KERNEL_FRAMEWORK_HEADERS pointing
# at MacKernelSDK; -nostdinc keeps the userspace libc++ headers out.
CXXFLAGS=(
    -std=gnu++20
    -target "x86_64-apple-macosx$DEPLOY"
    -mkernel -fno-builtin -nostdinc
    -isystem "$MKS" -I AMDMicrophone
    -D__KERNEL__ -DKERNEL -DAPPLE
    -Wno-\#warnings -Wno-inconsistent-missing-override
)

for src in AMDMicrophone/AMDMicrophoneDevice.cpp AMDMicrophone/AMDMicrophoneEngine.cpp; do
    obj="$OBJ/$(basename "${src%.cpp}").o"
    echo "CC  $src"
    clang++ "${CXXFLAGS[@]}" -c "$src" -o "$obj"
done

echo "LD  $OUT/Contents/MacOS/AMDMicrophone"
ld -kext -arch x86_64 \
    -platform_version macos "$DEPLOY" "$SDKVER" \
    -syslibroot "$SDK" \
    -o "$OUT/Contents/MacOS/AMDMicrophone" \
    "$OBJ/AMDMicrophoneDevice.o" "$OBJ/AMDMicrophoneEngine.o" \
    -lkmod

# GENERATE_INFOPLIST_FILE=YES in the .pbxproj expands $(PRODUCT_BUNDLE_IDENTIFIER)
# and injects the CFBundle* keys. Do the same here.
BUNDLE_ID=com.qhuyduong.AMDMicrophone VERSION=1.0.0d1 \
SRC_PLIST="$PWD/AMDMicrophone/Info.plist" \
DST_PLIST="$OUT/Contents/Info.plist" \
python3 - <<'PY'
import os, plistlib
with open(os.environ["SRC_PLIST"], "rb") as f:
    d = plistlib.load(f)
bid = os.environ["BUNDLE_ID"]
for personality in d.get("IOKitPersonalities", {}).values():
    if isinstance(personality, dict):
        for k, v in list(personality.items()):
            if isinstance(v, str):
                personality[k] = v.replace("$(PRODUCT_BUNDLE_IDENTIFIER)", bid)
d.update({
    "CFBundleIdentifier": bid,
    "CFBundleExecutable": "AMDMicrophone",
    "CFBundleName": "AMDMicrophone",
    "CFBundleDisplayName": "AMDMicrophone",
    "CFBundlePackageType": "KEXT",
    "CFBundleVersion": os.environ["VERSION"],
    "CFBundleShortVersionString": os.environ["VERSION"],
    "CFBundleInfoDictionaryVersion": "6.0",
    "CFBundleDevelopmentRegion": "en",
    "CFBundleSignature": "????",
    "NSHumanReadableCopyright": "",
})
with open(os.environ["DST_PLIST"], "wb") as f:
    plistlib.dump(d, f, sort_keys=True)
PY

plutil -lint "$OUT/Contents/Info.plist"
codesign -s - --force --deep "$OUT" >/dev/null 2>&1 || \
    echo "warning: ad-hoc codesign failed (kext may still load with SIP disabled)" >&2

echo "Built: $OUT"
file "$OUT/Contents/MacOS/AMDMicrophone"
