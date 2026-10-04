import os
import hashlib
import subprocess
import webview

class LauncherAPI:
    """API exposta para comunicação entre o JavaScript e o Python."""
    
    EXPECTED_ROM_SHA256 = "ac1682f17abcf590311a233289ee325214c2d71ab3a5aa175004002d85075e56".lower()
    EXPECTED_PATCH_SHA256 = "599d9f59090df3e1e77c1678e069290a2cd9ce59c71cf58d66c4dc3db8b7facd".lower()

    def __init__(self):
        self._window = None

    def set_window(self, window):
        """Armazena a referência da janela para abrir diálogos nativos."""
        self._window = window

    def _calculate_sha256(self, file_path: str) -> str:
        """Calcula o hash SHA256 de um arquivo de maneira otimizada por blocos."""
        if not file_path or not os.path.isfile(file_path):
            return ""
        
        sha256_hash = hashlib.sha256()
        try:
            with open(file_path, "rb") as f:
                # Lê em blocos de 64KB
                for byte_block in iter(lambda: f.read(65536), b""):
                    sha256_hash.update(byte_block)
            return sha256_hash.hexdigest().lower()
        except Exception as e:
            print(f"[Erro] Não foi possível ler o arquivo '{file_path}': {e}")
            return ""

    def select_rom(self):
        """Abre o seletor de arquivos para a ROM e valida seu SHA256."""
        if not self._window:
            return {"path": "", "valid": False, "hash": "", "error": "Janela não inicializada."}

        result = self._window.create_file_dialog(
            webview.OPEN_DIALOG,
            file_types=('ROM Files (*.gb;*.gbc)', 'All files (*.*)')
        )

        if result and len(result) > 0:
            file_path = result[0]
            calc_hash = self._calculate_sha256(file_path)
            is_valid = (calc_hash == self.EXPECTED_ROM_SHA256)
            
            return {
                "path": file_path,
                "filename": os.path.basename(file_path),
                "valid": is_valid,
                "hash": calc_hash,
                "expected": self.EXPECTED_ROM_SHA256
            }
        
        return {"path": "", "valid": False, "hash": "", "cancelled": True}

    def select_patch(self):
        """Abre o seletor de arquivos para o Patch IPS e valida seu SHA256."""
        if not self._window:
            return {"path": "", "valid": False, "hash": "", "error": "Janela não inicializada."}

        result = self._window.create_file_dialog(
            webview.OPEN_DIALOG,
            file_types=('Patch Files (*.ips)', 'All files (*.*)')
        )

        if result and len(result) > 0:
            file_path = result[0]
            calc_hash = self._calculate_sha256(file_path)
            is_valid = (calc_hash == self.EXPECTED_PATCH_SHA256)

            return {
                "path": file_path,
                "filename": os.path.basename(file_path),
                "valid": is_valid,
                "hash": calc_hash,
                "expected": self.EXPECTED_PATCH_SHA256
            }
        
        return {"path": "", "valid": False, "hash": "", "cancelled": True}

    def launch_game(self, rom_path, patch_path):
        """Executa o binário do jogo passando a ROM e o Patch validados."""
        if not rom_path or not patch_path:
            return {"success": False, "message": "ROM e Patch precisam estar selecionados e válidos."}

        # Validação dupla de segurança antes da execução
        rom_hash = self._calculate_sha256(rom_path)
        patch_hash = self._calculate_sha256(patch_path)

        if rom_hash != self.EXPECTED_ROM_SHA256:
            return {"success": False, "message": "SHA256 da ROM é inválido!"}
            
        if patch_hash != self.EXPECTED_PATCH_SHA256:
            return {"success": False, "message": "SHA256 do Patch IPS é inválido!"}

        print(f"[Executando] wario_land_port \"{rom_path}\" \"{patch_path}\"")
        try:
            # Executa o processo capturando a saída para verificar se há falhas imediatas
            process = subprocess.Popen(
                ["./wario_land_port", rom_path, patch_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            # Aguarda até 1.5 segundos para checar se o processo crashou/encerrou no início
            try:
                stdout, stderr = process.communicate(timeout=1.5)
                if process.returncode != 0:
                    error_msg = stderr.strip() if stderr else "Erro desconhecido ao executar o jogo."
                    print(f"[Erro] {error_msg}")
                    return {"success": False, "message": error_msg}
            except subprocess.TimeoutExpired:
                # Se passou de 1.5s sem fechar, significa que o jogo abriu com sucesso!
                pass

            # Fecha o launcher se o jogo foi iniciado com sucesso
            if self._window:
                self._window.destroy()

            return {"success": True, "message": "Jogo iniciado com sucesso!"}

        except Exception as e:
            return {"success": False, "message": f"Erro ao executar o jogo: {str(e)}"}


api = LauncherAPI()

html_code = """
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<style>
    * {
        box-sizing: border-box;
        margin: 0;
        padding: 0;
        user-select: none;
    }
    body {
        background-color: #0d0f17;
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
        color: #e2e8f0;
        display: flex;
        justify-content: center;
        align-items: center;
        height: 100vh;
        padding: 16px;
        overflow: hidden;
    }
    .gui-container {
        background: linear-gradient(145deg, #181a26 0%, #0f111a 100%);
        padding: 24px;
        border-radius: 20px;
        box-shadow: 0 15px 35px rgba(0, 0, 0, 0.6), inset 0 1px 0 rgba(255, 255, 255, 0.1);
        width: 100%;
        max-width: 580px;
        border: 1px solid rgba(255, 255, 255, 0.08);
    }
    .gui-header {
        text-align: center;
        margin-bottom: 24px;
    }
    .gui-title {
        font-size: 50px;
        font-weight: 900;
        background: linear-gradient(45deg, #ffcc00, #ff6600);
        -webkit-background-clip: text;
        -webkit-text-fill-color: transparent;
        letter-spacing: -0.5px;
        text-shadow: 0 4px 12px rgba(255, 102, 0, 0.2);
    }
    .gui-subtitle {
        font-size: 13px;
        color: #94a3b8;
        margin-top: 4px;
        text-transform: uppercase;
        letter-spacing: 1.5px;
        font-weight: 700;
    }
    .field-container {
        background: rgba(255, 255, 255, 0.03);
        padding: 16px 18px;
        border-radius: 14px;
        margin-bottom: 16px;
        border: 1px solid rgba(255, 255, 255, 0.06);
        transition: border-color 0.2s, background 0.2s;
    }
    .field-container:hover {
        background: rgba(255, 255, 255, 0.05);
        border-color: rgba(255, 204, 0, 0.25);
    }
    .field-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 6px;
    }
    .field-title {
        font-size: 15px;
        font-weight: 700;
        color: #ffffff;
    }
    .field-subtext {
        font-size: 11px;
        color: #64748b;
        line-height: 1.4;
        font-family: 'Consolas', 'Courier New', monospace;
        word-break: break-all;
        margin-bottom: 12px;
        background: rgba(0, 0, 0, 0.25);
        padding: 6px 10px;
        border-radius: 6px;
        border: 1px solid rgba(255, 255, 255, 0.03);
    }
    .file-input-wrapper {
        display: flex;
        align-items: center;
        gap: 12px;
    }
    .btn-action {
        background: linear-gradient(135deg, #2563eb 0%, #1d4ed8 100%);
        border: none;
        color: white;
        padding: 9px 16px;
        border-radius: 8px;
        font-size: 12px;
        font-weight: 700;
        cursor: pointer;
        transition: transform 0.1s, opacity 0.2s, box-shadow 0.2s;
        white-space: nowrap;
        box-shadow: 0 2px 8px rgba(37, 99, 235, 0.3);
    }
    .btn-action:hover {
        opacity: 0.95;
        transform: translateY(-1px);
        box-shadow: 0 4px 12px rgba(37, 99, 235, 0.4);
    }
    .btn-action:active {
        transform: translateY(0);
    }
    .file-path-display {
        font-size: 12px;
        color: #94a3b8;
        font-family: 'Consolas', monospace;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
        flex-grow: 1;
    }
    .status-badge {
        font-size: 12px;
        font-weight: 700;
        padding: 3px 8px;
        border-radius: 6px;
        display: inline-flex;
        align-items: center;
        gap: 4px;
        transition: all 0.3s ease;
    }
    .status-badge.neutral {
        background: rgba(148, 163, 184, 0.1);
        color: #64748b;
    }
    .status-badge.valid {
        background: rgba(34, 197, 94, 0.15);
        color: #4ade80;
        border: 1px solid rgba(34, 197, 94, 0.3);
    }
    .status-badge.invalid {
        background: rgba(239, 68, 68, 0.15);
        color: #f87171;
        border: 1px solid rgba(239, 68, 68, 0.3);
    }
    .status-badge.loading {
        background: rgba(234, 179, 8, 0.15);
        color: #facc15;
    }
    .error-msg {
        color: #f87171;
        font-size: 11px;
        margin-top: 6px;
        display: none;
        font-weight: 600;
    }
    .footer-bar {
        border-top: 1px solid rgba(255, 255, 255, 0.08);
        padding-top: 20px;
        margin-top: 20px;
        display: flex;
        justify-content: space-between;
        align-items: center;
    }
    .footer-info {
        font-size: 11px;
        color: #64748b;
    }
    .btn-play {
        background: linear-gradient(135deg, #e11d48 0%, #be123c 100%);
        box-shadow: 0 4px 15px rgba(225, 29, 72, 0.4);
        border: none;
        color: white;
        padding: 12px 32px;
        border-radius: 30px;
        font-size: 14px;
        font-weight: 800;
        cursor: pointer;
        letter-spacing: 1px;
        transition: all 0.2s ease;
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .btn-play:hover:not(:disabled) {
        transform: scale(1.04);
        box-shadow: 0 6px 20px rgba(225, 29, 72, 0.6);
    }
    .btn-play:active:not(:disabled) {
        transform: scale(0.98);
    }
    .btn-play:disabled {
        background: #27272a;
        color: #52525b;
        box-shadow: none;
        cursor: not-allowed;
        opacity: 0.6;
    }
</style>
</head>
<body>

<div class="gui-container">
    <div class="gui-header">
        <div class="gui-title">Bigodão</div>
        <div class="gui-subtitle">Wario Land: Super Mario Land 3</div>
    </div>

    <!-- Campo 1: Seleção da ROM -->
    <div class="field-container">
        <div class="field-header">
            <div class="field-title">Wario Land - Super Mario Land 3 (World)</div>
            <div id="rom-status" class="status-badge neutral">Pendente</div>
        </div>
        <div class="field-subtext">sha256sum: ac1682f17abcf590311a233289ee325214c2d71ab3a5aa175004002d85075e56</div>
        <div class="file-input-wrapper">
            <button class="btn-action" onclick="buscarRom()">Buscar ROM</button>
            <span id="rom-path" class="file-path-display">Nenhum arquivo selecionado</span>
        </div>
        <div id="rom-error" class="error-msg">✖ SHA256 incorreto! Por favor, selecione a ROM original correta.</div>
    </div>

    <!-- Campo 2: Seleção do Patch IPS -->
    <div class="field-container">
        <div class="field-header">
            <div class="field-title">Wario Land - Super Mario Land 3 DX</div>
            <div id="patch-status" class="status-badge neutral">Pendente</div>
        </div>
        <div class="field-subtext">
            author: korxo | Version 1.2<br>
            sha256sum: 599d9f59090df3e1e77c1678e069290a2cd9ce59c71cf58d66c4dc3db8b7facd
        </div>
        <div class="file-input-wrapper">
            <button class="btn-action" onclick="buscarPatch()">Buscar Patch IPS</button>
            <span id="patch-path" class="file-path-display">Nenhum arquivo selecionado</span>
        </div>
        <div id="patch-error" class="error-msg">✖ SHA256 incorreto! O arquivo de patch IPS não corresponde.</div>
    </div>

    <!-- Rodapé com botão LOAD & PLAY -->
    <div class="footer-bar">
        <div class="footer-info">Status: <span id="launcher-status">Aguardando arquivos</span></div>
        <button id="btn-play" class="btn-play" disabled onclick="jogar()">▶ LOAD & PLAY</button>
    </div>
</div>

<script>
    let state = {
        romPath: "",
        romValid: false,
        patchPath: "",
        patchValid: false
    };

    function updatePlayButton() {
        const playBtn = document.getElementById('btn-play');
        const launcherStatus = document.getElementById('launcher-status');

        if (state.romValid && state.patchValid) {
            playBtn.disabled = false;
            launcherStatus.innerText = "Pronto para jogar!";
            launcherStatus.style.color = "#4ade80";
        } else {
            playBtn.disabled = true;
            if (!state.romPath || !state.patchPath) {
                launcherStatus.innerText = "Selecione a ROM e o Patch";
            } else {
                launcherStatus.innerText = "Arquivos inválidos";
            }
            launcherStatus.style.color = "#64748b";
        }
    }

    async function buscarRom() {
        if (!window.pywebview || !window.pywebview.api) {
            alert("Aguarde a inicialização do launcher...");
            return;
        }

        const statusEl = document.getElementById('rom-status');
        const errorEl = document.getElementById('rom-error');
        const pathEl = document.getElementById('rom-path');

        statusEl.className = "status-badge loading";
        statusEl.innerText = "Verificando...";

        try {
            const res = await window.pywebview.api.select_rom();

            if (res.cancelled) {
                if (!state.romPath) {
                    statusEl.className = "status-badge neutral";
                    statusEl.innerText = "Pendente";
                }
                return;
            }

            pathEl.innerText = res.filename || res.path;
            state.romPath = res.path;
            state.romValid = res.valid;

            if (res.valid) {
                statusEl.className = "status-badge valid";
                statusEl.innerText = "✓ Válido";
                errorEl.style.display = "none";
                pathEl.style.color = "#4ade80";
            } else {
                statusEl.className = "status-badge invalid";
                statusEl.innerText = "✖ Invalido";
                errorEl.style.display = "block";
                pathEl.style.color = "#f87171";
            }
        } catch (err) {
            console.error("Erro ao selecionar ROM:", err);
            statusEl.className = "status-badge invalid";
            statusEl.innerText = "Erro";
        }

        updatePlayButton();
    }

    async function buscarPatch() {
        if (!window.pywebview || !window.pywebview.api) {
            alert("Aguarde a inicialização do launcher...");
            return;
        }

        const statusEl = document.getElementById('patch-status');
        const errorEl = document.getElementById('patch-error');
        const pathEl = document.getElementById('patch-path');

        statusEl.className = "status-badge loading";
        statusEl.innerText = "Verificando...";

        try {
            const res = await window.pywebview.api.select_patch();

            if (res.cancelled) {
                if (!state.patchPath) {
                    statusEl.className = "status-badge neutral";
                    statusEl.innerText = "Pendente";
                }
                return;
            }

            pathEl.innerText = res.filename || res.path;
            state.patchPath = res.path;
            state.patchValid = res.valid;

            if (res.valid) {
                statusEl.className = "status-badge valid";
                statusEl.innerText = "✓ Válido";
                errorEl.style.display = "none";
                pathEl.style.color = "#4ade80";
            } else {
                statusEl.className = "status-badge invalid";
                statusEl.innerText = "✖ Invalido";
                errorEl.style.display = "block";
                pathEl.style.color = "#f87171";
            }
        } catch (err) {
            console.error("Erro ao selecionar Patch:", err);
            statusEl.className = "status-badge invalid";
            statusEl.innerText = "Erro";
        }

        updatePlayButton();
    }

    async function jogar() {
        if (!state.romValid || !state.patchValid) {
            alert("Certifique-se de que a ROM e o Patch possuem hashes válidos!");
            return;
        }

        const res = await window.pywebview.api.launch_game(state.romPath, state.patchPath);
        if (!res.success) {
            alert("Erro ao iniciar o jogo: " + res.message);
        }
    }
</script>

</body>
</html>
"""

if __name__ == '__main__':
    window = webview.create_window(
        'Bigodão Launcher', 
        html=html_code, 
        js_api=api,
        width=620, 
        height=580,
        resizable=False
    )
    # Atribui a janela para a API poder chamar o seletor nativo de arquivos
    api.set_window(window)
    
    # Inicia a aplicação pywebview
    webview.start(gui="qt")