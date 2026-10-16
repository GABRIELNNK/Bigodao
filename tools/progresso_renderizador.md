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
- **Passo 6a** (testado: saturou) — Mixer no `platform_sdl.cpp`: reamostragem linear 32.32 do buffer do jogo no callback. Na VM do usuário o relógio difere ~1,6% do áudio.
- **Passo 6a.1** — Integral (até ±3%) no ritmo dos frames (`g_audio_pace_ppm` em `gb_platform_vsync`); proporcional (±0,5%) na leitura do buffer.
- **Passo 6a.2** (validado: sem underrun) — Porta desliga o LCD; a fatia de 70224 ciclos termina sem `frame_done` (`gb_add_cycles` marca `stopped`) e o `main.cpp` não chamava vsync. `main.cpp`: `else if (ctx->frame_lcd_off_cycles > 0) { gb_platform_render_lcd_off_frame(); gb_platform_vsync(ctx->frame_cycles); }`. Mixer: integral limitado (erro máx. 25%, ganho 0,0003) + proporcional no ritmo (1%).
- **Passo 6a.3** (validado) — Chuvisco nas trocas de cena: `render_frame_internal` ignorava `framebuffer_is_widescreen`. Agora respeita e, com o LCD desligado, reaproveita o frame HD (`reuse_hd_frame`).
- **Passo 6b** (validado) — Monitor de música, só leitura: `$A61C` = pedido, `$A61D` = música tocando (RAM do cartucho, `ctx->eram`). Overlay, Desenvolvedor > "Musicas vistas", terminal "[Musica] ...". O valor no frame 1 é sobra do save (RAM com bateria).
- **Passo 6c.1** (entregue, aguardando teste) — Laboratório de áudio: `gb_audio_set_channel_mute_mask` (bit por canal, só tira da mistura em `audio_emit_sample`), `gb_audio_get_channel_info` (ligado, volume, freq, saída NR51, disparos); contadores fora do `GBAudio` (save state igual). Vigia de 64 bytes da RAM do cartucho (padrão `$A600`) com destaque do que mudou.

## Músicas identificadas ($A61D)
- 00 = silêncio; FF = parada/transição (não são músicas)
- 20 = abertura
- 26 = escolha do slot de save
- 15 = mapa de fases de Rice Beach
- 02 = mapa geral (escolha de região)
- 2B = jingle de entrada na fase (curto, ~1 s; seguido de 00)
- 04 = fase 1 de Rice Beach
- 08 = Wario morre (jingle)

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- 5b: em 4x Emu 2,81 ms médio / 5,17 pico com tiles HD. 5d: Emu 3,94 med com conferência ligada.
- 6a.1/6a.2 (simulação): derivas de -2,5% a +2,5% estáveis. Sintaxe checada com headers SDL2/ImGui.

## Pendências / próximos
- 6c.1: descobrir quais canais a música usa e como o driver marca canal ocupado por efeito (bytes perto de `$A600`/`$A615`).
- 6c: trilhas `.ogg` por música em `.o2r` (loop por JSON), silenciando só a música do GB.
- Despejo gravar `mods/dump/pack.json` com as paletas (adiado pelo usuário).
- Opção futura: fundo por posição no mundo; otimizar amostragem HD; limpar campos mortos do GBPPU; GPU só se necessário.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.