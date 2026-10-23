#!/bin/bash
# floppywatch: built with the ps5-payload-dev SDK and MSX PS5's own floppy reader (ps5/frontend/fe_floppy.c).
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk bash ps5/tools/floppywatch/build.sh   -> ps5/tools/floppywatch/floppywatch.elf
set -e
here=$(cd "$(dirname "$0")" && pwd)
front="$here/../../frontend"
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK}"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -O2 -I"$front" -o "$here/floppywatch.elf" "$here/floppywatch.c" "$front/fe_floppy.c" \
  -lSceSystemService -lSceUserService
ls -la "$here/floppywatch.elf"
