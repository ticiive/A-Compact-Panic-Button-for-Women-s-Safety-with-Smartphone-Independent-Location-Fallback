#!/bin/sh
set -eu
cd "$(dirname "$0")"
app="build/Painel.app"
mkdir -p "$app/Contents/MacOS"
cp Info.plist "$app/Contents/"
swiftc -parse-as-library -O Painel.swift -o "$app/Contents/MacOS/Painel"
codesign --force --sign - "$app"
printf '%s\n' "Aplicativo criado em macos/$app"
