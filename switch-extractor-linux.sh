#!/bin/bash
# OpenGOAL Switch Build App - launcher (Linux)
cd "$(dirname "$0")"
exec python3 scripts/switch/extractor_app.py
