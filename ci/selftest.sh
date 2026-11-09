#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Selftest for CI: the normal "make test" with its limits unchanged. The test runs in real
# time and a shared runner can stall a process for tens of milliseconds, so a run is repeated
# (at most ATTEMPTS times) if, and only if, every FAIL line is one of the checks listed in
# TIMING below: checks that compare a measured time or a frame count over a time window.
# A failure of any other check ends the job at once, and a green result still means one
# complete run with every check passing. A pass after a retry leaves a warning in the log.
set -e
ATTEMPTS=3
TIMING='leads play time|delay follows the slave|bounded backlog|copy resumes for a reader|is continuous|drain returns after|copy released up to the drop|new stream after prepare|continues after recovery|finishes when the audio|hidden delay: (delay|position)|a second of audio plays'

n=1
while :; do
    log=build/selftest-$n.log
    mkdir -p build
    status=0
    timeout 300 make test BUILD=build/native CC=gcc CFLAGS="-O2 -g -Wall -Wextra -Werror" >"$log" 2>&1 || status=$?
    cat "$log"
    if [ "$status" = 0 ] && tail -n 3 "$log" | grep -q 'ALL OK'; then
        if [ "$n" -gt 1 ]; then
            echo "::warning title=selftest::passed on attempt $n of $ATTEMPTS after timing-only failures (see the log above)"
        fi
        exit 0
    fi
    if ! grep -q '^FAIL ' "$log"; then
        echo "selftest stopped without a FAIL line (exit $status)" >&2
        exit 1
    fi
    if grep '^FAIL ' "$log" | grep -q -v -E "$TIMING"; then
        echo "selftest failed in a check that is not time-bound: no retry" >&2
        grep '^FAIL ' "$log" | grep -v -E "$TIMING" >&2
        exit 1
    fi
    if [ "$n" -ge "$ATTEMPTS" ]; then
        echo "selftest: timing checks failed in all $ATTEMPTS attempts" >&2
        exit 1
    fi
    echo "::warning title=selftest::attempt $n failed only in timing checks, retrying"
    n=$((n + 1))
done
