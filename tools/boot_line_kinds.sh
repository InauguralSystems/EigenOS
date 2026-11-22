#!/bin/sh
# Validate number kinds in the two boot verdicts whose text exposes HAL integers.
set -u
log=${1:?usage: boot_line_kinds.sh SERIAL_LOG}
fail=0
# COM1 lines end in CRLF: the carriage return is made here, because `\r` inside the pattern is not one.
cr=$(printf '\r')
rtc='^\[m12\] rtc: [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} UTC( .*)? OK'"$cr"'?$'
# rtt is a whole count of ticks; loss is a ratio (lost / sent), so it may carry a fraction.
icmp='^\[m13-2\] icmp: .* rtt [0-9]+ ms loss [0-9]+(\.[0-9]+)?( \([^)]*\))? \[[0-9]+/[0-9]+\] OK'"$cr"'?$'
grep -qE "$rtc" "$log" || { echo "boot-gate: FAIL: no m12 RTC verdict of the form YYYY-MM-DD HH:MM:SS UTC in whole numbers"; fail=1; }
grep -qE "$icmp" "$log" || { echo "boot-gate: FAIL: no m13-2 ICMP verdict with rtt as a whole number of ms and loss as a number"; fail=1; }
[ "$fail" -eq 0 ]
