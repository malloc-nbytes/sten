#!/usr/bin/env earl

module Build

import "std/system.rl"; as sys
import "std/datatypes/list.rl";

set_flag("-xe", "--show-lets");

let release = false;
try { release = argv()[1] == "release"; }

let files = List::to_str(sys::get_all_files_by_ext(".", "c"));
let libs = "$(pkg-config --libs ncurses) $(forge lib)";
let flags = "$(pkg-config --cflags ncurses) -Wextra -Wall " + case release of {
    true = "-DRELEASE";
    _ = "-O0 -ggdb";
};

if release {
    $f"cc {flags} -o sten {files} {libs}";
} else {
    $f"cc {flags} -o sten {files} {libs}";
}
