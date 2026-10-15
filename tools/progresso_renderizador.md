# Progresso: renderizador nativo (Bigodao)

## Feito
- **Passo 1** — Supersampling só ativo quando há filtro (Linear/pós-processamento); medição de Emu/Render no overlay; preferências de vídeo salvas. Áudio: marca baixa do buffer nunca abaixo de 1,5x o bloco do dispositivo (2048).
- **Passo 2a** — Módulo `ppu_native.c/.h` (estado global, fora do GBPPU) ligado em paralelo: hooks em `ppu_begin_dot_transfer` (begin_line), fim do modo 3 (end_line), VBlank (end_frame), escrita de VRAM da CPU, HDMA e load de savestate. Menu "Renderer" + estatísticas no overlay. Preferência `display.renderer`.
- **Passo 2c** — Renderizador antigo (`ppu_render_frame_native`, cache de tiles no GBPPU) removido. Nativo é o padrão. Modo 0 = Emulado (referência). Campos antigos do GBPPU mantidos sem uso para não invalidar savestates.
- **Passo 3a** — Margens do widescreen: versão "Rápida" calcula só as margens quando o renderer é Nativo/Comparar e escolhe sprites uma vez por linha.
- **Passo 3b** — Parte 1: montagem do cache de atributos das margens mais barata (sem zerar 9 KB, VRAM direta). Parte 2: cache montado uma vez por frame; remonta se mudar câmera, mapa de BG do LCDC, modo CGB, modo de jogo ou o mapa de tiles da VRAM (`ppu_native_vram_map_generation()`). Mudanças de WRAM/ERAM no meio do frame entram no frame seguinte.
- **Passo 3c** (entregue, aguardando teste) — Remove a versão Original das margens, o Comparar delas e o combo do menu; Renderer vai para a seção recolhida "Desenvolvedor".

## Validação
- Renderer Comparar: 22.466 + 3.653 frames, **0 diferenças**, 0 escritas de VRAM sem hook.
- Margens Comparar: 701.424 linhas (3a), 1.227.456 (3b1), 2.755.296 (3b2), **0 diferenças**.
- Emu médio/pico (widescreen, Nativo): Original 3,77/11,91 ms → 3a 2,37/5,13 → 3b1 1,95/7,48 → 3b2 **1,41/3,03 ms**.

## Pendências / próximos
- Limpar campos mortos do GBPPU (`tile_cache`, `native_line`, `native_render_enabled`) junto com a próxima mudança que já quebre savestates.
- Passo 4: composição na GPU (atlas de tiles, camadas). Passo 5: troca de tiles por PNG via hash do tile. Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; não mexer no áudio durante o renderizador. Entregas como script Python que confere antes de gravar (`--conferir`).