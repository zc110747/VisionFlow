# Load the MSVC (cl.exe) environment for Ninja builds.
# Usage:  source scripts/msvc-env.sh
#
# The "Visual Studio 17 2022" CMake generator depends on MSBuild.exe, which
# crashes with an access violation on this machine. Using the Ninja generator
# with cl.exe directly sidesteps MSBuild entirely.
#
# Auto-detects the newest MSVC toolset and Windows SDK installed.

MSVC_BASE="${MSVC_BASE:-D:/Software/vs/VC/Tools/MSVC}"
KITS="${WIN_KITS:-C:/Program Files (x86)/Windows Kits/10}"

VCVER="$(ls -1 "$MSVC_BASE" 2>/dev/null | sort -V | tail -1)"
SDKVER="$(ls -1 "$KITS/Include" 2>/dev/null | sort -V | tail -1)"

if [ -z "$VCVER" ] || [ -z "$SDKVER" ]; then
  echo "msvc-env: could not detect MSVC toolset or Windows SDK" >&2
  return 1 2>/dev/null || exit 1
fi

VC="$MSVC_BASE/$VCVER"
# CMake/Ninja shipped with Visual Studio: these are native Windows binaries.
# The MSYS2 cmake would hand a colon-separated PATH down to cmd.exe, which then
# fails to locate rc.exe / mt.exe during linking.
VS_CMAKE="${VS_CMAKE:-D:/Software/vs/Common7/IDE/CommonExtensions/Microsoft/CMake}"

export INCLUDE="$VC/include;$KITS/Include/$SDKVER/ucrt;$KITS/Include/$SDKVER/um;$KITS/Include/$SDKVER/shared;$KITS/Include/$SDKVER/winrt;$KITS/Include/$SDKVER/cppwinrt"
export LIB="$VC/lib/x64;$KITS/Lib/$SDKVER/ucrt/x64;$KITS/Lib/$SDKVER/um/x64"
export PATH="$VC/bin/Hostx64/x64:$KITS/bin/$SDKVER/x64:$VS_CMAKE/CMake/bin:$VS_CMAKE/Ninja:$PATH"

echo "msvc-env: MSVC $VCVER / Windows SDK $SDKVER"
