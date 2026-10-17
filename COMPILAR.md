# Compilar — Linux, Windows e Android

Todos os alvos usam o mesmo `CMakeLists.txt`.

## Linux (como antes)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Opcional: `-DWL_SDL_FROM_SOURCE=ON` compila o SDL2 de `compiler_src/SDL2-source`
em vez de usar o `libsdl2-dev` da distro.

## Windows (cross-compilado do Linux com MinGW-w64)

Pré-requisito: `SDL2-devel-2.30.0-mingw.tar.gz` extraído em `compiler_src/SDL2-2.30.0`.

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

O `.exe` sai **sem precisar de `SDL2.dll`** nem das DLLs do MinGW (só DLLs do
próprio Windows). Para depurar com console aparecendo os logs:
`-DWL_WINDOWS_GUI=OFF`.

O manifesto em `platform/windows/app.manifest` liga UTF-8 como code page do
processo, para ROMs e saves em pastas com acento (ex.: `C:\Users\João\...`).

## Android

Uma vez só:

```bash
bash android/setup_android.sh
```

Isso baixa o código-fonte do SDL2 para `compiler_src/SDL2-source`, copia as
classes Java do SDL, ícones e Gradle wrapper para `android/`, e aplica as
mudanças de Android em `main.cpp` e `runtime/src/platform_sdl.cpp`
(`tools/aplicar_port_android.py`, que guarda um `.orig` de cada arquivo).

Depois:

```bash
cd android
./gradlew assembleRelease
adb install -r app/build/outputs/apk/release/app-release.apk
```

Precisa de JDK 17, Android SDK (`ANDROID_HOME` ou `android/local.properties`
com `sdk.dir=...`), NDK e CMake 3.22.1 — o SDK Manager do Android Studio
instala tudo. Também dá para abrir a pasta `android/` direto no Android Studio.

### Como funciona no celular

- **Primeira execução:** o launcher abre; "Browse ROM"/"Browse Patch" abrem o
  seletor de arquivos do Android. O arquivo é copiado para dentro do app e
  validado pelo SHA-256, como no PC. Depois disso o jogo inicia direto pelo
  `wario.o2r` (armazenamento interno do app).
- **Controles:** gamepad na tela (D-pad, A, B, Start, Select e botão ☰ de menu).
  Some sozinho quando um controle Bluetooth/USB está conectado e volta ao tocar
  na tela. Liga/desliga em Menu → Controls → *Touch Controls*.
- **Botão Voltar:** abre/fecha o menu.
- **Mods e log:** `Android/data/com.bigodao.warioland/files/` (acessível pelo
  gerenciador de arquivos ou USB). A pasta de mods aparece no menu *Mods*.
  O log da última execução fica em `last_run.log` nessa mesma pasta.
- Saves e preferências ficam no armazenamento interno do app.
