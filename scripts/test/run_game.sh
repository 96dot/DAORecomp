#!/bin/sh
# Runs the game unattended in a background window with scripted input, captures its
# output every few seconds, and puts the user's settings and saves back afterwards.
#
# Usage: scripts/test/run_game.sh <out dir> <seconds> <input script> [section.key=value ...]
#   <input script>: a file (see scripts/test/inputs/) or inline statements.
#   section.key=value: config.toml settings for this run only, e.g.
#     video.resolution_scale=2 'video.temporal_upscaler="fsr4"' xenia.dao_motion_vectors=false
# Environment:
#   EXTRA="DAO_X=1 ..."  more variables for the game (DAO_GPU_TIMING=1, DAO_DRED=1, ...)
#   CAPTURE=<s>          capture interval (default 2; 0 = none)
#   EXE=<path>           the executable (default build/DAORecomp/DAORecomp.exe)
#   KEEP_SAVES=1         keep saves the run created (they're removed by default)
#
# Leaves <out dir> with capture_N.png, dao.log and xenia.log, and prints a summary.
set -u
cd "$(dirname "$0")/../.." || exit 1
if [ $# -lt 3 ]; then sed -n '5,14p' "$0"; exit 2; fi
OUT=$1; SECS=$2; INPUT=$3; shift 3
EXE=${EXE:-build/DAORecomp/DAORecomp.exe}
CFG=userdata/config.toml
BAK=userdata/config.toml.testbak
SAVES=userdata/test_saves.json

if tasklist 2>/dev/null | grep -qi "daorecomp.exe"; then
    echo "The game is running: not starting a test."; exit 1
fi
if [ -e "$BAK" ]; then
    echo "$BAK exists: an earlier run didn't finish. Check it and move it back to $CFG first."; exit 1
fi
[ -f "$CFG" ] || { echo "No $CFG: run the game once first."; exit 1; }
[ -x "$EXE" ] || [ -f "$EXE" ] || { echo "No $EXE."; exit 1; }

# A record left by an interrupted run would make the user's later saves look new.
rm -f "$SAVES"
cp "$CFG" "$BAK" || exit 1
restore() {
    cp "$BAK" "$CFG" && cmp -s "$BAK" "$CFG" && rm -f "$BAK" && echo "Settings restored." ||
        echo "WARNING: settings not restored; the user's copy is $BAK."
    if [ -f "$SAVES" ]; then
        python -I scripts/test/saves.py check "$SAVES" ${KEEP_SAVES:+--keep-new} && rm -f "$SAVES"
    fi
}
trap restore EXIT
trap 'exit 130' INT TERM

python -I scripts/test/set_config.py "$BAK" "$CFG" "$@" || exit 1
python -I scripts/test/saves.py snapshot "$SAVES" || exit 1

rm -f userdata/capture_*.png
CAPTURE=${CAPTURE:-2}
CAPTURE_VAR=
[ "$CAPTURE" != 0 ] && CAPTURE_VAR="DAO_CAPTURE_EVERY=$CAPTURE"
# EXTRA is split into words on purpose (several VAR=value assignments).
# shellcheck disable=SC2086
env DAO_BACKGROUND=1 $CAPTURE_VAR ${EXTRA:-} DAO_INPUT_SCRIPT="$INPUT" \
    timeout "$SECS" "$EXE" --play > /dev/null 2>&1

mkdir -p "$OUT" || exit 1
rm -f "$OUT"/capture_*.png "$OUT"/dao.log "$OUT"/xenia.log "$OUT"/sheet.png
mv userdata/capture_*.png "$OUT"/ 2>/dev/null
cp userdata/dao.log userdata/xenia.log "$OUT"/ 2>/dev/null
echo "$(ls "$OUT" | grep -c '^capture_') captures in $OUT"
# Only two log files and no captures: the game didn't run (check the command and EXE).
grep -hiE "abort|device (removed|hung|lost)|887A000|fatal" "$OUT"/dao.log "$OUT"/xenia.log 2>/dev/null | head -5
