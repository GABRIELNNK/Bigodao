#!/bin/bash
set -e

echo "=================================================="
echo "🏴‍☠️ Wario Land Widescreen - Extrator & Recompilador"
echo "=================================================="

# Verificação se a ROM necessária foi fornecida na raiz
if [ ! -f "base_WL-SM3.gb" ]; then
    echo "❌ Erro: Arquivo 'base_WL-SM3.gb' nao encontrado na raiz do projeto!"
    echo "Por favor, coloque sua ROM do Wario Land aqui com o nome 'base_WL-SM3.gb'."
    exit 1
fi

# PASSO 1: Atualizar e Compilar o Submódulo gbrecomp do arcanite24
echo -e "\n🔹 Passo 1/5: Compilando a ferramenta de descompilacao gbrecomp..."
git submodule update --init --recursive
cmake -G Ninja -S gbrecomp -B gbrecomp/build
ninja -C gbrecomp/build

# PASSO 2: Executar a Descompilação (Gera o código C do jogo)
echo -e "\n🔹 Passo 2/5: Traduzindo o binario da ROM para codigo C nativo..."
mkdir -p compiler_src/game_code
./gbrecomp/build/bin/gbrecomp base_WL-SM3.gb -o compiler_src/game_code/

# PASSO 3: Deletar a pasta runtime genérica gerada automaticamente
echo -e "\n🧹 Passo 3/5: Eliminando a pasta runtime redundante gerada pelo gbrecomp..."
if [ -d "compiler_src/game_code/runtime" ]; then
    rm -rf compiler_src/game_code/runtime
fi
# Remove também o CMakeLists.txt gerado por ele, pois usaremos o seu unificado na raiz
if [ -f "compiler_src/game_code/CMakeLists.txt" ]; then
    rm -f compiler_src/game_code/CMakeLists.txt
fi

# PASSO 4: Injetar o Patch de Widescreen diretamente no código C legítimo
echo -e "\n🔹 Passo 4/5: Aplicando os patches do manifesto no codigo gerado..."
python3 compiler_src/widescreen_patches/apply_widescreen_WL-sm3.py base_WL-SM3.gb compiler_src/game_code/

# PASSO 5: Compilar o Jogo Final de PC com a SUA pasta runtime perfeita
echo -e "\n🔹 Passo 5/5: Compilando o jogo nativo de PC com a sua PPU estavel..."
cmake -G Ninja -S . -B build
ninja -C build

echo -e "\n=================================================="
echo "✅ Sucesso absoluto! O pipeline foi concluido."
echo "🎮 Para jogar o seu Wario Land Nativo em Widescreen, execute:"
echo "   ./build/wario_land_port"
echo "=================================================="
