cmake_minimum_required(VERSION 3.16)
project(Bigodao C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ==============================================================================
# Opções
# ==============================================================================
option(WL_APPLY_WIDESCREEN_PATCH "Roda tools/inject_text.py antes de compilar" ON)
# Compila o SDL2 a partir do código-fonte em vez de usar o instalado.
# Sempre ligado no Android; opcional no Linux (útil para não depender da distro).
option(WL_SDL_FROM_SOURCE "Compila o SDL2 a partir do código-fonte" OFF)
set(WL_SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/compiler_src/SDL2-source"
    CACHE PATH "Pasta com o código-fonte do SDL2 (tarball SDL2-2.30.x.tar.gz, não o pacote -devel-mingw)")
# No Windows: ON = sem console (release). OFF = abre console com os logs (debug).
option(WL_WINDOWS_GUI "Windows: executável sem janela de console" ON)

if(ANDROID)
    set(WL_SDL_FROM_SOURCE ON)
endif()

# ==============================================================================
# SDL2
# ==============================================================================
if(WL_SDL_FROM_SOURCE)
    if(NOT EXISTS "${WL_SDL_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Código-fonte do SDL2 não encontrado em ${WL_SDL_SOURCE_DIR}.\n"
            "Rode android/setup_android.sh (ele baixa o SDL) ou passe -DWL_SDL_SOURCE_DIR=...")
    endif()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    add_subdirectory("${WL_SDL_SOURCE_DIR}" sdl2-build EXCLUDE_FROM_ALL)
    # Os headers do build ficam em <build>/include/SDL2, então <SDL.h> e <SDL2/SDL.h> funcionam.
    set(WL_SDL_LIBS SDL2::SDL2)
    if(NOT ANDROID AND TARGET SDL2::SDL2main)
        list(PREPEND WL_SDL_LIBS SDL2::SDL2main)
    endif()
elseif(WIN32)
    # Pacote oficial SDL2-devel-x.y.z-mingw (o caminho vem do mingw-toolchain.cmake).
    if(NOT WL_WINDOWS_GUI)
        set(SDL2_NO_MWINDOWS ON)
    endif()
    find_package(SDL2 REQUIRED CONFIG)
    # SDL2_LIBRARIES = SDL2main + SDL2.dll  -> exigiria SDL2.dll ao lado do .exe.
    # SDL2_STATIC_LIBRARIES = SDL2main + libSDL2.a + libs do Windows -> executável único.
    set(WL_SDL_LIBS ${SDL2_STATIC_LIBRARIES})
    include_directories(${SDL2_INCLUDE_DIRS})
else()
    # Linux: SDL2 do sistema (libsdl2-dev).
    find_package(SDL2 REQUIRED)
    if(TARGET SDL2::SDL2)
        set(WL_SDL_LIBS SDL2::SDL2)
    else()
        set(WL_SDL_LIBS ${SDL2_LIBRARIES})
    endif()
    include_directories(${SDL2_INCLUDE_DIRS})
endif()

# Ativa as diretivas gráficas de janela e som do ecossistema gb-recompiled
add_compile_definitions(GB_HAS_SDL2)

include_directories(
    .
    runtime/include
    runtime/vendor/imgui
)

# CONFIGURE_DEPENDS: arquivo .c/.cpp novo entra no build sem precisar reconfigurar à mão.
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS
    "runtime/src/*.c"
    "runtime/src/*.cpp"
    "runtime/vendor/imgui/*.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdl2.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp"
    "runtime/vendor/imgui/backends/imgui_impl_opengl3.cpp"   # passo 14a
)

# Código gerado da ROM hack. A ROM nunca entra no executável: chega em execução
# pelo launcher (ROM + IPS) ou pelo wario.o2r, e é validada pelo SHA-256 antes de
# qualquer código gerado rodar (gb_context_load_validated_rom).
# Por isso o *_rom.c (a ROM inteira em um vetor C) não pode entrar nesta lista.
set(WL_GAME_CODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/compiler_src/game_code_hack")
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS
    "${WL_GAME_CODE_DIR}/*_funcs_*.c"
    "${WL_GAME_CODE_DIR}/*_dispatch_*.c"
)

