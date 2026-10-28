#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
app="../RID工作台.app"
mkdir -p "$app/Contents/MacOS"
cp Info.plist "$app/Contents/Info.plist"
swiftc -O main.swift -framework AppKit -framework WebKit -framework IOKit -o "$app/Contents/MacOS/RIDClient"
