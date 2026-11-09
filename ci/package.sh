#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Pack a built plugin for a GitHub Release:
#   ci/package.sh aarch64 0.1.0 OUTDIR    -> OUTDIR/alsa-leadtap-0.1.0-aarch64.tar.gz
set -e
ARCH=$1
VERSION=$2
OUT=$3
NAME=alsa-leadtap-$VERSION-$ARCH
SO=build/$ARCH/libasound_module_pcm_leadtap.so
[ -f "$SO" ] || { echo "$SO missing: run ci/build.sh $ARCH first" >&2; exit 1; }
rm -rf "build/$NAME"
mkdir -p "build/$NAME" "$OUT"
cp "$SO" README.md LICENSE "build/$NAME/"
tar czf "$OUT/$NAME.tar.gz" --owner=0 --group=0 -C build "$NAME"
rm -rf "build/$NAME"
echo "$OUT/$NAME.tar.gz"
