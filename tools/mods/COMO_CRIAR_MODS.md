# Bigodão: como criar e aplicar mods HD

Este guia mostra como trocar os gráficos do jogo por versões em alta resolução, pintadas por você, sem mexer na ROM. A seção 6 mostra como trocar as músicas por trilhas `.ogg`.

A ideia é esta: o jogo grava uma "foto" da cena e você pinta por cima dela. Um script corta a pintura em tiles. Os tiles ficam num arquivo `.o2r`, que o jogo carrega pelo menu.

```
F9 no jogo  →  pintar  →  hd_slice.py  →  testar  →  hd_empacotar.py  →  meu_mod.o2r
```

---

## 1. Aplicar um mod pronto

Esta seção é para quem só quer usar um mod.

1. Copie o arquivo `.o2r` para a pasta `mods`, que fica ao lado do executável do jogo. Se a pasta não existir, crie.
   ```
   <pasta do jogo>/
   ├── wario_land_port      (ou .exe)
   ├── wario.o2r            ← é o jogo; não mexa
   └── mods/
       └── meu_mod.o2r      ← o mod vai aqui
   ```
2. Abra o jogo e aperte **Esc** para abrir o menu.
3. Em **Mods**:
   - deixe **Tiles HD** marcado;
   - confira se a caixa do mod está marcada;
   - se o jogo já estava aberto quando você copiou o arquivo, clique em **Recarregar**.
4. Em **Graphics & Supersampling**, escolha **Internal Resolution 2x, 3x ou 4x**. Em 1x os tiles HD não aparecem.

Ao lado de cada mod aparece quantos tiles ele carregou ou, se houver problema, a mensagem de erro.

### Vários mods ao mesmo tempo

- A lista mostra primeiro os `.o2r` em ordem alfabética e, no fim, a pasta `mods/hd`, se existir.
- **Quando o mesmo tile está em mais de um mod, vale o que está mais embaixo na lista.**
- Para mudar a ordem, renomeie os arquivos com um número na frente: `01_cenario.o2r`, `02_wario.o2r`, `03_ajustes.o2r`.
- Desmarcar a caixa de um mod tira o mod na hora. Essa escolha fica salva para a próxima vez.

---

## 2. Criar um mod

### O que você precisa

- O jogo compilado, com o passo 5c ou mais novo.
- Python 3 com Pillow:
  ```
  pip install pillow        # ou: sudo apt install python3-pil
  ```
- Um editor de imagem com camadas e transparência: Krita, GIMP, Aseprite, Photoshop etc.

### 2.1 Capturar a cena

1. No jogo, coloque a **Internal Resolution em 4x**. Isso não é obrigatório para capturar, mas facilita testar depois.
2. Vá até o ponto que quer redesenhar. Pode pausar com o menu.
3. Aperte **F9** (ou use Menu > Desenvolvedor > **Capturar cena (F9)**).

A captura vai para `mods/scenes`. Com o **widescreen ligado**, a cena sai com 256 px de largura, incluindo as laterais, e você pinta as laterais também. As laterais saem sem o escurecimento da borda, que o jogo aplica sozinho por cima. Com o widescreen desligado, a cena tem 160 px.

| Arquivo | Para que serve |
|---|---|
| `scene_001_bg_4x.png` | **Fundo**, ampliado 4x. É sobre ele que você pinta. |
| `scene_001_obj_4x.png` | **Sprites** (Wario, inimigos, itens), ampliados 4x, com fundo transparente. É sobre ele que você pinta. |
| `scene_001_bg.png` / `scene_001_obj.png` | As mesmas imagens em 1x. Servem de referência, e o fatiador também usa. Não edite. |
| `scene_001.json` | Diz de qual tile veio cada pixel. Não edite. |
| `scene_001_bg_grade_4x.png` / `scene_001_obj_grade_4x.png` | **Grade de referência** (veja abaixo). Só para ajudar a pintar; não edite nem entregue ao fatiador. |

O número aumenta a cada captura: 001, 002…

