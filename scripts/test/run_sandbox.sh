#!/bin/sh
# Runs the game unattended in a background window against a sandbox: a folder with a
# `game` folder (a junction to the real one is enough) and its own `userdata` (a copy of a
# player's settings and saves). Saves can be overwritten there without touching anyone's
# real ones, and settings aren't put back afterwards (the sandbox is disposable).
#
# Usage: scripts/test/run_sandbox.sh <out dir> <seconds> <input script>
#   <input script>: a file (see scripts/test/inputs/) or inline statements.
# Environment:
#   DAO_SANDBOX=<folder>  the sandbox (required)
#   EXTRA="DAO_X=1 ..."   more variables for the game (DAO_GPU_TIMING=1, ...)
#   CAPTURE=<s>           capture interval (default 2; 0 = none)
#   EXE=<path>            the executable (default build/DAORecomp/DAORecomp.exe)
#
# Leaves <out dir> with capture_N.png, dao.log and xenia.log (and the logs the run wrote
# in the exe's folder), and prints a summary.
# Delete a sandbox's `game` junction with rmdir, never rm -r (that empties the target).
set -u
cd "$(dirname "$0")/../.." || exit 1
if [ $# -lt 3 ]; then sed -n '7,13p' "$0"; exit 2; fi
OUT=$1; SECS=$2; INPUT=$3
SBX=${DAO_SANDBOX:-}
EXE=${EXE:-build/DAORecomp/DAORecomp.exe}
[ -n "$SBX" ] || { echo "Set DAO_SANDBOX to the sandbox folder."; exit 1; }
[ -f "$SBX/game/default.xex" ] || { echo "No $SBX/game/default.xex."; exit 1; }
[ -d "$SBX/userdata" ] || { echo "No $SBX/userdata."; exit 1; }
[ -f "$EXE" ] || { echo "No $EXE."; exit 1; }
if tasklist 2>/dev/null | grep -qi "daorecomp.exe"; then
    echo "The game is running: not starting a test."; exit 1
fi

rm -f "$SBX"/userdata/capture_*.png
CAPTURE=${CAPTURE:-2}
CAPTURE_VAR=
[ "$CAPTURE" != 0 ] && CAPTURE_VAR="DAO_CAPTURE_EVERY=$CAPTURE"
# Started in the exe's folder, as a shortcut starts it: files written to the working
# folder (mods' logs, such as OptiScaler's) land there. So the paths the game gets are
# absolute, and Windows paths (C:/...).
EXE=$(cygpath -m "$(realpath "$EXE")")
SBX=$(cygpath -m "$(realpath "$SBX")")
[ -f "$INPUT" ] && INPUT=$(cygpath -m "$(realpath "$INPUT")")
[ -n "$EXE" ] && [ -n "$SBX" ] || { echo "Can't make the paths absolute."; exit 1; }
EXE_DIR=$(dirname "$EXE")
STARTED=$(mktemp) || exit 1
trap 'rm -f "$STARTED"' EXIT
# EXTRA is split into words on purpose (several VAR=value assignments).
# shellcheck disable=SC2086
(cd "$EXE_DIR" &&
    env DAO_BACKGROUND=1 $CAPTURE_VAR ${EXTRA:-} DAO_INPUT_SCRIPT="$INPUT" DAO_GAME="$SBX/game" \
        timeout "$SECS" "$EXE" --play > /dev/null 2>&1)

mkdir -p "$OUT" || exit 1
rm -f "$OUT"/capture_*.png "$OUT"/dao.log "$OUT"/xenia.log
mv "$SBX"/userdata/capture_*.png "$OUT"/ 2>/dev/null
cp "$SBX"/userdata/dao.log "$SBX"/userdata/xenia.log "$OUT"/ 2>/dev/null
# Logs the run wrote in the exe's folder (mods').
find "$EXE_DIR" -maxdepth 1 -name '*.log' -newer "$STARTED" -exec cp {} "$OUT"/ \;
echo "$(ls "$OUT" | grep -c '^capture_') captures in $OUT"
grep -hiE "abort|device (removed|hung|lost)|887A000|fatal" "$OUT"/dao.log "$OUT"/xenia.log 2>/dev/null | head -5
