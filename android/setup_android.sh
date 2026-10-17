#!/usr/bin/env bash
# Prepara o projeto Android (rodar uma vez, de qualquer pasta):
#   1. baixa o código-fonte do SDL2 para compiler_src/SDL2-source
#      (o pacote SDL2-devel-mingw que você já tem NÃO serve: só tem as libs do Windows)
#   2. copia as classes Java do SDL (org.libsdl.app), os ícones padrão e o Gradle wrapper
#   3. aplica as mudanças de Android em main.cpp e platform_sdl.cpp
set -euo pipefail

SDL_VERSION="${SDL_VERSION:-2.30.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/compiler_src/SDL2-source"
TEMPLATE="$SRC/android-project"

if [ ! -f "$SRC/CMakeLists.txt" ]; then
    echo ">> Baixando SDL2 $SDL_VERSION (código-fonte)..."
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fL -o "$TMP/sdl.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz"
    tar xzf "$TMP/sdl.tar.gz" -C "$TMP"
    mkdir -p "$ROOT/compiler_src"
    rm -rf "$SRC"
    mv "$TMP/SDL2-${SDL_VERSION}" "$SRC"
else
    echo ">> SDL2 já está em $SRC"
fi

echo ">> Copiando classes Java do SDL (precisam ser da mesma versão da lib nativa)..."
mkdir -p "$HERE/app/src/main/java/org/libsdl/app"
cp "$TEMPLATE"/app/src/main/java/org/libsdl/app/*.java "$HERE/app/src/main/java/org/libsdl/app/"

echo ">> Copiando ícones padrão (troque depois pelos seus)..."
for d in "$TEMPLATE"/app/src/main/res/mipmap-*; do
    dest="$HERE/app/src/main/res/$(basename "$d")"
    mkdir -p "$dest"
    cp -n "$d"/* "$dest/" 2>/dev/null || true
done

echo ">> Copiando o Gradle wrapper..."
mkdir -p "$HERE/gradle/wrapper"
cp "$TEMPLATE/gradlew" "$TEMPLATE/gradlew.bat" "$HERE/"
cp "$TEMPLATE"/gradle/wrapper/* "$HERE/gradle/wrapper/"
chmod +x "$HERE/gradlew"

echo ">> Aplicando mudanças de Android no código C++..."
python3 "$ROOT/tools/aplicar_port_android.py"

cat <<EOF

Pronto. Para compilar:
  cd "$HERE"
  ./gradlew assembleRelease
APK: app/build/outputs/apk/release/app-release.apk
Instalar no celular (depuração USB ligada):  adb install -r app/build/outputs/apk/release/app-release.apk

Precisa de: Android SDK (ANDROID_HOME ou local.properties com sdk.dir), NDK e CMake 3.22.1
(instaláveis pelo SDK Manager do Android Studio) e JDK 17.
EOF