**Grade de referência.** Ponha o `_grade_4x.png` como uma camada por cima do desenho (com a camada travada ou só visível). Ele tem:
- uma caixa em volta de cada tile, na posição real dele (acompanha a rolagem do cenário);
- o **número do tile** no `.json`, o mesmo em todo lugar onde o tile se repete;
- caixa **amarela** = o tile aparece mais de uma vez nesta camada (pintou um, pintou todos); caixa **azul** = aparece só uma vez;
- **`x`** ou **`y`** depois do número = aquela ocorrência está espelhada na horizontal ou na vertical (é o mesmo desenho, virado).

O mesmo tile com outra paleta tem o mesmo número. No fundo e nos sprites a contagem é separada, mas o número é o mesmo da lista `tiles` do `.json`.

> Dica: o fundo e os sprites saem separados. Por isso, os sprites não atrapalham a pintura do fundo, e o fundo não atrapalha a dos sprites.

### 2.2 Pintar

Abra o `_bg_4x.png` e/ou o `_obj_4x.png` e pinte **por cima**, sem mudar o tamanho e sem deslocar nada. Cada bloco de 32×32 px da imagem 4x é um tile do jogo, que tem 8×8.

**Regras do fundo** (`_bg_4x.png`)
- A pintura é sempre opaca. A transparência é ignorada.
- O PNG só troca a **cor**. Quem passa na frente ou atrás dos sprites continua sendo decidido pelo jogo original.

**Regras dos sprites** (`_obj_4x.png`)
- A **transparência define a forma**: alfa abaixo de 128 é vazio, acima é desenho.
- Cada pedaço de sprite ocupa uma caixa de 8×8 no jogo, ou 32×32 na imagem 4x. Você pode pintar **em qualquer lugar dentro dessa caixa**, até onde o original era transparente. Fora das caixas a pintura é ignorada.

**Outras escalas**
- Pode pintar em 2x, 3x, 5x, 8x… desde que a imagem tenha o tamanho da cena vezes N: **160·N × 144·N**, ou **256·N × 144·N** numa captura com widescreen.
- As duas camadas precisam estar na mesma escala.
- Se pintar numa escala maior, amplie o `_4x.png` com vizinho mais próximo (nearest) antes de começar. Um tile 8x vira 64×64 px.

**Tiles repetidos**
- O jogo reaproveita o mesmo tile em vários lugares, como blocos, chão e nuvens, e também **em fases diferentes e com paletas diferentes**. Por isso, o fundo que você pinta vale, por padrão, **só na fase da captura e só com a mesma paleta** (seção 2.3b). Os sprites valem em todo lugar.
- Dentro dessa fase e dessa paleta existe um único PNG por tile. Se você pintar duas ocorrências do mesmo tile de jeitos diferentes, só uma pode valer. Na seção 2.3 está como escolher qual.
- O jogo também reaproveita tiles **espelhados**. O fatiador desfaz o espelhamento sozinho. Basta pintar o que você vê.
- Para variar a mesma coisa em lugares diferentes (dois tijolos iguais com rachaduras diferentes), por enquanto não há jeito: o tile é um só.

**Cores e fades**
- Pinte com as cores que estão na tela no momento da captura.
- Quando o jogo muda a paleta (escurecer, piscar, transição de fase), o tile HD é recolorido na mesma proporção. Isso usa a paleta de referência que o fatiador grava no `pack.json`.
- Evite capturar no meio de um fade: a cena escura vira a referência.

**Pode pintar numa camada separada.** O que ficar 100% transparente na pintura não conta: um tile sem nenhum ponto pintado não é gravado (fica como estava) e, no fundo, os pontos transparentes também ficam como estavam. Então dá para exportar só a camada da sua pintura, sem o original por baixo. (Para esconder de propósito um pedaço de sprite com um tile transparente, use `--manter-vazios`.)

Salve como **PNG**, com transparência no caso dos sprites. O nome pode ser qualquer um, por exemplo `pintado_bg.png` e `pintado_obj.png`.

### 2.3 Fatiar

Na pasta do projeto, onde fica `tools/`:

```
python3 tools/hd_slice.py <pasta do jogo>/mods/scenes/scene_001.json \
    --bg pintado_bg.png --obj pintado_obj.png
```

- Pode passar só `--bg` ou só `--obj`.
- Os tiles saem em `<pasta do jogo>/mods/hd`, ao lado de `mods/scenes`, como `b_<código>.png` (fundo) e `o_<código>.png` (sprites). O `mods/hd/pack.json` guarda a paleta de referência de cada tile.
- Fatiar outra cena **acrescenta** à mesma pasta. Um tile que já existe é atualizado: as partes que apareceram na cena nova são trocadas, e o resto do tile é mantido.
- Partes de um tile que nunca apareceram em nenhuma captura recebem o desenho original ampliado.

