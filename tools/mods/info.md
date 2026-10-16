Scripts:
tools/hd_slice.py - gera o mod de textura
tools/hd_empacotar.py - gera o o2r

O fluxo para pintar:

Aperte F9 numa tela que você queira redesenhar.
Pinte por cima de scene_001_bg_4x.png e, se quiser, de scene_001_obj_4x.png, no editor que preferir. Também pode trabalhar em 2x, 3x ou outra escala inteira: o importante é a imagem ter 160N × 144N pixels.
Rode o fatiador:

   python3 tools/hd_slice.py mods/scenes/scene_001.json --bg meu_bg.png --obj meu_obj.png

Os tiles HD saem em mods/hd, junto com um pack.json que guarda a paleta de referência de cada tile para a recoloração da 5b.

Regras do fatiador:

Tiles repetidos: o mesmo tile costuma aparecer várias vezes na tela. Vale a primeira aparição, e o script avisa se outra foi pintada diferente.
Partes fora da tela: pedaços de tile que não aparecem na captura mantêm o PNG HD que já existir, então dá para juntar várias cenas. Se ainda não existir PNG, entram com o desenho original ampliado.
Sprites: a transparência da sua pintura define a forma, e dá para pintar dentro da caixa 8×8 de cada parte, mesmo onde o original era transparente.
Fundo: a pintura é sempre opaca.

Conferido aqui:

Em uma cena com scroll, flips, dois bancos de tiles e 40 sprites, cada pixel capturado foi reconstruído exatamente a partir da tabela de tiles.
Fatiando a cena sem pintar nada, os 80 tiles saíram idênticos ao original ampliado.
Pintando metade da tela de vermelho, os tiles atingidos foram atualizados e o script apontou os conflitos.
A mesma cena em 2x também fatiou certo.
Os testes anteriores continuam passando


__________________________________________________________________________________________
2 parte

O que aconteceu: o Game Boy monta o cenário repetindo poucos tiles. O mesmo tile de chão, céu ou parede aparece várias vezes na mesma tela, mas só existe um PNG por tile. Se cada cópia foi pintada um pouco diferente, só uma versão pode valer. Os números confirmam isso: um tile de 32×32 tem 1.024 subpixels, então 4.400 subpixels diferentes significa umas 4 ou mais cópias inteiras pintadas de jeitos distintos. Isso acontece muito quando a imagem passa por um ampliador automático ou por IA, porque ele pinta cada cópia de um jeito.

O que eu atualizei no fatiador (hd_slice_v2.zip, que substitui o tools/hd_slice.py):

--modo escolhe o que fazer com as cópias diferentes:
primeira: a primeira cópia vale, como antes;
comum: em cada subpixel, vale a cor que mais se repete entre as cópias. Costuma dar o melhor resultado com imagens ampliadas;
media: a média das cores, que suaviza mas pode embaçar um pouco.
--mapa-conflitos grava conflitos_bg.png e conflitos_obj.png: a sua pintura em cinza, com os pontos de conflito em vermelho. Assim você vê exatamente onde as cópias divergem e pode corrigir à mão.
python3 tools/hd_slice.py mods/scenes/scene_001.json --bg meu_bg.png --obj meu_obj.png --modo comum --mapa-conflitos

Testei com a mesma cena de antes: fatiada sem pintura, ela continua idêntica ao original. Os três modos e o mapa funcionaram com a pintura de teste.

Para pintar bem por tile: pense no cenário como peças que se repetem. A parede pintada num lugar vai aparecer igual em todo lugar onde aquela peça é usada. O mapa de conflitos ajuda a manter isso.

Se você quiser arte única por posição (um cenário pintado como uma ilustração, sem repetição), dá para fazer uma troca por posição no mundo só para o fundo. O código de widescreen já sabe a posição da câmera e o layout da fase, então uma imagem HD da fase inteira poderia ser desenhada pela posição de cada pixel, e não pelo tile. Os sprites continuariam por tile. É um passo maior, que fica para depois da 5b.

Uma pergunta: você pintou à mão ou usou algum ampliador? Se usou um ampliador, o --modo comum provavelmente resolve. Se pintou à mão, o mapa de conflitos é o melhor caminho.

Quando quiser, sigo para a 5b, em que o jogo carrega esses PNGs e desenha com eles. Aí dá para ver o resultado da sua pintura rodando no jogo.


______________________________________________________________________________
3 parte


Como os tiles HD entram no jogo:

Fundo: o PNG troca só a cor. A prioridade em relação aos sprites continua vindo do tile original, então nada passa na frente ou atrás errado. Onde o PNG tiver alfa 0, aparece a cor original.
Sprites: a transparência do PNG define a forma. O desenho pode ocupar a caixa 8×8 inteira de cada parte, inclusive onde o original era transparente.
Fades e piscadas: quando a paleta muda, o tile HD é recolorido na mesma proporção, a partir da paleta de referência que o fatiador grava no pack.json. Tiles sem paleta de referência não são recoloridos.
Menu Graphics: nova opção “Tiles HD (mods/hd)”, que fica salva, e um botão “Recarregar”. Ele relê a pasta sem fechar o jogo, então dá para pintar, fatiar e ver o resultado na hora.
Escala: com tiles HD carregados, a textura usa a Internal Resolution pedida, mesmo com Nearest e sem pós-processamento. Para ver os tiles HD, escolha 2x ou mais. Em 1x, o menu avisa.

O fatiador também mudou e vem no zip. Quando o mesmo tile aparece com paletas diferentes na cena, ele agora converte as cores para a paleta de referência, e o jogo faz a conta inversa ao desenhar. Sem isso, o pacote sem pintura saía com cores erradas nesses tiles. Por isso, fatie de novo as cenas que você já fatiou.

Conferido aqui:

Fluxo completo: captura, fatiamento sem pintura, carregamento e desenho em 4x. A imagem saiu idêntica ao original ampliado, inclusive com tiles em várias paletas.
Em 40 cenas aleatórias, com tiles HD iguais ao original e depois um fade, a imagem também foi idêntica, o que confirma a recoloração.
Com uma pintura vermelha, o vermelho apareceu no jogo. Com os tiles HD desligados, a imagem volta exatamente à original.
Os testes anteriores continuam passando.


_______________________________________________________________________________________________________

jogo agora lê mods .o2r da pasta mods, e o menu ganhou uma seção “Mods” com uma caixa para ligar e desligar cada um.

Como os mods são lidos:

Entram todos os .o2r da pasta mods, em ordem alfabética, e por último a pasta mods/hd, se ela existir.
Quando o mesmo tile aparece em mais de um mod, vale o que está mais embaixo na lista. Para mudar a ordem, renomeie os arquivos, por exemplo 01_base.o2r e 02_detalhes.o2r.
Dentro do .o2r valem os b_<tile>.png e o_<tile>.png em qualquer subpasta, e os pack.json.
Um .o2r criado no 7-Zip, no WinRAR, no Ark ou no zip funciona, comprimido ou não. Só não é aceito ZIP com senha ou ZIP64 (arquivos acima de 4 GB).
Um arquivo que não seja ZIP aparece na lista com o erro ao lado, sem travar os outros.

Empacotador: o zip inclui o tools/hd_empacotar.py, que junta uma pasta de tiles num .o2r e deixa de fora os mapas de conflito e os outros arquivos:

python3 tools/hd_empacotar.py mods/hd mods/meu_mod.o2r