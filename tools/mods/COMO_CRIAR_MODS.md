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

O número aumenta a cada captura: 001, 002…

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
- O jogo reaproveita o mesmo tile em vários lugares, como blocos, chão e nuvens. **Existe um único PNG por tile.** Se você pintar duas ocorrências do mesmo tile de jeitos diferentes, só uma pode valer. Na seção 2.3 está como escolher qual.
- O jogo também reaproveita tiles **espelhados**. O fatiador desfaz o espelhamento sozinho. Basta pintar o que você vê.
- Para variar a mesma coisa em lugares diferentes (dois tijolos iguais com rachaduras diferentes), por enquanto não há jeito: o tile é um só.

**Cores e fades**
- Pinte com as cores que estão na tela no momento da captura.
- Quando o jogo muda a paleta (escurecer, piscar, transição de fase), o tile HD é recolorido na mesma proporção. Isso usa a paleta de referência que o fatiador grava no `pack.json`.
- Evite capturar no meio de um fade: a cena escura vira a referência.

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
| `--modo primeira` | (padrão) Tile repetido pintado diferente: vale a primeira ocorrência, de cima para baixo e da esquerda para a direita. |
| `--modo comum` | Vale a cor mais frequente em cada ponto. |
| `--modo media` | Vale a média das cores. |
| `--mapa-conflitos` | Grava `conflitos_bg.png` / `conflitos_obj.png`, com a pintura apagada e, em vermelho, os pontos onde ela diferiu do que ficou no tile. |

No fim, o script diz quantos tiles gravou e lista os tiles com conflito, por exemplo `b_6d5d26b984233cbc: 4400 subpixels diferentes`. Um conflito não é erro. Ele avisa que um tile repetido foi pintado de jeitos diferentes.

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
- O código de 16 caracteres é tirado do conteúdo do tile (os 16 bytes dele na VRAM). Por isso, o mesmo desenho tem sempre o mesmo nome, em qualquer fase. Cada quadro de uma animação é um tile diferente.
- **Tamanho**: PNG **quadrado**, com lado múltiplo de 8 (16, 24, 32, 64…, até 512). Tiles diferentes podem ter escalas diferentes.
- **`pack.json`**:
  ```json
  {"version": 1, "tiles": {
      "b_0123456789abcdef": {"palette": [4294967295, 4289374890, 4283782485, 4278190080], "scale": 4}
  }}
  ```
  `palette` são as 4 cores originais do tile na captura, em ARGB. Um tile sem entrada no `pack.json` também funciona, mas não é recolorido nos fades.
- Se vários `.o2r` ou `pack.json` tiverem o mesmo tile, vale o último carregado, isto é, o mais embaixo na lista.

---

## 4. Problemas comuns

| Sintoma | O que verificar |
|---|---|
| Nada muda na tela | Internal Resolution em 2x ou mais; **Tiles HD** marcado; caixa do mod marcada; clicar em **Recarregar** depois de copiar arquivos. |
| Mod com "erro: …" na lista | O arquivo não é um ZIP válido, tem senha ou é ZIP64. Recrie com o `hd_empacotar.py`. |
| "alguns ignorados (veja o terminal)" | Algum PNG não é quadrado ou o lado não é múltiplo de 8. O terminal mostra o nome. |
| Cores erradas só nos fades | O tile não tem paleta no `pack.json`. Inclua o `pack.json` no `.o2r`. |
| Pintura de um lugar apareceu em outro | É um tile repetido. Use `--mapa-conflitos` para ver onde, e `--modo` para escolher. |
| `aviso: ... recriado em NxN` no fatiador | O tile já existia em outra escala. Ele foi refeito na escala nova, e a pintura antiga dele se perdeu. |
| As laterais do widescreen não mudam | Os tiles das laterais precisam estar no pacote: capture a cena com o widescreen ligado. Nas telas em que o jogo deixa as laterais pretas (menu, mapa de bônus…), elas continuam pretas. |
| Pintura do fundo passa atrás do Wario quando não devia | A prioridade vem do jogo original e não pode ser mudada pelo PNG. |

---

## 5. Extras (menu Desenvolvedor)

- **Despejar tiles (PNG)**: grava cada tile novo que aparece na tela em `mods/dump`, em 8×8 e no tamanho original. Serve para explorar, mas para pintar o fluxo por cena (F9) é bem mais prático.
- **Conferir amostragem HD** / **Usar frame HD na tela**: ferramentas de teste do renderizador. Deixe como estão.

---

## 6. Trilhas de música (.ogg)

Um mod também pode trocar as músicas do jogo por gravações em `.ogg`. A trilha toca no lugar da música do Game Boy, e os efeitos sonoros (moeda, pulo, passos…) continuam saindo do GB. Música sem trilha continua tocando no GB.

### 6.1 Nomes dos arquivos

Cada trilha fica numa pasta `music` (a pasta `mods/music/` ou uma pasta `music/` dentro de um `.o2r`), com o **número da música em hexadecimal** no começo do nome. Depois do número pode vir `_`, `-` ou espaço e o que você quiser:

```
music/04.ogg
music/04_rice_beach.ogg
music/2B - entrada da fase.ogg
```

Para descobrir o número de uma música, jogue até ela tocar e veja:
- o overlay, que mostra `Musica: XX` (com `[ogg]` quando a trilha está tocando);
- **Menu > Desenvolvedor > Musicas vistas**, com as últimas 16 trocas;
- o terminal, se você marcar **Registrar no terminal** nessa mesma seção. Sai uma linha `[Musica] frame N: musica XX (modo de jogo YY)` a cada troca, o que facilita anotar uma sessão inteira de jogo.

**Números já confirmados no jogo:**

| Número | Onde toca |
|---|---|
| 20 | Abertura |
| 26 | Escolha do slot |
| 02 | Mapa geral (escolha de região) |
| 15 | Mapa de fases de Rice Beach |
| 2B | Jingle de entrada na fase (curto) |
| 04 | Fase 1 de Rice Beach |
| 08 | Wario morre (jingle) |

`00` e `FF` não são músicas (silêncio e parada).

**Ainda a descobrir.** Não há uma lista pública com os números das músicas do Wario Land. Pela RAM do jogo, os pedidos vão de 01 a 30 (hexa). A MarioWiki lista estas faixas da trilha sonora; vá preenchendo a coluna do número conforme for jogando:

| Faixa | Onde toca | Número |
|---|---|---|
| Ripple Course | primeira fase (Rice Beach 1) | 04 |
| Results | resultado da fase | ? |
| Boss | luta contra chefe | ? |
| Boss Course | fases de chefe | ? |
| Underwater theme | fases debaixo d'água | ? |
| Underground theme | fases subterrâneas | ? |
| WL Ice Theme | fases de Sherbet Land | ? |
| Train Course | fases do trem | ? |
| Syrup Castle | mapa do Syrup Castle | ? |
| Final Boss | chefe final | ? |
| Overworld Theme 2 / 3 | outros mapas | ? |

Faltam também invencibilidade, game over, bônus e final, que não estão nessa lista.

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
  - `"loop": false`: toca uma vez só. Use nos **jingles** (2B, 08…) para não repetirem.
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