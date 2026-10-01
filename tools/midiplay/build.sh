#!/bin/sh
# Build the midiplay command (macOS)
cd "$(dirname "$0")" && swiftc -O main.swift -o midiplay && echo "built: $(pwd)/midiplay"
