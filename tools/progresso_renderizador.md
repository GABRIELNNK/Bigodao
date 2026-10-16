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
- **Passo 5d** (validado) — Laterais do widescreen em HD via `PPUNativeMarginPixel` / `ppu_native_margin_line()`; captura larga (256) com F9.
- **Passo 6a / 6a.1** — Mixer no `platform_sdl.cpp` (reamostragem linear 32.32); integral (até ±3%) no ritmo dos frames (`g_audio_pace_ppm`), proporcional (±0,5%) na leitura. Relógio da VM difere ~1,6% do áudio.
- **Passo 6a.2** (validado: sem underrun) — LCD desligado (portas): `main.cpp` faz `else if (ctx->frame_lcd_off_cycles > 0) { gb_platform_render_lcd_off_frame(); gb_platform_vsync(ctx->frame_cycles); }`. Integral limitado + proporcional no ritmo.
- **Passo 6a.3** (validado) — Frame repetido com LCD desligado lido com a largura certa (`framebuffer_is_widescreen`) e em HD (`reuse_hd_frame`).
- **Passo 6b** (validado) — Monitor de música: `$A61C` pedido, `$A61D` tocando (RAM do cartucho). Overlay, Desenvolvedor > "Musicas vistas", terminal "[Musica] ...".
- **Passo 6c.1** (validado) — Laboratório de áudio: mudo por canal (`gb_audio_set_channel_mute_mask`), estado dos canais (`gb_audio_get_channel_info`), vigia de 64 bytes da RAM do cartucho (padrão `$A600`).
- **Passo 6c.2** (validado: CH1 e CH4 separam bem música e efeitos) — Portão por canal em `audio_emit_sample` (tocar sempre / mudo / só com efeito quando byte da RAM do cartucho != 0). Configuração final: CH1 em `$A601`, CH2 e CH3 mudos, CH4 em `$A616`.
- **Passo 6c** (validado: trilha e .o2r empacotado funcionando) — Trilhas .ogg: `music_replace.cpp/.h` (stb_vorbis v1.22 em `runtime/vendor/stb`, compilado por `stb_vorbis_impl.c`). Arquivos `music/XX*.ogg` nos mods; loop por tags LOOPSTART/LOOPLENGTH/LOOPEND ou `music/music.json` (loop_start, loop_end, loop, volume). `hd_pack_io` ganhou `hd_pack_io_list_files`, `hd_pack_io_read_file` (lê só o pedaço do .o2r) e `hd_pack_io_generation`. Platform: segue `$A61D` (ignora os 8 primeiros frames), atrasa a trilha pelo nível do buffer do GB, liga o portão enquanto a trilha toca (`g_audio_lab_gate || music_replace_active()`), mixa no callback só quando o GB produziu áudio (pausa junto). Menu Mods: "Trilhas .ogg", volume e lista. `tools/musica_empacotar.py`. Precisa `cmake -S . -B build` (arquivos novos, GLOB). Testado fora do jogo: atraso, loop, tags, jingle sem loop, troca com fade, 44,1/48 kHz, mono.

- **Passo 6d** (entregue, aguardando teste) — Pasta solta `mods/music/` (fonte "music/", por último); caminhos das pastas relativos a `mods/`. `[Musica]` no terminal só com Desenvolvedor > Musicas vistas > "Registrar no terminal" (`music_replace_set_log`; erros sempre). Preferências `music.enabled`, `music.volume_percent`, `music.log`. Guia ganhou tabela de faixas (MarioWiki) com números a descobrir.

## Áudio do jogo (achados)
- Músicas (`$A61D`): 00 silêncio; FF parada/transição; 20 abertura; 26 escolha do slot; 15 mapa de Rice Beach; 02 mapa geral; 2B jingle de entrada na fase; 04 fase 1 de Rice Beach; 08 Wario morre.
- CH1 = efeitos (moeda/pulo/pisar) + melodia; CH4 = efeitos (passos/correr/quebrar) + bateria. CH2 e CH3 só tocam música: os sons de entrada na fase e de moedas ao morrer são as próprias músicas 2B e 08.
- `$A630`–`$A637` parece o estado da música: `$A633` = nº de canais e depois a lista (04 01 02 03 04 na fase; 03 01 02 03 no 2B; 02 01 02 no 08).
- Ao morrer: `$A601`=10 e `$A603`=1B (efeitos), `$A629` muda de 21 para 10.
- Correndo (efeito no CH4): mudam `$A616`, `$A618`, `$A626` e `$A63B`–`$A63F`; `$A629` = 21. Hipótese: `$A600`/`$A615` = pedido de efeito (Data Crystal), byte seguinte = efeito tocando.

## Validação
- Renderer e margens: Comparar com 0 diferenças.
- Emu (widescreen, Nativo): 3,77/11,91 ms → 1,41/3,03 ms (passo 3).
- 5b: em 4x Emu 2,81 ms médio / 5,17 pico com tiles HD. 5d: Emu 3,94 med com conferência ligada.
- 6a.1/6a.2 (simulação): derivas de -2,5% a +2,5% estáveis. Sintaxe checada com headers SDL2/ImGui.

## Pendências / próximos
- Trilhas: abrir o .ogg fora da thread principal se a troca travar; ir completando a tabela de números das músicas.
- Despejo gravar `mods/dump/pack.json` com as paletas (adiado pelo usuário).
- Opção futura: fundo por posição no mundo; otimizar amostragem HD; limpar campos mortos do GBPPU; GPU só se necessário.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.