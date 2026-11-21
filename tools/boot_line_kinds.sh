#!/bin/sh
# Validate number kinds in the two boot verdicts whose text exposes HAL integers.
set -u
log=${1:?usage: boot_line_kinds.sh SERIAL_LOG}
fail=0
rtc='^\[m12\] rtc: [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} UTC( .*)? OK\r?$'
# rtt is a whole count of ticks; loss is a ratio (lost / sent), so it may carry a fraction.
icmp='^\[m13-2\] icmp: .* rtt [0-9]+ ms loss [0-9]+(\.[0-9]+)?( \([^)]*\))? \[[0-9]+/[0-9]+\] OK\r?$'
grep -qE "$rtc" "$log" || { echo "boot-gate: FAIL: m12 RTC verdict is not whole-number YYYY-MM-DD HH:MM:SS UTC"; fail=1; }
grep -qE "$icmp" "$log" || { echo "boot-gate: FAIL: m13-2 ICMP verdict: rtt is not a whole number of ms, or loss is not a number"; fail=1; }
[ "$fail" -eq 0 ]
