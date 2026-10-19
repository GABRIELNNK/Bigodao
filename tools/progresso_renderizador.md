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

- **Passo 6d** (validado) — Pasta solta `mods/music/` (fonte "music/", por último); caminhos das pastas relativos a `mods/`. `[Musica]` no terminal só com Desenvolvedor > Musicas vistas > "Registrar no terminal" (`music_replace_set_log`; erros sempre). Preferências `music.enabled`, `music.volume_percent`, `music.log`. Guia ganhou tabela de faixas (MarioWiki) com números a descobrir.

- **Passo 6e** (validado) — Volume do GB separado: `g_audio_gb_volume_percent` (0..200, pref `audio.gb_volume_percent`); GB usa master×GB/100 (`gb_effective_volume_percent`) nos dois caminhos do callback; trilhas usam só o master × volume das trilhas. Slider "Game Boy Volume (%)" no menu Audio e "Volume do GB" no menu Mods (quando há trilhas).

- **Passo 7a** (validado) — Gravador de poses de sprites em `hd_pack_io.cpp` (`hd_pack_io_sprite_rec_*`): com a gravação ligada, a plataforma pede uma cena por frame (`g_scene_for_recorder`) e entrega ao gravador; componentes 8-conexos de `obj_tile` viram poses; assinatura FNV por tile_hash+uvf (e a espelhada, `mirror_uvf`); folha em prateleiras (largura 320, folga 4) gravada como cena `mods/scenes/sprites_NNN.json` + `_obj.png` + `_obj_4x.png` (bg vazio), fatiável pelo `hd_slice.py --obj` sem mudanças. Retângulo (padrão 40,24,80,96 na tela 160x144, centro da pose) desenhado na tela; aviso "REC poses". F9 desligado durante a gravação. Testado fora do jogo: deduplicação, espelho, retângulo e fatiamento.

- **Passo 7b** (validado) — Achado no sprites_001 do usuário: o jogo espelha o Wario pelo hardware (mesmos tiles com flip X), mas a pose da direita tem um pedaço a mais (`…9db8`); havia poses repetidas porque o jogo troca a ordem dos pedaços sobrepostos. Gravador: assinatura pela lista de pedaços (canto da caixa = x-u, y-v; tile_hash; flips), espelho com x' = w-8-x. `hd_slice.py`: amostra igual ao original (comparando com o `_obj.png` 1x) não vota se alguma ocorrência foi pintada; `--todas-votam` desliga. Testado fora do jogo.

- **Passo 7c** (entregue, aguardando teste) — Pintura vazia: `hd_slice.py` faz uma passada por caixa (x-u, y-v, tile) e ignora ocorrências sem nenhum ponto com alfa ≥ 128; tile sem ocorrência pintada não é gravado (avisa quantos); no fundo, ponto transparente não é pintura (fica a base). `--manter-vazios` volta ao antigo. `hd_empacotar.py` deixa de fora `o_*.png` 100% transparentes (avisa; `--manter-vazios`).
- **Passo 7d** (entregue, aguardando teste) — Grade de referência (`write_grade_png` em `hd_pack_io.cpp`): `scene_NNN_bg_grade_4x.png`, `scene_NNN_obj_grade_4x.png`, `sprites_NNN_grade_4x.png`; caixa por ocorrência, número = primeira entrada do `.json` com o mesmo nome (kind+hash), amarelo repetido / azul único, sufixo x/y; fonte 3x5 em escala 2 com contorno. Usuário escolheu número em todas as caixas também no fundo.

- **Ajuste manual do usuário (2026-10-17)** — `platform_sdl.cpp`, `gb_mode_is_side_scrolling`: `case 2` (Mode_LevelInit) removido → modo 2 inteiro com laterais pretas. Motivo: o modo 2 cobre a tela "COURSE No.XX" (VRAM com a fonte → laterais do mapa de blocos saíam "000000") e os primeiros quadros da fase (tilemap da VRAM sem as colunas fora da tela → lixo listrado). `ppu_widescreen_uses_level_layout` (ppu.c) continua com `mode == 2 || mode == 3`. Efeito colateral: laterais pretas por uma fração de segundo na entrada da fase.