**Opções**

| Opção | O que faz |
|---|---|
| `--out PASTA` | Grava os tiles em outra pasta, em vez de `mods/hd`. |
| `--substituir` | Ignora o PNG que já existe e refaz o tile inteiro com esta cena. |
| `--modo primeira` | (padrão) Tile repetido pintado diferente em mais de um lugar: vale a primeira ocorrência, de cima para baixo e da esquerda para a direita. |
| `--modo comum` | Vale a cor mais frequente em cada ponto. |
| `--modo media` | Vale a média das cores. |
| `--manter-vazios` | Sprite 100% transparente na pintura vira tile transparente (esconde o pedaço no jogo). Por padrão, tile sem pintura não é gravado. |
| `--todas-votam` | Ocorrências deixadas iguais ao original também contam. Por padrão, o que foi pintado vence. |
| `--mapa-conflitos` | Grava `conflitos_bg.png` / `conflitos_obj.png`, com a pintura apagada e, em vermelho, os pontos onde ela diferiu do que ficou no tile. |
| `--fundo MODO` | Onde o fundo pintado vale: `fase+paleta` (padrão), `paleta`, `fase` ou `generico` (todo lugar). Veja 2.3a. |
| `--sprites MODO` | O mesmo para os sprites. Padrão: `generico`. |
| `--generico` | Fundo e sprites valem em todo lugar (o comportamento antigo). |
| `--contexto fXX` | Força a fase (`fXX`) ou a tela do mapa (`mXX`), em hexadecimal. Serve para cenas gravadas antes desta versão, que não têm essa informação. |

No fim, o script diz quantos tiles gravou e lista os tiles com conflito, por exemplo `b_6d5d26b984233cbc_f10_p3: 4400 subpixels diferentes`. Um conflito não é erro. Ele avisa que um tile repetido foi pintado de jeitos diferentes.

### 2.3a Por fase e por paleta

O nome do PNG diz onde ele vale:

| Arquivo | Vale para |
|---|---|
| `b_<código>.png` | todo lugar |
| `b_<código>_p3.png` | paleta 3, em qualquer fase |
| `b_<código>_f10.png` | fase `$10`, com qualquer paleta |
| `b_<código>_f10_p3.png` | fase `$10` com a paleta 3 |

`_m44` no lugar de `_f10` é a tela do mapa (página DX `$44` = Rice Beach), e `_g00` é uma das outras telas, pelo modo do jogo (`g00` título e slots, `g08` game over, `g09` final, `g0b` tesouro obtido). O número da paleta é o da paleta do Game Boy Color (0 a 7), não as cores: nos fades as cores mudam, o número não.

A lista completa dos números está em **`tools/fases.csv`**. O número da fase é o interno do jogo, não o do Course: o Course 1 é `f07`. Algumas fases têm duas versões com números diferentes: o Course 1 e o Course 3 depois da inundação de Rice Beach (`f17` e `f24`) e o Course 31 com o lago esvaziado (`f2a`). A fase continua valendo nas portas, na morte, no tempo esgotado e na saída da fase. A transição dos slots para o mapa (`m42`) usa o mesmo contexto dos slots (`g00`), e a transição do contador de moedas no fim da fase para o mapa regional (`m4d`) usa o contexto `f19` (Course 05): a arte pintada numa vale na outra. No `m4d` as laterais do widescreen ficam sem cenário (preto ou desfoque).

No jogo vale o **mais específico** que existir: fase+paleta, depois paleta, depois fase, depois o genérico. Então os mods antigos, só com genéricos, continuam funcionando igual.

- O fatiador já dá o nome certo pela captura: o fundo sai com fase e paleta, e os sprites saem genéricos. Use `--fundo` e `--sprites` para mudar.
- Em **Menu > Mods** aparece o contexto do momento (`Contexto agora: fase (sufixo _f10)`).
- Um tile novo por fase começa do genérico que já existir na pasta (o que você pintou antes), não do original.
- Se a pasta tiver o genérico e o específico, o script avisa: o genérico continua valendo nas outras fases. Apague-o se ele estiver "vazando" para onde não devia.
- Cenas gravadas antes desta versão não têm a fase nem o número da paleta. Fatie com `--contexto f10` (a fase da cena) para separar por fase; o `_pN` fica de fora nelas.

