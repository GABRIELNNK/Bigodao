import sys, os
out = sys.argv[1]; items = sys.argv[2:]  # name=path
with open(out, "w") as f:
    f.write("// Gerado automaticamente — não editar.\n")
    f.write("// Recursos embutidos do launcher (arte JPG + fontes Nunito, SIL OFL 1.1).\n#pragma once\n\n")
    for it in items:
        name, path = it.split("=", 1)
        data = open(path, "rb").read()
        f.write(f"// {os.path.basename(path)} ({len(data)} bytes)\n")
        f.write(f"static const unsigned char {name}[] = {{\n")
        for i in range(0, len(data), 32):
            f.write(",".join(str(b) for b in data[i:i+32]) + ",\n")
        f.write("};\n")
        f.write(f"static const unsigned int {name}_LEN = {len(data)}u;\n\n")
