# Cross-compilação para Windows x64 a partir do Linux (MinGW-w64).
# Uso:
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Pacote oficial SDL2-devel-2.30.0-mingw extraído em compiler_src/.
# Caminho absoluto a partir deste arquivo, então funciona com qualquer pasta de build.
set(WL_SDL2_MINGW_ROOT "${CMAKE_CURRENT_LIST_DIR}/compiler_src/SDL2-2.30.0/x86_64-w64-mingw32")
list(APPEND CMAKE_PREFIX_PATH "${WL_SDL2_MINGW_ROOT}")
list(APPEND CMAKE_FIND_ROOT_PATH "${WL_SDL2_MINGW_ROOT}" "/usr/x86_64-w64-mingw32")

# Python do host (usado pelo inject_text.py durante o build)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Bibliotecas e headers só do ambiente Windows
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