### 2.3b Sprites animados: gravar poses

O Wario e os inimigos mudam de desenho o tempo todo, até parados. Cada quadro de animação usa tiles diferentes, e capturar todos com o F9 dá muito trabalho. Para isso existe o gravador de poses, que monta um spritesheet sozinho:

1. Em **Menu > Desenvolvedor > Gravar poses de sprites**, ajuste o **retângulo vermelho** em volta de onde o personagem vai ficar. Só entram as poses com o centro dentro dele, o que evita misturar inimigos que encostam no Wario. Com **"So dentro do retangulo"** desmarcado, entram todos os sprites da tela.
2. Clique em **Comecar a gravar**, feche o menu e jogue: ande, corra, pule, apanhe, deixe os inimigos se mexerem. No canto da tela aparece `REC poses: N`.
3. Volte ao menu e clique em **Parar e salvar**.

Sai em `mods/scenes`:

| Arquivo | Para que serve |
|---|---|
| `sprites_001_obj_4x.png` | A folha com todas as poses, ampliada 4x. É sobre ela que você pinta. |
| `sprites_001_obj.png` | A mesma folha em 1x (referência do fatiador). Não edite. |
| `sprites_001.json` | De qual tile veio cada pixel, e a paleta de cada tile. Não edite. |
| `sprites_001_grade_4x.png` | Grade de referência das poses (caixas e números dos tiles). |

- Cada pose aparece uma vez só, na ordem em que surgiu no jogo. A mesma pose espelhada (virado para o outro lado) conta como a mesma.
- Os pedaços de sprite que se encostam viram uma pose. Se dois personagens se encostaram dentro do retângulo, eles saem juntos numa pose a mais. Não tem problema: pinte ou ignore.
- O F9 fica desligado enquanto grava.

Pinte por cima do `_obj_4x.png` com as mesmas regras dos sprites (seção 2.2) e fatie só com `--obj`:

```
python3 tools/hd_slice.py <pasta do jogo>/mods/scenes/sprites_001.json --obj pintado_sprites.png
```

**Você não precisa pintar todas as poses.** O jogo espelha os sprites pelo hardware: o Wario olhando para a esquerda e para a direita usa os mesmos tiles. Poses diferentes também repetem pedaços (a mesma cabeça com pernas diferentes). O fatiador junta tudo: **o que foi pintado vence o que ficou igual ao original**. Então basta pintar cada pedaço uma vez, por exemplo só as poses olhando para um lado, e deixar as outras como estão.

Atenção aos pedaços que só existem num dos lados. O Wario olhando para a direita, por exemplo, tem um pedaço a mais. Esses também precisam ser pintados, na pose em que aparecem.

Se pintar o mesmo pedaço de jeitos diferentes em duas poses, vale o `--modo`, como em qualquer tile repetido. A folha já leva a paleta de cada tile, então os sprites pintados a partir dela são recoloridos certo nos fades.

### 2.4 Testar no jogo

1. Abra o jogo (ou deixe aberto) e vá em **Menu > Mods > Recarregar**.
2. A pasta aparece na lista como **`hd/ (pasta)`**, com o número de tiles ao lado.
3. Volte para o jogo. Para comparar com o original, marque e desmarque **Tiles HD**.

Para ajustar, pinte de novo, rode o `hd_slice.py` e clique em **Recarregar**. Não precisa fechar o jogo.

### 2.5 Empacotar em `.o2r`

Quando estiver bom:

```
python3 tools/hd_empacotar.py <pasta do jogo>/mods/hd <pasta do jogo>/mods/meu_mod.o2r
```

- Entram os `b_*.png` / `o_*.png` e o `pack.json`. Os mapas de conflito e o resto ficam de fora.
- Um `o_*.png` 100% transparente fica de fora, com aviso (`--manter-vazios` para incluir).
- Depois de empacotar, **apague ou renomeie a pasta `mods/hd`**, por exemplo para `hd_trabalho`. Se ela ficar, continua aparecendo no fim da lista e sobrepondo o `.o2r`.

