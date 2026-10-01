cmake_minimum_required(VERSION 3.16)
project(WarioLandWidescreenPort C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Encontra o SDL2 no computador do usuário
find_package(SDL2 REQUIRED)

# Inclui os diretórios de cabeçalhos
include_directories(
    .
    runtime/include
    runtime/vendor/imgui
    ${SDL2_INCLUDE_DIRS}
)

# Coleta todos os arquivos de código da engine runtime e do ImGui
file(GLOB RUNTIME_SOURCES
    "runtime/src/*.c"
    "runtime/src/*.cpp"
    "runtime/vendor/imgui/*.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdl2.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp"
)

# NOVO CAMINHO PROTEGIDO: Busca a lógica gerada localmente pelo usuário
file(GLOB GAME_SOURCES
    "compiler_src/game_code/*.c"
)

# Cria o executável unificando a engine com o código do jogo patcheado
add_executable(wario_land_port 
    main.cpp 
    ${RUNTIME_SOURCES} 
    ${GAME_SOURCES}
)

# Linka as dependências de biblioteca
target_link_libraries(wario_land_port PRIVATE ${SDL2_LIBRARIES})
