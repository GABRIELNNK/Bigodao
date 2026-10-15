# Progresso: renderizador nativo (Bigodao)

## Feito
- **Passo 1** — Supersampling só ativo quando há filtro (Linear/pós-processamento); medição de Emu/Render no overlay; preferências de vídeo salvas. Áudio: marca baixa do buffer nunca abaixo de 1,5x o bloco do dispositivo (2048).
- **Passo 2a** — Módulo `ppu_native.c/.h` (estado global, fora do GBPPU) ligado em paralelo: hooks em `ppu_begin_dot_transfer` (begin_line), fim do modo 3 (end_line), VBlank (end_frame), escrita de VRAM da CPU, HDMA e load de savestate. Menu "Renderer" + estatísticas no overlay. Preferência `display.renderer`.
- **Passo 2c** — Renderizador antigo (`ppu_render_frame_native`, cache de tiles no GBPPU) removido. Nativo é o padrão. Modo 0 = Emulado (referência). Campos antigos do GBPPU mantidos sem uso para não invalidar savestates.

## Validação
- Comparar: 22.466 frames + 3.653 frames após o 2c, **0 diferenças**, 0 escritas de VRAM sem hook.
- Nativo x antigo: Emu médio 3,16 x 3,36 ms (empate; o antigo também já pulava os pixels emulados).

## Pendências / próximos
- Margens do widescreen (`ppu_render_widescreen_scanline`): recalcula o cache de atributos (32x32) e varre 40 sprites por pixel em cada uma das 144 linhas — principal custo restante.
- Limpar campos mortos do GBPPU (`tile_cache`, `native_line`, `native_render_enabled`) junto com a próxima mudança que já quebre savestates.
- Passo 4: composição na GPU (atlas de tiles, camadas). Passo 5: troca de tiles por PNG via hash do tile. Passo 6: mixer de áudio com controle de taxa.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; não mexer no áudio durante o renderizador.
