#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the plugin with warnings as errors, from the repository root:
#   ci/build.sh native|aarch64|armhf  -> build/<arch>/libasound_module_pcm_leadtap.so
# The cross builds compile against a copy of the host ALSA headers; the plugin links no
# libasound, its symbols resolve at load time inside the host application.
set -e
ARCH=$1
CFLAGS="-O2 -g -Wall -Wextra -Werror"
case "$ARCH" in
    native)  CC=gcc;                     READELF=readelf;                     MACHINE="X86-64|AArch64" ;;
    aarch64) CC=aarch64-linux-gnu-gcc;   READELF=aarch64-linux-gnu-readelf;   MACHINE=AArch64 ;;
    armhf)   CC=arm-linux-gnueabihf-gcc; READELF=arm-linux-gnueabihf-readelf; MACHINE=ARM ;;
    *) echo "usage: ci/build.sh native|aarch64|armhf" >&2; exit 2 ;;
esac

INC=build/alsainc-$ARCH
rm -rf "$INC"
mkdir -p "$INC"
cp -r /usr/include/alsa "$INC/"
make plugin CC="$CC" BUILD="build/$ARCH" ALSA_INC="$PWD/$INC" CFLAGS="$CFLAGS"

SO=build/$ARCH/libasound_module_pcm_leadtap.so
"$READELF" -h "$SO" | grep -E "Machine:.*($MACHINE)"
echo "built $SO"
