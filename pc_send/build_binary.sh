#!/usr/bin/env bash
# ==============================================================================
# Script de compilación del binario del emisor de audio para PC (Linux / Linux GUI)
# Genera el ejecutable autónomo 'pc_audio_sender' a partir de pc_audio_sender.py
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Compilando Binario PC Audio Sender ==="

# 1. Verificar dependencias de construcción
python3 -m pip install pyinstaller PyQt6 sounddevice soundfile --break-system-packages 2>/dev/null || true

if ! command -v pyinstaller &> /dev/null && ! python3 -m PyInstaller &> /dev/null; then
    echo "❌ Error: PyInstaller no está instalado."
    exit 1
fi

# 2. Limpieza de carpetas temporales anteriores
rm -rf build dist *.spec

echo "⚡ Compilando con PyInstaller (OneFile GUI Executable)..."

# 3. Ejecutar PyInstaller
python3 -m PyInstaller \
    --noconfirm \
    --onefile \
    --windowed \
    --name "pc_audio_sender" \
    --clean \
    pc_audio_sender.py

# 4. Mover el binario compilado a la carpeta actual
if [ -f "dist/pc_audio_sender" ]; then
    mv "dist/pc_audio_sender" "./pc_audio_sender"
    chmod +x "./pc_audio_sender"
    
    # Limpieza final de temporales
    rm -rf build dist *.spec
    
    echo "============================================================"
    echo "✅ Binario creado con éxito: $SCRIPT_DIR/pc_audio_sender"
    echo "   Tamaño: $(du -h ./pc_audio_sender | cut -f1)"
    echo "============================================================"
else
    echo "❌ Error: No se generó el binario en dist/pc_audio_sender"
    exit 1
fi
