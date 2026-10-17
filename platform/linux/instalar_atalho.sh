#!/usr/bin/env bash
# Cria o atalho "Bigodão" no menu de aplicativos, com o ícone.
# Uso (na pasta do projeto, depois de compilar):
#   bash platform/linux/instalar_atalho.sh            # usa build/wario_land_port
#   bash platform/linux/instalar_atalho.sh caminho/do/executavel
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
EXE="$(realpath "${1:-$ROOT/build/wario_land_port}")"

if [ ! -x "$EXE" ]; then
    echo "Executável não encontrado: $EXE"
    echo "Compile primeiro ou passe o caminho: bash $0 caminho/do/executavel"
    exit 1
fi

ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"
APP_DIR="$HOME/.local/share/applications"
mkdir -p "$ICON_DIR" "$APP_DIR"
cp "$HERE/bigodao.png" "$ICON_DIR/bigodao.png"

cat > "$APP_DIR/bigodao.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Bigodão
Comment=Wario Land: Super Mario Land 3 DX (recompilado)
Exec="$EXE"
Path=$(dirname "$EXE")
Icon=bigodao
Terminal=false
Categories=Game;
StartupWMClass=wario_land_port
EOF

update-desktop-database "$APP_DIR" 2>/dev/null || true
gtk-update-icon-cache "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
echo "Atalho criado: procure \"Bigodão\" no menu de aplicativos."
