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

echo "=== Check binarios ==="
APK=$(ls -t android/app/build/outputs/apk/release/*.apk | head -n 1)
python3 tools/verificar_rom_no_binario.py build/wario_land_port build-win/wario_land_port.exe "$APK"

echo "Build process completed successfully."