- **Passo 7e** (entregue, aguardando teste) — Laterais: atributo CGB do bloco agora vem 1) do cache do frame (tilemap VRAM), 2) de `ws_block_known`, 3) da tabela DX da ROM (`ppu_dx_attribute_from_rom`, indexada só por `$A800`), 4) de `ws_tile_known`. Antes a tabela da ROM vinha primeiro e errava em salas com outro jogo de tiles (caverna após porta: blocos `…a31e…` marrom/amarelo na lateral direita, `scene_001.json` do usuário).

- **Passo 7f** (entregue, aguardando teste) — Causa real da paleta errada na lateral direita: `has_current_map_attribute` aceitava o atributo da VRAM só conferindo o número do tile; fora da tela o tilemap guarda restos, e o tile liso (`…a31e…`) é usado por blocos com paletas diferentes. Erro começava em x=216 da cena (8 px dentro da lateral, fim da coluna que o jogo prepara). Agora a VRAM só vale dentro de [câmera−8, câmera+160+8) (e idem na vertical), em `ppu_fetch_warioland_world_pixel` e no aprendizado do cache (`ppu_build_widescreen_block_attribute_cache_fast`). O 7e (ordem cache → bloco aprendido → ROM → tile) continua.

- **Passo 7g** (validado na caverna) — Depois do 7f, blocos da lateral ainda não vistos (água `…37ca…`/`…a7e2…` e borda `…484e…`/`…ba94…` em `scene_002.json`) caíam na tabela DX pela fase e saíam com paleta errada (teal no lugar de azul, vermelho-escuro no lugar de teal). Agora `ppu_dx_calibrate` escolhe, a cada frame, a página 0x40–0x7F da tabela (0x84000 + página<<8 + tile) que bate com ≥90% dos tiles visíveis (atributo real da VRAM); log `[DX] tabela de paletas das laterais: pagina XX (fase YY, N/M)`. Ordem: VRAM fresca → cache → tabela calibrada → bloco aprendido → tabela pela fase → tile. Zera ao entrar no modo 2; volta a -1 após 30 frames sem bater.

- **Passo 7h** (entregue) — Diagnóstico: Desenvolvedor > "Laterais: mostrar origem da cor" tinge cada pixel da lateral pela origem do atributo (sem tinta VRAM, verde cache, azul tabela calibrada, amarelo bloco aprendido, magenta tabela pela fase, vermelho palpite do tile, cinza nada) e mostra a página DX calibrada. API em ppu.h: `ppu_set/get_widescreen_attr_debug`, `ppu_get_dx_page`. Motivo: `scene_003.json` (praia/mar) com onda marrom na lateral direita; observação: na captura, a paleta registrada no `.json` para tiles das laterais nem sempre é a usada no desenho (o PNG é a referência).

- **Passo 7i** (validado: onda certa) — Diagnóstico 7h na praia: faixa errada da direita (céu/onda) em amarelo = `ws_block_known` com cor de outra sala; nenhuma área azul (página DX calibrada não usada ali). Agora `ws_block_known`, `ws_tile_known` e `g_dx_page` zeram também quando `ctx->total_lcd_off_span_count` muda (porta/troca de sala). Se continuar amarelo errado sem ter passado por porta, suspeitar da máscara `& 0x7F` do block_id (blocos 0x80+ colidindo com 0x00+).

- **Passo 7j** (validado: céu certo) — Diagnóstico: céu acima das ondas na lateral direita em cinza (nenhuma origem). Nova origem antes da tabela pela fase e do palpite: `ppu_ws_nearest_fresh_attr` procura o mesmo tile na parte atualizada do tilemap, mesma linha ou a mais próxima (até ±20), lado esquerdo/direito conforme a lateral; tabelas `s_ws_near_left/right[32][256]` montadas uma vez por frame (`ctx->completed_frames`). Diagnóstico: ciano. Ordem atual: VRAM fresca → cache → tabela DX calibrada → bloco aprendido → mesmo tile perto da tela → tabela pela fase → palpite do tile.

- **Passo 7k** (entregue) — `scene_004.json` (início da praia, navio na esquerda). A página calibrada só exige 90% dos tiles distintos batendo, então erra alguns. Agora a montagem do cache registra, para cada tile visível no frame, o atributo mostrado (`g_dx_screen_state/attr[256]`: 0 não aparece, 1 um atributo só, 2 vários); se a tabela calibrada contradiz um tile com atributo único na tela, vale a tela (origem 7, ciano). Legenda: "ciano = cor do mesmo tile na tela (7j/7k)".

