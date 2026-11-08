#!/usr/bin/env bash
# Render-harness runner for the whisperbox view (nix-store Qt on Bosgame, or
# system Qt6 elsewhere: needs qt6-qtbase-devel + qt6-qtdeclarative-devel).
# Compiles harness.cpp against the nix-store Qt6, points QML_IMPORT_PATH at the
# design system the module flake pulls in, then renders Main.qml offscreen with
# each fixture and reports QML errors + screenshots.
#
# usage: render.sh [fixture ...]   (default: all fixtures)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HDIR="$ROOT/scripts/qml-harness"
QML="$ROOT/module/Main.qml"
OUTDIR="${WB_OUT:-/tmp/crema-harness}"
mkdir -p "$OUTDIR"

# ── locate Qt6: nix store (Bosgame) or system Qt via pkg-config (any distro) ──
pick() { for d in /nix/store/*-qt$1-6.9.*; do [ -e "$d$2" ] && { echo "$d"; return; }; done; for d in /nix/store/*-qt$1-6.*; do [ -e "$d$2" ] && { echo "$d"; return; }; done; }
CXXFLAGS="-std=c++17 -fPIC -O1"
QTBASE=""; QTDCL=""
[ -d /nix/store ] && { QTBASE=$(pick base '/lib/libQt6Core.so'); QTDCL=$(pick declarative '/lib/libQt6Quick.so'); }
cd "$HDIR"
if [ -n "$QTBASE" ] && [ -n "$QTDCL" ]; then
    echo "qt (nix):    $QTBASE | $QTDCL"
    MOC=""
    for c in "$QTBASE/libexec/moc" "$QTBASE/bin/moc"; do [ -x "$c" ] && { MOC="$c"; break; }; done
    [ -z "$MOC" ] && MOC=$(for d in /nix/store/*-qtbase-6.*; do [ -d "$d" ] || continue; for c in "$d/libexec/moc" "$d/bin/moc"; do [ -x "$c" ] && { echo "$c"; break 2; }; done; done)
    INCS="-I$QTBASE/include -I$QTBASE/include/QtCore -I$QTBASE/include/QtGui -I$QTBASE/include/QtTest -I$QTDCL/include -I$QTDCL/include/QtQml -I$QTDCL/include/QtQuick"
    LIBS="-L$QTBASE/lib -L$QTDCL/lib -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Test -lQt6Core"
    export LD_LIBRARY_PATH="$QTBASE/lib:$QTDCL/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    QMLPATH="$QTDCL/lib/qt-6/qml"
elif pkg-config --exists Qt6Quick 2>/dev/null; then
    echo "qt (system): $(pkg-config --modversion Qt6Quick)"
    MOC="$(pkg-config --variable=libexecdir Qt6Core)/moc"
    INCS="$(pkg-config --cflags Qt6Quick Qt6Qml Qt6Gui Qt6Test Qt6Core)"
    LIBS="$(pkg-config --libs Qt6Quick Qt6Qml Qt6Gui Qt6Test Qt6Core)"
    QMLPATH="$(qmake6 -query QT_INSTALL_QML 2>/dev/null)"
else
    echo "FATAL: no Qt6 (nix store or pkg-config Qt6Quick)" >&2; exit 2
fi

# Design system QML dir (Logos/Theme/qmldir). WB_DS overrides (e.g. a
# logos-design-system checkout's src/qml); else the nix-built output.
DS="${WB_DS:-}"
if [ -z "$DS" ] && [ -d /nix/store ]; then
    for d in /nix/store/*-logos-design-system-*/lib; do
        if [ -f "$d/Logos/Theme/qmldir" ]; then DS="$d"; break; fi
    done
fi
echo "design sys:  ${DS:-NOT FOUND (set WB_DS=<logos-design-system>/src/qml)}"

"$MOC" harness.cpp -o harness.moc || { echo "FATAL: moc failed" >&2; exit 2; }
g++ $CXXFLAGS $INCS harness.cpp -o harness $LIBS || { echo "FATAL: g++ failed" >&2; exit 2; }
echo "harness built: $HDIR/harness"

[ -n "$DS" ] && QMLPATH="$DS:$QMLPATH"
export QML_IMPORT_PATH="${QML_IMPORT_PATH:+$QML_IMPORT_PATH:}$QMLPATH"
# scenarios.sh reuses exactly this runtime (same Qt as the harness was linked to).
printf 'export LD_LIBRARY_PATH=%q\nexport QML_IMPORT_PATH=%q\n' "${LD_LIBRARY_PATH:-}" "$QML_IMPORT_PATH" > "$HDIR/harness.env"

# ── run against fixtures ──
FIXTURES=("$@")
[ ${#FIXTURES[@]} -eq 0 ] && FIXTURES=("$HDIR"/fixtures/*.json)


FAIL=0
for F in "${FIXTURES[@]}"; do
    NAME=$(basename "$F" .json)
    echo "── rendering fixture: $NAME ──"
    if ./harness "$QML" "$F" "$OUTDIR/$NAME.png" 2> "$OUTDIR/$NAME.log"; then
        echo "PASS: $NAME (screenshot $OUTDIR/$NAME.png)"
    else
        RC=$?
        echo "FAIL($RC): $NAME — see $OUTDIR/$NAME.log"
        grep -E "\[W\]|\[E\]" "$OUTDIR/$NAME.log" | head -15
        FAIL=1
    fi
done
exit $FAIL