**Sem o script:** o `.o2r` é um ZIP comum com outra extensão. Compacte os PNGs e o `pack.json` no 7-Zip, WinRAR, Ark ou `zip` e troque a extensão de `.zip` para `.o2r`. Os arquivos podem ficar em subpastas dentro do ZIP. Não use senha, e não passe de 4 GB (ZIP64 não é suportado).

Pronto: é só distribuir o `meu_mod.o2r`. Quem recebe segue a seção 1.

---

## 3. Referência do formato

```
meu_mod.o2r  (ZIP)
├── pack.json
├── b_0123456789abcdef.png
├── o_fedcba9876543210.png
└── qualquer/subpasta/b_....png   ← subpastas são permitidas
```

- **`b_<16 hex>.png`**: tile de fundo ou de window (inclui o HUD).
- **`o_<16 hex>.png`**: tile de sprite.
- Sufixos opcionais, nesta ordem: `_fXX` (fase `$XX`), `_mXX` (tela do mapa, página DX `$XX`) ou `_gXX` (outra tela, modo do jogo `$XX`), e `_pN` (paleta 0–7). Exemplo: `b_0123456789abcdef_f07_p3.png`. Veja a seção 2.3a e `tools/fases.csv`.
- O código de 16 caracteres é tirado do conteúdo do tile (os 16 bytes dele na VRAM). Por isso, o mesmo desenho tem sempre o mesmo nome, em qualquer fase. Cada quadro de uma animação é um tile diferente.
- **Tamanho**: PNG **quadrado**, com lado múltiplo de 8 (16, 24, 32, 64…, até 512). Tiles diferentes podem ter escalas diferentes.
- **`pack.json`**:
  ```json
  {"version": 1, "tiles": {
      "b_0123456789abcdef": {"palette": [4294967295, 4289374890, 4283782485, 4278190080], "scale": 4}
  }}
  ```
  `palette` são as 4 cores originais do tile na captura, em ARGB. A chave é o nome do arquivo sem `.png`, com os sufixos. Um tile sem entrada no `pack.json` também funciona, mas não é recolorido nos fades.
- Se vários `.o2r` ou `pack.json` tiverem o mesmo tile, vale o último carregado, isto é, o mais embaixo na lista.

---

## 4. Problemas comuns

| Sintoma | O que verificar |
|---|---|
| Nada muda na tela | Internal Resolution em 2x ou mais; **Tiles HD** marcado; caixa do mod marcada; clicar em **Recarregar** depois de copiar arquivos. |
| Mod com "erro: …" na lista | O arquivo não é um ZIP válido, tem senha ou é ZIP64. Recrie com o `hd_empacotar.py`. |
| "alguns ignorados (veja o terminal)" | Algum PNG não é quadrado ou o lado não é múltiplo de 8. O terminal mostra o nome. |
| Cores erradas só nos fades | O tile não tem paleta no `pack.json`. Inclua o `pack.json` no `.o2r`. |
| Pintura de um lugar apareceu em outro | Na mesma tela: é um tile repetido; use `--mapa-conflitos` para ver onde, e `--modo` para escolher. Em outra fase: o tile está genérico (`b_<código>.png`, sem `_fXX`); apague o genérico e fatie de novo (seção 2.3a). |
| `aviso: ... recriado em NxN` no fatiador | O tile já existia em outra escala. Ele foi refeito na escala nova, e a pintura antiga dele se perdeu. |
| As laterais do widescreen não mudam | Os tiles das laterais precisam estar no pacote: capture a cena com o widescreen ligado. Nas telas em que o jogo deixa as laterais pretas (menu, mapa de bônus, sala do chefe…), elas continuam pretas. |
| Pintura do fundo passa atrás do Wario quando não devia | A prioridade vem do jogo original e não pode ser mudada pelo PNG. |

---

## 5. Extras (menu Desenvolvedor)

- **Despejar tiles (PNG)**: grava cada tile novo que aparece na tela em `mods/dump`, em 8×8 e no tamanho original, e a paleta de cada um em `mods/dump/pack.json`. Serve para explorar, mas para pintar o fluxo por cena (F9) é bem mais prático.
- **Conferir amostragem HD** / **Usar frame HD na tela**: ferramentas de teste do renderizador. Deixe como estão.