- **Passo 7l** (aplicado; o usuário deixou o bloco comentado, como opção) — Laterais pretas nas linhas em que a janela (HUD) cobre o centro inteiro: `ws_hud_band` em `ppu_render_widescreen_scanline_fast`. O usuário comentou o `if (margin && ws_hud_band)` (com nota de cor branca/preta) para ativar no futuro. Atrás do HUD fica a última fileira de blocos da fase, que no jogo original nunca aparece.

- **Passo 7m** (aplicado) — Empate na calibração: antes ganhava a página de número mais baixo; agora a da fase (0x41+`$A800`), depois a atual. Log `[DX] ... N/M tiles, K paginas empatadas, e a da fase`. Não era empate: o log do usuário mostrou o problema real (7n).

- **Passo 7n** (validado pelo usuário: "parece que resolveu todos que eu notei"; caverna não reportada) — Barris da praia da fase 1 (fase 10) brancos/azuis na lateral direita antes de aparecerem (`scene_006.json` = barril no centro, cor real marrom/laranja; `scene_007.json` = barril na lateral sem cor). Log: início da fase página 51 (a da fase, 19/19); no trecho do barril a calibração troca para 52 com 20/22 (1 página, sem empate) → barris errados; andando volta à 51 (17/17). Agora a página da fase fica mesmo perdendo, se perde por até 3 tiles e explica ≥75% da tela (aceita com 75% em vez de 90%). Log extra: `[DX]   pagina da fase XX: a/b tiles (mantida: perdeu por pouco)`. Tiles visíveis que a página da fase erra continuam corrigidos pelo 7k.

- **Passo 7o** (removido pelo 7p: não fez nada) — Tentou separar o modo 1 em 11/12 pela música e pintar a faixa do mapa pela janela (WY/WX). A faixa do nome da região não é janela, por isso nada mudou. O usuário pediu para tirar tudo.

- **Passo 7p** (entregue, aguardando teste) — Tira o 7o inteiro (ppu.c, ppu.h e platform_sdl.cpp voltam ao estado do 7n, conferido por diff). Achado do usuário no log `[DX]`: página da fase 43 = mapa geral; 44 = mapa regional 1 (Rice Beach); 46 = regional 2; 4A = regional especial 1. Regra nova em `ppu_render_widescreen_scanline_fast`: `ws_map_band` = modo `$A8C3` == 1 && `0x41 + $A800` != 0x43 && `ly >= 136` (última fileira de tiles, faixa do nome da região) → laterais brancas (0xFFFFFFFF), também no registro HD (`ws_rec`).

## Mapa (achados)
- No mapa (modo 1), `$A800` indica qual mapa: página DX = 0x41 + `$A800`; 43 (`$A800`=02) = mapa geral; 44 (03) = Rice Beach; 46 (05) = regional 2; 4A (09) = regional especial 1.
- A faixa do nome da região (linhas 136–143) é fundo, não janela.

## Revisões futuras
- **Laterais no modo 2 (refinar):** achar o sub-estado do Mode_LevelInit que separa "COURSE na tela" de "fase já na tela" (vigiar `$A8C0`–`$A8FF` no Laboratório de áudio durante a transição, ou procurar no disassembly perto de `CheckMode`/bank01.asm). Regra final: laterais da fase no modo 3, ou no modo 2 quando o sub-estado indicar fase visível; laterais pretas na tela do COURSE.
- **Sprites cortados na coluna -8..-1 da lateral esquerda:** `ppu_ws_collect_line_sprites` (ppu.c) descarta OAM X == 0 (`current_x == 0`), mas no widescreen essa coluna é visível. Teste sugerido: tirar `current_x == 0` do filtro (manter `current_y == 0`). Se aparecerem pedaços soltos parados na coluna, o jogo usa X = 0 para esconder → desenhar X = 0 só quando o objeto tem pedaços vizinhos visíveis. Aguardando resultado do teste do usuário.
- **Faixa do HUD (7l):** opção comentada no código; alternativa não escolhida: estender a faixa de pedras do HUD até as bordas.
- **Caverna após o 7n:** se a página da fase explicar ≥75% lá e a água voltar a errar, ajustar o limite com o log.


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
- Despejo gravar `mods/dump/pack.json` com as paletas (em parte coberto pelo 7a para sprites).
- Opção futura: fundo por posição no mundo; otimizar amostragem HD; limpar campos mortos do GBPPU; GPU só se necessário.
- Regra de trabalho: uma mudança por vez, teste e commit entre elas; entregas como script Python com `--conferir`.