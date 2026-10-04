cmake_minimum_required(VERSION 3.16)
project(WarioLandWidescreenPort C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Busca a biblioteca gráfica SDL2 instalada no Linux
find_package(SDL2 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

# Coleta o código gerado da ROM hack; a imagem carregada em execução vem do IPS.
file(GLOB GAME_SOURCES
    "compiler_src/game_code_hack/*_funcs_*.c"
    "compiler_src/game_code_hack/*_dispatch_*.c"
    "compiler_src/game_code_hack/hack1_2_WL_SM3.c"
    "compiler_src/game_code_hack/*_rom.c"
)

add_executable(wario_land_port 
    main.cpp 
    ${RUNTIME_SOURCES} 
    ${GAME_SOURCES}
)

# Aplica as flags estáticas APENAS quando estiver compilando para o Windows (via MinGW)
if(WIN32)
    target_link_options(wario_land_port PRIVATE "-static-libgcc" "-static-libstdc++" "-static")
    target_link_libraries(wario_land_port PRIVATE comdlg32)
endif()

add_custom_target(apply_widescreen_hack_fov_patch
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/inject_text.py"
        --map "${CMAKE_CURRENT_SOURCE_DIR}/tools/widescreen_hack_fov_30_patch.json"
        --dir "${CMAKE_CURRENT_SOURCE_DIR}/compiler_src/game_code_hack"
        --apply
        --no-backup
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)
add_dependencies(wario_land_port apply_widescreen_hack_fov_patch)

# Vincula o executável final com os drivers binários do SDL2 do sistema operacional
target_link_libraries(wario_land_port PRIVATE ${SDL2_LIBRARIES})
