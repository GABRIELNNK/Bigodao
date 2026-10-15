# Progresso: renderizador nativo (Bigodao)

## Feito
- **Passo 1** — Supersampling só ativo quando há filtro; medição de Emu/Render no overlay; preferências de vídeo salvas. Áudio: marca baixa do buffer nunca abaixo de 1,5x o bloco do dispositivo (2048).
- **Passo 2 (a, c)** — Módulo `ppu_native.c/.h` (estado global, fora do GBPPU), hooks de linha/frame/VRAM/HDMA/savestate; renderizador antigo removido; Nativo é o padrão; Emulado (referência) e Comparar ficam na seção "Desenvolvedor".
- **Passo 3 (a, b, c)** — Margens do widescreen só na versão rápida: centro pulado no Nativo, sprites escolhidos por linha, cache de atributos montado uma vez por frame (remonta com câmera, LCDC, CGB, modo de jogo ou escrita no mapa da VRAM).
- **Passo 4a** — Frame HD no `ppu_native` (centro 160*N x 144*N, N até 4). Cada pixel guarda tile, posição no tile, flip, paleta e prioridade; "Conferir amostragem HD" usa o caminho por subpixel (o dos PNGs).
- **Passo 4b** — Textura usa o centro HD; escala HD segue a escala real da textura (`effective_internal_scale`). Opção "Usar frame HD na tela".
- **Passo 5a** (entregue, aguardando teste) — Hash FNV-1a 64 dos 16 bytes do tile e FNV-1a 32 das 4 cores da paleta da linha; despejo em PNG 8x8 em `<pasta do jogo>/mods/dump` com nomes `b_<tile>_<paleta>.png` (fundo, opaco) e `o_<tile>_<paleta>.png` (sprite, cor 0 transparente). Novos: `hd_pack_io.cpp/.h`, `third_party/stb_image*.h`.

## Validação
- Renderer Comparar: 0 diferenças; margens Comparar: 0 diferenças em milhões de linhas.
- Emu médio/pico (widescreen, Nativo): 3,77/11,91 ms (original) → 1,41/3,03 ms (passo 3).
- Conferência HD (4a): 6.599 frames, 0 diferenças; Emu 2,72/4,48 ms com a conferência ligada.

## Decisões
- Composição HD na CPU agora; GPU (OpenGL/SDL3 + shader) só depois, usando a CPU como referência.
- Regras planejadas para o 5b: no fundo, o PNG só troca a cor (prioridade continua pelo tile original); no sprite, o alfa do PNG define a transparência. Arquivo sem paleta (`b_<tile>.png`) vale para qualquer paleta.

## Pendências / próximos
- 5b: carregar `mods/hd/*.png` (stb_image), qualquer tamanho múltiplo de 8, e desenhar no caminho por subpixel; forçar textura em Nx quando houver pacote.
- HD nas margens do widescreen.
- Limpar campos mortos do GBPPU (`tile_cache`, `native_line`, `native_render_enabled`) quando outra mudança já quebrar savestates.
- Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python que confere antes de gravar (`--conferir`).