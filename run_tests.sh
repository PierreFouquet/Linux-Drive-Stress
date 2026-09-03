#!/usr/bin/env bash
#
# Regression tests for the drive stress tools.
#
#   ./run_tests.sh              build and run the functional tests
#   ./run_tests.sh --sanitizers also run under ASan/UBSan and TSan (slower)
#
# Writes only inside a temporary directory, which is removed on exit.
# Exits non-zero if any check fails.

set -u

ROOT=$(cd "$(dirname "$0")" && pwd)
SINGLE="$ROOT/drive_stress_linux"
MULTI="$ROOT/stress_test_multi"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
SANITIZERS=0
[ "${1:-}" = "--sanitizers" ] && SANITIZERS=1

ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s: %s\n' "$1" "$2"; }
want() { # description, actual, expected
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', want '$3'"; fi
}

# Run a tool, echo its exit status. Never lets a failure abort the script.
status() { "$@" >/dev/null 2>&1; echo $?; }

# Corrupt one granule of a file behind the tool's back.
corrupt() { # file, granule index, granule size
    dd if=/dev/urandom of="$1" bs="$3" seek="$2" count=1 conv=notrunc \
       oflag=direct status=none 2>/dev/null ||
    dd if=/dev/urandom of="$1" bs="$3" seek="$2" count=1 conv=notrunc status=none
    sync
}

echo "== build =="
if make -C "$ROOT" clean >/dev/null 2>&1 &&
   make -C "$ROOT" CFLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -std=c99" \
        >"$WORK/build.log" 2>&1; then
    if grep -qE 'warning|error' "$WORK/build.log"; then
        bad "builds with no warnings" "$(grep -cE 'warning|error' "$WORK/build.log") diagnostics"
        grep -E 'warning|error' "$WORK/build.log" | head -5
    else
        ok "builds clean under -Wall -Wextra -Wpedantic -Wshadow -Wconversion"
    fi
else
    bad "build" "compilation failed"; cat "$WORK/build.log"; exit 1
fi

echo "== basic operation =="
want "--help exits 0"          "$(status "$SINGLE" --help)" 0
want "single file, 2 iters"    "$(status "$SINGLE" 32 "$WORK/a.dat" --iterations 2 --mixed-sec 1)" 0
want "multi file, 4 threads"   "$(status "$MULTI" 16 4 --path "$WORK/m_" --iterations 2 --mixed-sec 1)" 0
want "buffered fallback"       "$(status "$SINGLE" 16 "$WORK/a.dat" --iterations 1 --mixed-sec 1 --no-direct)" 0
want "random phase disabled"   "$(status "$SINGLE" 16 "$WORK/a.dat" --iterations 1 --mixed-sec 0)" 0
want "verification disabled"   "$(status "$SINGLE" 16 "$WORK/a.dat" --iterations 1 --mixed-sec 1 --no-verify)" 0
want "non-default block sizes" "$(status "$SINGLE" 32 "$WORK/a.dat" --iterations 1 --mixed-sec 1 --seq-block 4096 --rand-block 64)" 0
want "--recreate"              "$(status "$MULTI" 16 3 --path "$WORK/m_" --recreate --iterations 2 --mixed-sec 1)" 0
want "--duration bound"        "$(status "$SINGLE" 16 "$WORK/a.dat" --duration 4 --mixed-sec 1)" 0

echo "== read/write mix =="
mix() { grep -oE '[0-9]+ rd / [0-9]+ wr' "$1" | head -1; }
"$SINGLE" 16 "$WORK/a.dat" --iterations 1 --mixed-sec 2 --read-pct 100 >"$WORK/r100.txt" 2>&1
want "read-pct 100 issues no writes" "$(mix "$WORK/r100.txt" | awk '{print $4}')" 0
"$SINGLE" 16 "$WORK/a.dat" --iterations 1 --mixed-sec 2 --read-pct 0 >"$WORK/r0.txt" 2>&1
want "read-pct 0 issues no reads"    "$(mix "$WORK/r0.txt" | awk '{print $1}')" 0

