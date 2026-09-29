#!/bin/sh
# Build and run the host test, from any directory.
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I.. -o nixie_render_test nixie_render_test.c ../nixie_render.c
./nixie_render_test
