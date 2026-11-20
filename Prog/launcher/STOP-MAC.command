#!/bin/bash
# Останавливает сервер стенда на macOS — двойной клик по этому файлу.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/stop-stand.sh"
