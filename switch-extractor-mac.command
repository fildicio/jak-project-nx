#!/bin/bash
# OpenGOAL Switch Build App - double-click launcher (macOS)
cd "$(dirname "$0")"
exec python3 scripts/switch/extractor_app.py
