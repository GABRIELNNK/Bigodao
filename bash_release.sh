#!/bin/bash

# Encerra o script imediatamente se qualquer comando falhar
set -e

echo "=== Compilando Linux x86_64 ==="
cmake --build build

echo "=== Compilando Windows ==="
cmake --build build-win

#echo "=== Compilando Linux ARM64 (Anbernic / PortMaster) ==="
#cmake --build build-arm64

echo "=== Compilando Android ==="
cd android
./gradlew assembleRelease
cd ..

echo "Build process completed successfully."