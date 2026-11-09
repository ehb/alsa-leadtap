#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Print the CHANGELOG.md section of a version; fails if there is none.
#   ci/release_notes.sh 0.1.0
set -e
notes=$(awk -v v="## $1" '$0 == v {found = 1; next} /^## / {found = 0} found {print}' CHANGELOG.md)
[ -n "$notes" ] || { echo "CHANGELOG.md has no section for $1" >&2; exit 1; }
printf '%s\n' "$notes"