---

## 6. Trilhas de música (.ogg)

Um mod também pode trocar as músicas do jogo por gravações em `.ogg`. A trilha toca no lugar da música do Game Boy, e os efeitos sonoros (moeda, pulo, passos…) continuam saindo do GB. Música sem trilha continua tocando no GB.

### 6.1 Nomes dos arquivos

Cada trilha fica numa pasta `music` (a pasta `mods/music/` ou uma pasta `music/` dentro de um `.o2r`), com o **número da música em hexadecimal** no começo do nome. Depois do número pode vir `_`, `-` ou espaço e o que você quiser:

```
music/04.ogg
music/04_tema_principal.ogg
music/2B - entrada da fase.ogg
```

A tabela abaixo tem todos os números. Para conferir qual música está tocando agora, veja:
- o overlay, que mostra `Musica: XX` (com `[ogg]` quando a trilha está tocando);
- **Menu > Desenvolvedor > Musicas vistas**, com as últimas 16 trocas;
- o terminal, se você marcar **Registrar no terminal** nessa mesma seção. Sai uma linha `[Musica] frame N: musica XX (modo de jogo YY)` a cada troca, o que facilita anotar uma sessão inteira de jogo.

**Lista completa das músicas.** Os números e nomes vêm das constantes `BGM_*` do disassembly do jogo original ([Kak2X/wl](https://github.com/Kak2X/wl), `src/constants.asm`); a DX usa os mesmos números. ✔ = conferido jogando no Bigodão. **Jingle** = trecho curto, que provavelmente deve tocar uma vez só (`"loop": false` no `music.json`, seção 6.2).

A mesma lista existe em formato de dados em `tools/musicas.json`, para scripts e ferramentas.

| Número | Onde toca | Nome no disassembly | Jingle |
|---|---|---|---|
| 01 | Tela de título | `BGM_TITLE` |  |
| 02 | Mapa geral (escolha de região) ✔ | `BGM_OVERWORLD` |  |
| 03 | Fases debaixo d'água | `BGM_WATER` |  |
| 04 | Tema principal (fase 1 de Rice Beach e outras) ✔ | `BGM_COURSE1` |  |
| 05 | Cofre de moedas | `BGM_COINVAULT` |  |
| 06 | Invencibilidade | `BGM_INVINCIBILE` |  |
| 07 | Chuva de moedas no fim da luta com o chefe | `BGM_COINGAME` |  |
| 08 | Wario morre ✔ | `BGM_LIFELOST` | sim |
| 09 | Fases de navio | `BGM_SHIP` |  |
| 0A | Fases de lava | `BGM_LAVA` |  |
| 0B | Fases do trem | `BGM_TRAIN` |  |
| 0C | Jogo bônus de moedas | `BGM_COINBONUS` |  |
| 0D | Fase concluída | `BGM_LEVELCLEAR` | sim |
| 0E | Fase de chefe (antes da luta) | `BGM_BOSSLEVEL` |  |
| 0F | Sala do tesouro | `BGM_TREASURE` |  |
| 10 | Fases de lava (segunda versão) | `BGM_LAVA2` |  |
| 11 | Luta contra o chefe | `BGM_BOSS` |  |
| 12 | Fase de chefe concluída | `BGM_BOSSCLEAR` | sim |
| 13 | Mapa de Mt. Teapot | `BGM_MTTEAPOT` |  |
| 14 | Mapa de Sherbet Land | `BGM_SHERBETLAND` |  |
| 15 | Mapa de Rice Beach ✔ | `BGM_RICEBEACH` |  |
| 16 | Cavernas | `BGM_CAVE` |  |
| 17 | Tema de fase (fase 3) | `BGM_COURSE3` |  |
| 18 | Jogo bônus de corações | `BGM_HEARTBONUS` |  |
| 19 | Chegada ao chefe final | `BGM_FINALBOSSINTRO` |  |
| 1A | Mundo concluído | `BGM_WORLDCLEAR` | sim |
| 1B | Ambiente | `BGM_AMBIENT` |  |
| 1C | Tesouro obtido (também no final) | `BGM_TREASUREGET` | sim |
| 1D | Mapa do Syrup Castle | `BGM_SYRUPCASTLE` |  |
| 1E | Fase final | `BGM_FINALLEVEL` |  |
| 1F | Mapa de Parsley Woods | `BGM_PARSLEYWOODS` |  |
| 20 | Abertura ✔ | `BGM_INTRO` |  |
| 21 | Mapa de Stove Canyon | `BGM_STOVECANYON` |  |
| 22 | Tema de fase (variação da fase 3) | `BGM_COURSE32` |  |
| 23 | Mapa do SS Teacup | `BGM_SSTEACUP` |  |
| 24 | Game over | `BGM_GAMEOVER` | sim |
| 25 | Tempo esgotado | `BGM_TIMEOVER` | sim |
| 26 | Escolha do slot ✔ | `BGM_SAVESELECT` |  |
| 27 | Fases de gelo | `BGM_ICE` |  |
| 28 | Luta contra o chefe final | `BGM_FINALBOSS` |  |
| 29 | Créditos | `BGM_CREDITS` |  |
| 2A | Escolha do bônus na tela de fase concluída | `BGM_SELECTBONUS` |  |
| 2B | Entrada na fase ✔ | `BGM_LEVELENTER` | sim |
| 2C | Cena de história | `BGM_CUTSCENE` |  |
| 2D | Final (gênio) | `BGM_ENDINGGENIE` |  |
| 2E | Game over (segunda versão) | `BGM_GAMEOVER2` | sim |
| 2F | Final (estátua) | `BGM_ENDINGSTATUE` |  |
| 30 | Cena antes da bomba, depois do chefe final | `BGM_FINALBOSSOUTRO` |  |

`00` e `FF` não são músicas (silêncio e parada).

As descrições sem ✔ vêm dos nomes do disassembly e podem ser imprecisas em algum detalhe (por exemplo, qual fase usa qual tema). Se jogar e confirmar uma, marque na tabela e no `musicas.json`.

### 6.2 Loop

Sem configuração, a trilha **repete do começo ao fim**. Para escolher o ponto de loop, use uma destas opções:

- **Tags no próprio .ogg:** `LOOPSTART` e `LOOPLENGTH` (ou `LOOPEND`), em amostras. É o padrão de muitos pacotes de trilhas remasterizadas.
- **`music/music.json`:**
  ```json
  {
    "04": { "loop_start": 123456, "loop_end": 0 },
    "2B": { "loop": false },
    "08": { "loop": false, "volume": 0.8 }
  }
  ```
  - `loop_start` / `loop_end`: em amostras do `.ogg` (a 44100 Hz, 1 segundo = 44100). `loop_end` 0 = fim do arquivo.
  - `"loop": false`: toca uma vez só. Use nos **jingles** (a coluna "Jingle" da tabela) para não repetirem.
  - `volume`: ajuste desta trilha (1.0 = normal).
  - O `music.json` vale sobre as tags do `.ogg`.

### 6.3 Empacotar

Para testar sem empacotar, ponha os `.ogg` (e o `music.json`, se tiver) direto em **`mods/music/`** e clique em **Recarregar**. Na lista de Mods ela aparece como `music/ (pasta)`, por último, então vale sobre os `.o2r`.

Para distribuir, coloque os arquivos numa pasta e rode:

```
python3 tools/musica_empacotar.py ~/minhas_trilhas <pasta do jogo>/mods/trilhas.o2r
```

O script põe tudo em `music/` dentro do `.o2r`, sem compressão (o `.ogg` já é comprimido). Sem o script: um ZIP comum com a pasta `music/` e a extensão trocada para `.o2r`.

Um mesmo `.o2r` pode ter tiles HD e trilhas. Na lista, um mod só de música aparece com "0 tiles", o que é normal.

### 6.4 No jogo

Em **Menu > Mods**:
- **Trilhas .ogg** liga e desliga as trilhas (desligado, volta a música do GB);
- **Volume** ajusta só as trilhas (o volume geral do jogo também vale);
- a linha de baixo lista as músicas que têm trilha.

As duas escolhas ficam salvas para a próxima vez.

Se a mesma música tiver trilha em mais de um mod, vale a do mod mais embaixo na lista, como nos tiles.

**Formato:** OGG Vorbis (não Opus), mono ou estéreo, em qualquer taxa (44100 ou 48000 Hz são as mais comuns). Até 256 MB por arquivo.