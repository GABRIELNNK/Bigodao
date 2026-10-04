# Define o sistema operacional e arquitetura de destino
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Define os compiladores cruzados do MinGW-w64 no Ubuntu
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

# ==============================================================================
# !!! CONFIGURAÇÃO DO SDL2 !!!
# Substitua o caminho abaixo pela pasta Raiz onde você extraiu o SDL2 do Windows.
# Geralmente a estrutura oficial do tar.gz contém a subpasta 'x86_64-w64-mingw32'.
# ==============================================================================
set(SDL2_PATH "compiler_src/SDL2-2.30.0/x86_64-w64-mingw32/lib/cmake/SDL2")



# Diz ao CMake exatamente onde procurar o pacote SDL2Config.cmake
set(CMAKE_PREFIX_PATH "${SDL2_PATH}")
# ==============================================================================


# Permite o uso do Python nativo do Ubuntu durante o build (usado pelo script inject_text.py)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Restringe a busca de bibliotecas de código C/C++ estritamente para o ambiente Windows
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)