echo "== guards and argument validation =="
want "rejects bad --read-pct"   "$(status "$SINGLE" --read-pct 500)" 1
want "rejects mismatched blocks" "$(status "$SINGLE" --seq-block 5 --rand-block 4)" 1
want "rejects unknown option"   "$(status "$SINGLE" --bogus x)" 1
want "rejects non-numeric size" "$(status "$SINGLE" abc)" 1
want "rejects missing value"    "$(status "$SINGLE" --size-mb)" 1
want "rejects impossible size"  "$(status "$MULTI" 99999999 4 --path "$WORK/big_")" 1
mkdir -p "$WORK/adir"
want "refuses a directory"      "$(status "$SINGLE" 16 "$WORK/adir")" 1
if [ -e /dev/loop0 ] || [ -e /dev/null ]; then
    want "refuses a device node" "$(status "$SINGLE" 16 /dev/null)" 1
fi

echo "== data integrity =="
# Corrupt a granule mid-run, with a read-only random phase so nothing
# legitimately rewrites it. The tool must notice and exit 2.
"$SINGLE" 32 "$WORK/c.dat" --iterations 1 --mixed-sec 6 --read-pct 100 >"$WORK/c.log" 2>&1 &
CPID=$!
sleep 2
corrupt "$WORK/c.dat" 1234 4096
wait $CPID
want "corruption exits 2" "$?" 2
HITS=$(grep -c 'offset 5054464' "$WORK/c.log")
if [ "$HITS" -ge 1 ]; then
    ok "corruption reported at exact offset (1234 x 4096, $HITS reads)"
else
    bad "corruption reported at exact offset" "no report for offset 5054464"
fi
want "run marked FAIL" "$(grep -c 'result *FAIL' "$WORK/c.log")" 1

# A badly failing drive must not flood the terminal.
"$SINGLE" 32 "$WORK/d.dat" --iterations 1 --mixed-sec 6 --read-pct 100 >"$WORK/d.log" 2>&1 &
DPID=$!
sleep 2
dd if=/dev/urandom of="$WORK/d.dat" bs=4096 seek=1 count=2000 conv=notrunc status=none; sync
wait $DPID
LINES=$(grep -c 'DATA CORRUPTION: .*offset' "$WORK/d.log")
if [ "$LINES" -le 20 ] && [ "$LINES" -gt 0 ]; then
    ok "mismatch output bounded ($LINES lines)"
else
    bad "mismatch output bounded" "$LINES detail lines, want 1..20"
fi
want "suppression notice shown" "$(grep -c 'further reports suppressed' "$WORK/d.log")" 1

echo "== interruption =="
rm -f "$WORK"/i_*
"$MULTI" 32 3 --path "$WORK/i_" >/dev/null 2>&1 &
IPID=$!
sleep 3
kill -INT $IPID
wait $IPID
want "SIGINT exits 0"        "$?" 0
want "SIGINT leaves no files" "$(ls "$WORK"/i_* 2>/dev/null | wc -l)" 0

if [ "$SANITIZERS" = 1 ]; then
    echo "== sanitizers =="
    for san in "address,undefined" "thread"; do
        BIN="$WORK/san_$(echo "$san" | tr ',' '_')"
        if gcc -fsanitize="$san" -g -O1 -std=c99 -o "$BIN" \
               "$ROOT/stress_test_multi.c" "$ROOT/drive_stress.c" \
               -I"$ROOT" -lpthread >/dev/null 2>&1; then
            "$BIN" 16 4 --path "$WORK/s_" --iterations 2 --mixed-sec 1 \
                   >"$WORK/san.log" 2>&1
            want "clean under -fsanitize=$san" \
                 "$(grep -cE 'ERROR: |runtime error|WARNING: ThreadSanitizer' "$WORK/san.log")" 0
        else
            echo "  SKIP -fsanitize=$san (unsupported by this compiler)"
        fi
    done
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
