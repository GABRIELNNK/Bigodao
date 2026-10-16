# Progresso: renderizador nativo e áudio (Bigodao)

## Feito
- **Passo 1** — Supersampling só com filtro; medição de Emu/Render; preferências de vídeo salvas; correção da marca baixa do buffer de áudio.
- **Passo 2** — Módulo `ppu_native.c/.h` (fora do GBPPU); renderizador antigo removido; Nativo padrão; Emulado e Comparar na seção "Desenvolvedor".
- **Passo 3** — Margens do widescreen otimizadas (centro pulado, sprites por linha, cache de atributos por frame).
- **Passo 4** — Frame HD no `ppu_native` (centro 160*N x 144*N, N até 4) usado direto na textura.
- **Passo 5a** — Hash dos tiles (FNV-1a 64 dos 16 bytes) e despejo em PNG (`mods/dump`, `b_<tile>.png`, `o_<tile>.png`).
- **Passo 5a.2** — F9 captura a cena em `mods/scenes`; `tools/hd_slice.py` fatia a cena pintada em `mods/hd` + `pack.json`.
- **Passo 5b** (validado) — Tiles HD no jogo: fundo troca só a cor; sprite com forma pelo alfa; recoloração proporcional para fades.
- **Passo 5b.1** — `STB_IMAGE_STATIC` no `hd_pack_io.cpp` (o `main.cpp` tem a sua cópia).
- **Passo 5c** (validado) — Mods em `mods/*.o2r` + pasta `mods/hd`; menu "Desenvolvedor" e "Mods" no topo; `tools/hd_empacotar.py`. Guia: `claude/COMO_CRIAR_MODS.md`.
- **Passo 5d** (validado: "HD: 4x + laterais", conferência 0 diferenças em 2.477 frames) — Laterais do widescreen em HD via `PPUNativeMarginPixel` / `ppu_native_margin_line()`; captura larga (256) com F9.
- **Passo 6a** (testado: saturou) — Mixer no `platform_sdl.cpp` (audio.c não muda): reamostragem linear 32.32 do buffer do jogo no callback. Na VM do usuário o relógio difere ~1,6% do áudio; ajuste de ±0,5% saturou.
- **Passo 6a.1** (testado: underruns ~8 s depois de portas) — Integral (até ±3%) no ritmo dos frames (`g_audio_pace_ppm` em `gb_platform_vsync`); proporcional (±0,5%) na leitura do buffer.
- **Passo 6a.2** (validado: sem underrun) — Porta desliga o LCD; a fatia de 70224 ciclos termina sem `frame_done` (`gb_add_cycles` marca `stopped`) e o `main.cpp` não chamava vsync → jogo sem freio → rajada de áudio. `main.cpp`: `else if (ctx->frame_lcd_off_cycles > 0) { gb_platform_render_lcd_off_frame(); gb_platform_vsync(ctx->frame_cycles); }`. Mixer: integral limitado (erro máx. 25%, ganho 0,0003) + proporcional no ritmo (1%).
- **Passo 6a.3** (entregue, aguardando teste) — Efeito colateral do 6a.2: chuvisco (linhas horizontais) nas trocas de cena. `gb_platform_render_lcd_off_frame` passava o frame largo (256 px) e `render_frame_internal` ignorava `framebuffer_is_widescreen` (lia como 160 px). Agora respeita o parâmetro e, com o LCD ainda desligado, reaproveita o frame HD (`reuse_hd_frame`). Corrige de quebra `gb_platform_present_framebuffer` no widescreen.

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- 5b: em 4x Emu 2,81 ms médio / 5,17 pico com tiles HD. 5d: Emu 3,94 med com conferência ligada.
- 6a.1/6a.2 (simulação): derivas de -2,5% a +2,5% estáveis; caso do usuário (+1,6%) com rajadas sem underrun. Sintaxe checada com headers SDL2/ImGui.

## Pendências / próximos
- 6b: achar na RAM o número da música pedida ao driver de som; mostrar no overlay.
- 6c: trilhas `.ogg` por música em `.o2r` (loop por JSON), silenciando só os canais de música do GB.
- Despejo gravar `mods/dump/pack.json` com as paletas (adiado pelo usuário).
- Opção futura: fundo por posição no mundo; otimizar amostragem HD; limpar campos mortos do GBPPU; GPU só se necessário.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.