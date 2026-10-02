cmake_minimum_required(VERSION 3.16)
project(WarioLandWidescreenPort C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Busca a biblioteca gráfica SDL2 instalada no Linux
find_package(SDL2 REQUIRED)

# 🌟 CRUCIAL: Ativa as diretivas gráficas de janela e som do ecossistema gb-recompiled
add_compile_definitions(GB_HAS_SDL2)

include_directories(
    .
    runtime/include
    runtime/vendor/imgui
    ${SDL2_INCLUDE_DIRS}
)

file(GLOB RUNTIME_SOURCES
    "runtime/src/*.c"
    "runtime/src/*.cpp"
    "runtime/vendor/imgui/*.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdl2.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp"
)

# Coleta os arquivos de funções e dados do jogo, mantendo o base_WL_SM3_main.c de fora
file(GLOB GAME_SOURCES
    "compiler_src/game_code/*_funcs_*.c"
    "compiler_src/game_code/*_dispatch_*.c"
    "compiler_src/game_code/base_WL_SM3.c"
    "compiler_src/game_code/*_rom.c"
)

add_executable(wario_land_port 
    main.cpp 
    ${RUNTIME_SOURCES} 
    ${GAME_SOURCES}
)

# Vincula o executável final com os drivers binários do SDL2 do sistema operacional
target_link_libraries(wario_land_port PRIVATE ${SDL2_LIBRARIES})
