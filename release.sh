#!/usr/bin/env bash
# ------------------------------------------------------------------
# MajorSLOP release builder.
#
#   ./release.sh [VERSION]
#
# Rebuilds every DLL from source, stages a clean release tree, and zips
# it. Produces MajorSLOP-<VERSION>.zip in the repo root.
#
# Output layout (drops straight into a user's MegaMUD folder):
#   MajorSLOP-<VERSION>/
#     msimg32.dll              <- sits at MegaMUD root (proxy loader)
#     plugins/
#       vk_terminal.dll
#       vk_terminal.ini
#       mmudpy.dll
#       autoroam_watchdog.dll
#       script_manager.dll
#       TTS_SAM.dll
#       TTS_eSpeak.dll         (if buildable)
#       voice.dll              (if buildable)
#       slop.py, slop_console.py, vft_demo.py
#       MMUDPy/{mmudpy.dll,scripts/}
#       fonts/
#       espeak-ng-data/
#       voice-model/
#       python/
#     README.md
# ------------------------------------------------------------------
set -euo pipefail

VERSION="${1:-v0.1.1-alpha}"
REPO="$(cd "$(dirname "$0")" && pwd)"
DLL_DIR="$REPO/dll"
DEPLOY_DIR="$HOME/.wine/drive_c/MegaMUD"
STAGE_ROOT="$REPO/release-staging"
STAGE="$STAGE_ROOT/MajorSLOP-$VERSION"
OUT_ZIP="$REPO/MajorSLOP-$VERSION.zip"

CC=i686-w64-mingw32-gcc

say()  { printf "\033[1;36m==>\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m!!\033[0m %s\n" "$*"; }
fail() { printf "\033[1;31mXX\033[0m %s\n" "$*" >&2; exit 1; }

command -v $CC >/dev/null 2>&1 || fail "missing mingw: $CC"
command -v python3 >/dev/null 2>&1 || fail "missing python3 (used for zipping)"

say "Building MajorSLOP $VERSION"
rm -rf "$STAGE_ROOT"
mkdir -p "$STAGE/plugins"

cd "$DLL_DIR"

# ------------------------------------------------------------------
# 1. msimg32.dll  (proxy loader at MegaMUD root — lowercase per README)
# ------------------------------------------------------------------
say "Build msimg32.dll (proxy loader)"
$CC -shared -o msimg32.dll msimg32_proxy.c msimg32.def \
    -lgdi32 -lws2_32 -luser32 -mwindows
cp msimg32.dll "$STAGE/msimg32.dll"

# ------------------------------------------------------------------
# 2. vk_terminal.dll  (main Vulkan renderer + MUDRadio)
# ------------------------------------------------------------------
say "Build vk_terminal.dll"
# Regenerate stdcall Vulkan import lib if missing
if [[ ! -f libvk_fixed.a ]]; then
    warn "libvk_fixed.a missing — regenerating"
    $CC -c -o vk_test.o vk_terminal.c -I. -O2 -Wno-all
    i686-w64-mingw32-nm vk_test.o | grep "U _vk" | sed 's/.*U _//' | sort -u > /tmp/vk_needed.txt
    echo "LIBRARY vulkan-1.dll" > vulkan-1-need.def
    echo "EXPORTS" >> vulkan-1-need.def
    while IFS= read -r sym; do echo "  $sym" >> vulkan-1-need.def; done < /tmp/vk_needed.txt
    i686-w64-mingw32-dlltool -d vulkan-1-need.def -l libvk_fixed.a -k
