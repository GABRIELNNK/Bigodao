# Patch DX (v1.2): como a paleta de BG CGB é escolhida

Fonte: engenharia reversa do `Wario Land - Super Mario Land 3 DX (World).ips` (1281 registros; ROM 512 KB -> 1 MB, MBC1 -> MBC5, flag CGB 0xC0). Não testado em execução, só lido do IPS.

## Mecanismo
O patch intercepta o código do jogo que escreve o mapa de tiles de BG na VRAM. Depois que o jogo grava o tile no banco 0, o hack (banco ROM 0x22, código em 0x4007 / 0x40B0 / 0x40F0) lê o número do tile de volta e escreve o atributo no banco 1 (via `LDH [FF4F],A` com 0xFF/0xFE).

Rotina de consulta (0x40A0, banco 0x22):
```
D = [$A800] + 0x41      ; $A800 = id da fase  (ctx->eram[0x800])
E = tile
A = [DE]                ; tabela no próprio banco 0x22, endereço (0x41+fase)*0x100 + tile
se A < 8:  atributo = A (paleta 0-7, sem flip, sem prioridade, banco de tile 0)
senão:     JP (A<<8)    ; rotina especial, página 0x78..0x7F do banco 0x22
```

## Onde está a tabela no arquivo ROM (1 MB)
`rom_offset = 0x84000 + ((0x41 + fase) << 8) + tile`   (banco 0x22 = 0x88000, endereço 0x4000 = início do banco)
Fases 0x00..0x29 têm dados; 0x2A..0x2D são tudo zero.

## Entradas >= 8 (rotinas especiais)
Poucos tiles por fase (0 a 13 de 256). A rotina decide a paleta pela posição no mapa de tiles (H = byte alto de HL, L = byte baixo) e às vezes por RAM (`$A804`, `$A9AB`, `$A9E4`, `$A3FA`) ou por tiles vizinhos na VRAM. Páginas: 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F. Não portadas ainda.

## Outros pontos que escrevem atributo
- 0x23:5000: preenche a linha 0x9C00.. com paleta 0
- 0x25:7F00: limpa 0x99E0.. (uma vez, flag `$B103`)
- 0x24:4000: rotina de cópia de linha com `$D0FD/$D0FE`
