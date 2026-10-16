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
- **Passo 5c** (entregue, aguardando teste) — Mods em `mods/*.o2r` (ZIP com outra extensão, leitor próprio: guardado/Deflate, sem ZIP64), em ordem alfabética, e depois a pasta `mods/hd`; a de baixo sobrepõe. Em cada fonte valem `b_/o_<tile>.png` em qualquer subpasta e os `pack.json`. Menu: "Desenvolvedor" no topo, depois "Mods" (Tiles HD, Recarregar, uma caixa por mod; desligados salvos como `mods.disabled=<nome>`), depois o resto. `tools/hd_empacotar.py pasta saida.o2r`.

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- Conferência HD (4a): 0 diferenças.
- 5b: pintura aparece no jogo; em 4x Emu 2,81 ms médio / 5,17 pico com tiles HD.
- 5c (testes locais): pacote .o2r (do empacotador e do `zip` de linha de comando) = imagem idêntica à pasta; sobreposição, ligar/desligar e arquivos inválidos conferidos.

## Pendências / próximos
- HD nas margens do widescreen.
- Opção futura: fundo por posição no mundo (arte única por fase), sprites por tile.
- Limpar campos mortos do GBPPU quando outra mudança já quebrar savestates.
- GPU (OpenGL/SDL3 + shader) só se a CPU não der conta, usando a CPU como referência.
- Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.