fi
$CC -shared -o vk_terminal.dll vk_terminal.c kiss_fft.c kiss_fftr.c \
    faad2_src/libfaad/*.c \
    -I. -Ifaad2_src/include -Ifaad2_src/libfaad -Iopus_inc -DPACKAGE_VERSION=\"2.11.1\" \
    -L. libvk_fixed.a libopusfile.a libopus.a libogg.a \
    -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32 -lshlwapi -lwinmm -lws2_32 \
    -mwindows -O2 -static-libgcc \
    -Wno-unused-function -Wno-unused-variable -Wno-unknown-pragmas \
    -Wno-misleading-indentation -Wno-pointer-sign
cp vk_terminal.dll "$STAGE/plugins/vk_terminal.dll"
[[ -f vk_terminal.ini ]] && cp vk_terminal.ini "$STAGE/plugins/vk_terminal.ini"

# ------------------------------------------------------------------
# 3. mmudpy.dll  (Python bridge — deploys to plugins/ AND plugins/MMUDPy/)
# ------------------------------------------------------------------
say "Build mmudpy.dll"
$CC -shared -o mmudpy.dll mmudpy.c \
    -lgdi32 -luser32 -lcomdlg32 -mwindows
cp mmudpy.dll "$STAGE/plugins/mmudpy.dll"
mkdir -p "$STAGE/plugins/MMUDPy"
cp mmudpy.dll "$STAGE/plugins/MMUDPy/mmudpy.dll"

# ------------------------------------------------------------------
# 4. autoroam_watchdog.dll
# ------------------------------------------------------------------
say "Build autoroam_watchdog.dll"
$CC -shared -o autoroam_watchdog.dll autoroam_watchdog.c \
    -lgdi32 -luser32 -static-libgcc
cp autoroam_watchdog.dll "$STAGE/plugins/autoroam_watchdog.dll"

# ------------------------------------------------------------------
# 5. script_manager.dll
# ------------------------------------------------------------------
say "Build script_manager.dll"
$CC -shared -o script_manager.dll script_manager.c \
    -lgdi32 -luser32 -lcomdlg32 -lcomctl32 -lshlwapi -mwindows
cp script_manager.dll "$STAGE/plugins/script_manager.dll"

# ------------------------------------------------------------------
# 6. TTS_SAM.dll
# ------------------------------------------------------------------
say "Build TTS_SAM.dll"
$CC -shared -o TTS_SAM.dll tts_sam.c \
    sam/sam.c sam/reciter.c sam/render.c sam/debug.c \
    -lwinmm -lgdi32 -luser32
cp TTS_SAM.dll "$STAGE/plugins/TTS_SAM.dll"

# ------------------------------------------------------------------
# 7. TTS_eSpeak.dll  (external espeak-ng source required — may skip)
# ------------------------------------------------------------------
ESPEAK_SRC="$HOME/AI/espeak-ng-src"
if [[ -d "$ESPEAK_SRC/build-mingw32/src/libespeak-ng" ]]; then
    say "Build TTS_eSpeak.dll"
    $CC -shared -o TTS_eSpeak.dll tts_espeak.c \
        -DLIBESPEAK_NG_EXPORT \
        -I"$ESPEAK_SRC/src/include" \
        -L"$ESPEAK_SRC/build-mingw32/src/libespeak-ng" \
        -L"$ESPEAK_SRC/build-mingw32/src/ucd-tools" \
        -lespeak-ng -lucd -lwinmm -lgdi32 -luser32 -static-libgcc
    cp TTS_eSpeak.dll "$STAGE/plugins/TTS_eSpeak.dll"
else
    warn "espeak-ng source tree missing at $ESPEAK_SRC — using currently-deployed TTS_eSpeak.dll"
    [[ -f "$DEPLOY_DIR/plugins/TTS_eSpeak.dll" ]] && \
        cp "$DEPLOY_DIR/plugins/TTS_eSpeak.dll" "$STAGE/plugins/TTS_eSpeak.dll" || \
        warn "no TTS_eSpeak.dll available to ship"
fi

# ------------------------------------------------------------------
# 8. voice.dll — EXCLUDED FROM RELEASE for now (WIP, not yet working).
#    Kept deployed locally for testing; re-enable by restoring this block
#    and re-adding 'voice-model' to the asset-dir loop below.
# ------------------------------------------------------------------
say "Skip voice.dll + voice-model (WIP, excluded from release)"

# ------------------------------------------------------------------
# 9. Stage asset directories from the deploy tree (single source of truth).
#    These are large/generated resources we don't rebuild here.
# ------------------------------------------------------------------
say "Stage asset directories"
for dir in fonts espeak-ng-data python; do
    src="$DEPLOY_DIR/plugins/$dir"
    if [[ -d "$src" ]]; then
        cp -r "$src" "$STAGE/plugins/$dir"
        # Strip dev junk that slips into asset dirs
        find "$STAGE/plugins/$dir" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
        find "$STAGE/plugins/$dir" -name '*.pyc' -delete 2>/dev/null || true
    else
        warn "missing asset dir: $src (skipping)"
    fi
done

# MMUDPy/scripts/ — Python user scripts the plugin auto-loads
if [[ -d "$DEPLOY_DIR/plugins/MMUDPy/scripts" ]]; then
    cp -r "$DEPLOY_DIR/plugins/MMUDPy/scripts" "$STAGE/plugins/MMUDPy/scripts"
    find "$STAGE/plugins/MMUDPy/scripts" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
    find "$STAGE/plugins/MMUDPy/scripts" -name '*.pyc' -delete 2>/dev/null || true
fi

# Loose plugin-root python scripts
for f in slop.py slop_console.py vft_demo.py; do
    [[ -f "$DEPLOY_DIR/plugins/$f" ]] && cp "$DEPLOY_DIR/plugins/$f" "$STAGE/plugins/$f"
done

# ------------------------------------------------------------------
# 10. README with install instructions
# ------------------------------------------------------------------
cat > "$STAGE/README.md" <<EOF
# MajorSLOP $VERSION

Drop-in plugin stack for MegaMUD (Vulkan terminal, MUDRadio, scripting, TTS).

## Install

1. Close MegaMUD if running.
2. Copy the contents of this folder into your MegaMUD directory, merging
   with the existing \`plugins/\` folder. File layout:

   \`\`\`
   MegaMUD/
     msimg32.dll            <- from this zip (proxy loader)
     plugins/
       vk_terminal.dll      <- from this zip
       mmudpy.dll           <- from this zip
       ...etc
   \`\`\`

3. Launch \`megamud.exe\`. The proxy loader (msimg32.dll) will inject the
   plugin stack.

## Contents

- \`msimg32.dll\` — proxy loader installed at MegaMUD root
- \`plugins/vk_terminal.dll\` — Vulkan terminal + MUDRadio + FX
- \`plugins/mmudpy.dll\` — Python scripting bridge
- \`plugins/autoroam_watchdog.dll\` — autoroam safety watchdog
- \`plugins/script_manager.dll\` — script manager UI
- \`plugins/TTS_SAM.dll\`, \`plugins/TTS_eSpeak.dll\` — TTS engines
- \`plugins/fonts/\` — bundled TTF fonts
- \`plugins/python/\` — embeddable Python runtime for mmudpy
- \`plugins/espeak-ng-data/\` — eSpeak voice data

Voice recognition (voice.dll + voice-model) is work-in-progress and
intentionally excluded from this release.

## Build info

- Version: $VERSION
- Built: $(date -u '+%Y-%m-%d %H:%M:%S UTC')
- Git commit: $(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo unknown)
- mmudpy.dll SHA256: $(sha256sum "$STAGE/plugins/mmudpy.dll" | cut -c1-16)...
- vk_terminal.dll SHA256: $(sha256sum "$STAGE/plugins/vk_terminal.dll" | cut -c1-16)...

## Recent changes

$(cd "$REPO" && git log --pretty=format:'- %s' -n 10 2>/dev/null || echo "(no git history)")
EOF

# ------------------------------------------------------------------
# 11. Zip it
# ------------------------------------------------------------------
say "Packaging zip (python zipfile, DEFLATE level 9)"
rm -f "$OUT_ZIP"
python3 - "$STAGE_ROOT" "MajorSLOP-$VERSION" "$OUT_ZIP" <<'PY'
import os, sys, zipfile
root, top, out = sys.argv[1], sys.argv[2], sys.argv[3]
src = os.path.join(root, top)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, _dirs, files in os.walk(src):
        for f in files:
            full = os.path.join(dirpath, f)
            arc = os.path.relpath(full, root)
            z.write(full, arc)
PY

SIZE=$(du -h "$OUT_ZIP" | cut -f1)
say "Release ready: $OUT_ZIP ($SIZE)"
echo
echo "To publish on GitHub:"
echo "  git tag $VERSION"
echo "  git push origin $VERSION"
echo "  gh release create $VERSION $OUT_ZIP --title \"MajorSLOP $VERSION\" --notes-file RELEASE_NOTES.md"
