"""
### Decodificador de Tiles de Sprite do Game Boy

Este notebook irá ler um arquivo binário contendo dados de tiles de sprite do Game Boy (`sprite_tiles.bin`), decodificá-los e gerar uma imagem PNG.

**Formato dos Tiles de Sprite do Game Boy:**

Cada tile de sprite no Game Boy é uma imagem 8x8 pixels. Os dados de cada tile são armazenados em 16 bytes. Cada 2 bytes representam uma linha de 8 pixels:

*   O primeiro byte (LSB - Least Significant Bit) contém o bit 0 de cada pixel na linha.
*   O segundo byte (MSB - Most Significant Bit) contém o bit 1 de cada pixel na linha.

Para um pixel `n` (de 0 a 7 na linha):

*   O valor da cor é formado por `(bit_1_do_pixel_n << 1) | bit_0_do_pixel_n`.
*   Isso resulta em um valor de 0 a 3, representando 4 cores.

Para a saída, usaremos uma paleta de 4 tons de cinza para representar as 4 cores possíveis.



"""

import os
from PIL import Image

# Define a palette para as 4 cores do Game Boy (tons de cinza)
# [R, G, B, A]
PALETTE = [
    (255, 255, 255, 255), # Cor 0 (Branco)
    (170, 170, 170, 255), # Cor 1 (Cinza Claro)
    (85, 85, 85, 255),    # Cor 2 (Cinza Escuro)
    (0, 0, 0, 255)        # Cor 3 (Preto)
]

def decode_gameboy_tile(tile_data):
    """Decodifica 16 bytes de dados de tile do Game Boy em uma matriz de pixels 8x8."""
    pixels = [[0 for _ in range(8)] for _ in range(8)]

    for y in range(8):
        # Cada linha é definida por 2 bytes (LSB e MSB)
        lsb_byte = tile_data[y * 2]
        msb_byte = tile_data[y * 2 + 1]

        for x in range(8):
            # Obtém o bit para o pixel atual (da esquerda para a direita, bit 7 ao bit 0)
            # A ordem dos bits é do mais significativo para o menos significativo para o pixel X
            # mas o X na imagem é da esquerda para a direita, então invertemos o índice do bit
            bit_index = 7 - x

            # Extrai o bit 0 e o bit 1 para o pixel atual
            bit0 = (lsb_byte >> bit_index) & 1
            bit1 = (msb_byte >> bit_index) & 1

            # Combina os bits para obter o valor da cor (0-3)
            color_value = (bit1 << 1) | bit0
            pixels[y][x] = color_value
    return pixels

# Caminho para o arquivo binário
input_bin_file = '/content/sprite_tiles.bin'
output_dir = 'individual_sprites'

# Verifica se o arquivo existe
if not os.path.exists(input_bin_file):
    print(f"Erro: Arquivo '{input_bin_file}' não encontrado.")
else:
    print(f"Processando arquivo: {input_bin_file}")

    # Cria o diretório de saída se não existir
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        print(f"Diretório '{output_dir}' criado.")

    all_tiles_pixels = []
    tile_size_bytes = 16 # 8x8 pixels * 2 bytes por linha

    with open(input_bin_file, 'rb') as f:
        while True:
            tile_data = f.read(tile_size_bytes)
            if not tile_data: # Fim do arquivo
                break
            if len(tile_data) == tile_size_bytes:
                decoded_tile = decode_gameboy_tile(tile_data)
                all_tiles_pixels.append(decoded_tile)
            else:
                print(f"Aviso: Dados incompletos no final do arquivo. Ignorando {len(tile_data)} bytes.")
                break

    if all_tiles_pixels:
        print(f"Decodificados {len(all_tiles_pixels)} tiles.")
        
        saved_tiles_count = 0
        for i, tile_pixels in enumerate(all_tiles_pixels):
            # Cria uma imagem para o tile atual
            tile_img = Image.new('RGBA', (8, 8)) # Tiles são 8x8
            tile_data_for_putdata = []
            for row in tile_pixels:
                for pixel_color_index in row:
                    tile_data_for_putdata.append(PALETTE[pixel_color_index])
            tile_img.putdata(tile_data_for_putdata)
            
            # Salva o tile individualmente
            tile_filename = os.path.join(output_dir, f'tile_{i:04d}.png')
            tile_img.save(tile_filename)
            saved_tiles_count += 1
            
        print(f"Total de {saved_tiles_count} tiles salvos individualmente em '{output_dir}/'")
    else:
        print("Nenhum tile válido foi encontrado ou decodificado.")
