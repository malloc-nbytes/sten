#!/bin/bash

set -xe

files=$(find . -type f -name '*.c')
cflags="$(pkg-config --cflags ncurses) -Wextra -Wall -ggdb -O0 -o sten"
libs="$(pkg-config --libs ncurses) $(forge lib)"

cc $cflags -o sten $files $libs
