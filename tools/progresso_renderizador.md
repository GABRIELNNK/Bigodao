# Progresso: renderizador nativo (Bigodao)

## Feito
- **Passo 1** — Supersampling só com filtro; medição de Emu/Render; preferências de vídeo salvas; correção da marca baixa do buffer de áudio.
- **Passo 2** — Módulo `ppu_native.c/.h` (fora do GBPPU); renderizador antigo removido; Nativo padrão; Emulado e Comparar na seção "Desenvolvedor".
- **Passo 3** — Margens do widescreen otimizadas (centro pulado, sprites por linha, cache de atributos por frame).
- **Passo 4** — Frame HD no `ppu_native` (centro 160*N x 144*N, N até 4) usado direto na textura.
- **Passo 5a** — Hash dos tiles (FNV-1a 64 dos 16 bytes) e despejo em PNG (`mods/dump`, um arquivo por tile: `b_<tile>.png`, `o_<tile>.png`).
- **Passo 5a.2** — F9 captura a cena em `mods/scenes` (fundo e sprites separados, 1x e 4x, e JSON com a origem de cada pixel). `tools/hd_slice.py` fatia a cena pintada em `mods/hd` + `pack.json` (paleta de referência). Modos de conflito `primeira|comum|media` e `--mapa-conflitos`.
- **Passo 5b** (entregue, aguardando teste) — O jogo carrega `mods/hd` (stb_image) e desenha: fundo troca só a cor (prioridade pelo tile original, alfa 0 = cor original); sprite com forma pelo alfa e pode ocupar a caixa 8x8; recoloração proporcional para fades. O fatiador converte cores para a paleta de referência quando o mesmo tile aparece com paletas diferentes. Opção "Tiles HD (mods/hd)" + "Recarregar" em Graphics; com tiles HD a textura usa a Internal Resolution mesmo com Nearest.

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- Conferência HD (4a): 0 diferenças.
- 5b (testes locais): pacote gerado pelo fatiador sem pintura = imagem idêntica ao original ampliado, inclusive com tiles em várias paletas e em fades; custo medido aqui em 4x: 0,7 ms sem HD, 2,8 ms com a tela toda em HD, 4,9 ms durante fade.

## Pendências / próximos
- HD nas margens do widescreen.
- Opção futura: fundo por posição no mundo (arte única por fase), sprites por tile.
- Limpar campos mortos do GBPPU quando outra mudança já quebrar savestates.
- GPU (OpenGL/SDL3 + shader) só se a CPU não der conta, usando a CPU como referência.
- Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.