# O hack1_2_WL_SM3.c referencia rom_data só no hack1_2_WL_SM3_init(), que o jogo
# não chama. Compila-se uma cópia sem essa função e sem os externs: não sobra
# símbolo de ROM para satisfazer, e chamar o _init por engano vira erro de link.
set(WL_GAME_MAIN_SRC "${WL_GAME_CODE_DIR}/hack1_2_WL_SM3.c")
set(WL_GAME_MAIN_COPY "${CMAKE_CURRENT_BINARY_DIR}/generated/hack1_2_WL_SM3_sem_init.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WL_GAME_MAIN_SRC}")
file(READ "${WL_GAME_MAIN_SRC}" WL_GAME_MAIN_TEXT)
string(REPLACE
    "/* Extern reference to ROM data */\nextern const uint8_t rom_data[];\nextern const size_t rom_size;\n"
    "" WL_GAME_MAIN_TEXT "${WL_GAME_MAIN_TEXT}")
string(REPLACE
    "void hack1_2_WL_SM3_init(GBContext* ctx) {\n    /* Load ROM data into context */\n    gb_context_load_rom(ctx, rom_data, rom_size);\n    /* Set MBC type from header */\n    ctx->mbc_type = rom_data[0x147];\n    gb_context_reset(ctx, true);\n}\n"
    "" WL_GAME_MAIN_TEXT "${WL_GAME_MAIN_TEXT}")
string(FIND "${WL_GAME_MAIN_TEXT}" "rom_data" WL_GAME_MAIN_ROM_REF)
if(NOT WL_GAME_MAIN_ROM_REF EQUAL -1)
    message(FATAL_ERROR
        "${WL_GAME_MAIN_SRC} ainda referencia rom_data depois da remoção do _init.\n"
        "O formato gerado pelo gbrecomp mudou: ajuste os string(REPLACE) deste arquivo.")
endif()
# Só regrava se mudou, para não recompilar a cada configuração.
set(WL_GAME_MAIN_COPY_OLD "")
if(EXISTS "${WL_GAME_MAIN_COPY}")
    file(READ "${WL_GAME_MAIN_COPY}" WL_GAME_MAIN_COPY_OLD)
endif()
if(NOT WL_GAME_MAIN_COPY_OLD STREQUAL WL_GAME_MAIN_TEXT)
    file(WRITE "${WL_GAME_MAIN_COPY}" "${WL_GAME_MAIN_TEXT}")
endif()
# A cópia está fora da pasta do gerado, mas inclui o hack1_2_WL_SM3_internal.h dela.
set_source_files_properties("${WL_GAME_MAIN_COPY}" PROPERTIES
    INCLUDE_DIRECTORIES "${WL_GAME_CODE_DIR}")
list(APPEND GAME_SOURCES "${WL_GAME_MAIN_COPY}")

set(WL_ALL_SOURCES main.cpp ${RUNTIME_SOURCES} ${GAME_SOURCES})

# ==============================================================================
# Alvo principal
# ==============================================================================
if(ANDROID)
    # No Android o jogo é uma biblioteca carregada pelo SDLActivity.
    # O nome precisa ser "main" (libmain.so), que é o que o Java procura.
    add_library(wario_land_port SHARED ${WL_ALL_SOURCES})
    set_target_properties(wario_land_port PROPERTIES OUTPUT_NAME main)
    target_link_libraries(wario_land_port PRIVATE android log GLESv3)   # GLESv3: passo 14a
    # Passo 14a: o backend OpenGL do ImGui no Android usa o OpenGL ES 3.0.
    target_compile_definitions(wario_land_port PRIVATE IMGUI_IMPL_OPENGL_ES3)
elseif(WIN32)
    enable_language(RC)
    add_executable(wario_land_port ${WL_ALL_SOURCES} platform/windows/app.rc)
    if(WL_WINDOWS_GUI)
        set_target_properties(wario_land_port PROPERTIES WIN32_EXECUTABLE ON)
    endif()
    # Tudo estático: nenhuma DLL extra (libgcc, libstdc++, winpthread, SDL2).
    target_link_options(wario_land_port PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(wario_land_port PRIVATE comdlg32)
else()
    add_executable(wario_land_port ${WL_ALL_SOURCES})
    # Passo 14a: o backend OpenGL do ImGui abre o libGL com dlopen.
    target_link_libraries(wario_land_port PRIVATE ${CMAKE_DL_LIBS})
endif()

target_link_libraries(wario_land_port PRIVATE ${WL_SDL_LIBS})

# ==============================================================================
# Patch de widescreen no código gerado (roda no PC host, também no build Android)
# ==============================================================================
if(WL_APPLY_WIDESCREEN_PATCH)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
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
    else()
        message(WARNING "Python3 não encontrado: o patch de widescreen NÃO será aplicado neste build.")
    endif()
endif()