# Progresso: renderizador nativo (Bigodao)

## Feito
- **Passo 1** — Supersampling só com filtro; medição de Emu/Render; preferências de vídeo salvas; correção da marca baixa do buffer de áudio.
- **Passo 2** — Módulo `ppu_native.c/.h` (fora do GBPPU); renderizador antigo removido; Nativo padrão; Emulado e Comparar na seção "Desenvolvedor".
- **Passo 3** — Margens do widescreen otimizadas (centro pulado, sprites por linha, cache de atributos por frame).
- **Passo 4** — Frame HD no `ppu_native` (centro 160*N x 144*N, N até 4) usado direto na textura.
- **Passo 5a** — Hash dos tiles (FNV-1a 64 dos 16 bytes) e despejo em PNG (`mods/dump`, um arquivo por tile: `b_<tile>.png`, `o_<tile>.png`).
- **Passo 5a.2** — F9 captura a cena em `mods/scenes` (fundo e sprites separados, 1x e 4x, e JSON com a origem de cada pixel). `tools/hd_slice.py` fatia a cena pintada em `mods/hd` + `pack.json` (paleta de referência). Modos de conflito `primeira|comum|media` e `--mapa-conflitos`.
- **Passo 5b** (validado no jogo) — O jogo carrega os tiles HD e desenha: fundo troca só a cor (prioridade pelo tile original, alfa 0 = cor original); sprite com forma pelo alfa e pode ocupar a caixa 8x8; recoloração proporcional para fades. O fatiador converte cores para a paleta de referência. Com tiles HD a textura usa a Internal Resolution mesmo com Nearest.
- **Passo 5b.1** — Correção de link: a cópia do stb_image no `hd_pack_io.cpp` é `STB_IMAGE_STATIC` (o `main.cpp` tem a sua, JPEG + zlib).
- **Passo 5c** (compilou no usuário) — Mods em `mods/*.o2r` (ZIP com outra extensão, leitor próprio: guardado/Deflate, sem ZIP64), em ordem alfabética, e depois a pasta `mods/hd`; a de baixo sobrepõe. Menu: "Desenvolvedor" no topo, depois "Mods" (Tiles HD, Recarregar, uma caixa por mod; desligados salvos como `mods.disabled=<nome>`). `tools/hd_empacotar.py pasta saida.o2r`. Guia: `claude/COMO_CRIAR_MODS.md`.
- **Passo 5d** (entregue, aguardando teste) — Laterais do widescreen em HD. `ppu.c` continua calculando as laterais (paredes, atributos aprendidos, fade) e passa, por pixel, um `PPUNativeMarginPixel` (tile 1+banco*384+tile, uvf, raw, paleta, cores usadas, keep do fade, sprite ou caixa) para `ppu_native_margin_line()`. O `ppu_native` desenha as laterais HD (buffer 96N x 144N, `ppu_native_hd_margin_frame`) com os mesmos PNGs, recoloração e fade (tabela por coluna), confere na "Conferir amostragem HD" e grava a captura com 256 px quando o widescreen está ligado (laterais sem fade). Plataforma copia as laterais para a textura (não nas telas com laterais pretas). `hd_slice.py` aceita cena de 256.

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- Conferência HD (4a): 0 diferenças.
- 5b: pintura aparece no jogo; em 4x Emu 2,81 ms médio / 5,17 pico com tiles HD.
- 5c (testes locais): pacote .o2r = imagem idêntica à pasta; sobreposição, ligar/desligar e arquivos inválidos conferidos.
- 5d (testes locais, PPU inteira com stub de gbrt.h): conferência 0 diferenças; pacote "sem pintura" com paletas de referência aleatórias = 0 subpixels diferentes no centro e nas laterais em 40 frames (fade ligado/desligado, modos 1 e 3, paredes); captura larga → fatiador → carga = 0 diferenças; saída 1x do ppu.c idêntica à versão anterior (hash de 60 frames). Custo extra das laterais HD aqui: ~2,5–3 ms/frame em 4x com tudo em HD (mais com recoloração).

## Pendências / próximos
- Opção futura: fundo por posição no mundo (arte única por fase), sprites por tile.
- Otimizar amostragem HD (ponteiro por linha do bloco N x N) se o custo apertar.
- Limpar campos mortos do GBPPU quando outra mudança já quebrar savestates.
- GPU (OpenGL/SDL3 + shader) só se a CPU não der conta, usando a CPU como referência.
- Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.