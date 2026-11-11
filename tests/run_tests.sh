#!/usr/bin/env bash
# Automated tests for the MC68VZ328 UART/TIMER emulator.
#
# Run from anywhere; the script changes to the repository root.
set -u

cd "$(dirname "$0")/.."

TMP=$(mktemp -d "${TMPDIR:-/tmp}/vz328-tests.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

pass() {
    echo "PASS: $1"
    PASS=$((PASS + 1))
}

fail() {
    local name=$1
    shift
    echo "FAIL: $name"
    for line in "$@"; do
        echo "      $line"
    done
    FAIL=$((FAIL + 1))
}

show_file() {
    tr -d '\r' <"$1" | sed 's/^/      /'
}

# Check that a file contains a literal string.
contains() {
    tr -d '\r' <"$1" | grep -Fq -- "$2"
}

# Run a program in direct-load mode on stdio.
# Usage: direct_load NAME ABS EXPECTED [INPUT] [EXPECTED_RC]
direct_load() {
    local name=$1
    local abs=$2
    local expected=$3
    local input=${4-}
    local want_rc=${5-0}
    local rc=0
    local out="$TMP/$name.out"
    local err="$TMP/$name.err"

    if [ -n "$input" ]; then
        printf '%s' "$input" | timeout 10 ./m68k-vz328 --load "$abs" --fast \
            >"$out" 2>"$err" || rc=$?
    else
        timeout 10 ./m68k-vz328 --load "$abs" --fast </dev/null \
            >"$out" 2>"$err" || rc=$?
    fi

    if [ "$rc" -ne "$want_rc" ]; then
        fail "$name" "exit status $rc (expected $want_rc)"
        show_file "$err"
        return
    fi
    if ! contains "$out" "$expected"; then
        fail "$name" "missing output: $expected"
        show_file "$out"
        show_file "$err"
        return
    fi
    pass "$name"
}

# Run one of the PTY based scenarios.
pty_test() {
    local name=$1
    local mode=$2
    local abs=$3
    local expected=$4
    local out="$TMP/$name.out"
    local err="$TMP/$name.err"

    if python3 tests/pty_test.py "$mode" ./m68k-vz328 "$abs" \
            "$expected" >"$out" 2>"$err"; then
        pass "$name"
    else
        fail "$name" "$(cat "$err")"
        show_file "$out"
    fi
}

echo "== UART and timer tests =="

direct_load uart_poll tests/uart_poll.abs "Hello from UART polling!"
direct_load uart_irq_min tests/uart_irq_min.abs "Minimal UART IRQ OK!" "abc"
direct_load timer_irq tests/timer_irq.abs "Timer 1 interrupt OK!"
direct_load timer2_irq tests/timer2_irq.abs "Timer 2 interrupt OK!"

# UART1 receive interrupt echo test: the echoed characters must appear as
# well as the final message.
if printf 'abq' | ./m68k-vz328 --load tests/uart_irq.abs --fast \
        >"$TMP/uart_irq.out" 2>"$TMP/uart_irq.err"; then
    if contains "$TMP/uart_irq.out" "abq" &&
       contains "$TMP/uart_irq.out" "UART 1 interrupt OK!"; then
        pass uart_irq
    else
        fail uart_irq "missing echoed input or final message"
        show_file "$TMP/uart_irq.out"
    fi
else
    fail uart_irq "emulator exited with an error"
    show_file "$TMP/uart_irq.err"
fi

# Unhandled exceptions must hit the ROM stub and exit with status 2.
rc=0
./m68k-vz328 --load tests/exception.abs --fast </dev/null \
    >"$TMP/exception.out" 2>"$TMP/exception.err" || rc=$?
if [ "$rc" -eq 2 ] && contains "$TMP/exception.err" "Unhandled exception"; then
    pass exception
else
    fail exception "expected exit status 2 and an unhandled exception report"
    show_file "$TMP/exception.err"
fi

echo "== IPL S-record loader =="

cat tests/ipl_hello.abs >"$TMP/ipl.in"
printf 'x' >>"$TMP/ipl.in"
rc=0
./m68k-vz328 --ipl --stdio --fast <"$TMP/ipl.in" \
    >"$TMP/ipl.out" 2>"$TMP/ipl.err" || rc=$?
if [ "$rc" -eq 0 ] &&
   contains "$TMP/ipl.out" "*** Load Success ***" &&
   contains "$TMP/ipl.out" "*** your program start. ***" &&
   contains "$TMP/ipl.out" "IPL upload OK!"; then
    pass ipl_stdio
else
    fail ipl_stdio "IPL upload or program output failed"
    show_file "$TMP/ipl.out"
    show_file "$TMP/ipl.err"
fi

# Kermit commonly sends CR line endings when transmit linefeed is disabled.
tr '\n' '\r' <tests/ipl_hello.abs >"$TMP/ipl-cr.in"
printf 'x' >>"$TMP/ipl-cr.in"
rc=0
./m68k-vz328 --ipl --stdio --fast <"$TMP/ipl-cr.in" \
        >"$TMP/ipl-cr.out" 2>"$TMP/ipl-cr.err" || rc=$?
if [ "$rc" -eq 0 ] &&
   contains "$TMP/ipl-cr.out" "*** Load Success ***" &&
   contains "$TMP/ipl-cr.out" "IPL upload OK!"; then
    pass ipl_stdio_cr
else
    fail ipl_stdio_cr "CR-terminated S-record upload failed"
    show_file "$TMP/ipl-cr.out"
    show_file "$TMP/ipl-cr.err"
fi

echo "== PTY tests =="

pty_test pty_uart1 load tests/uart_irq.abs "UART 1 interrupt OK!"
pty_test pty_uart2 uart2 tests/uart2_poll.abs "Hello from UART 2!"
pty_test pty_ipl ipl tests/ipl_hello.abs "IPL upload OK!"

echo
echo "----------------------------------------"
echo "Passed: $PASS  Failed: $FAIL"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
exit 0
