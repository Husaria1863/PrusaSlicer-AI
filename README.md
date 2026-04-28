![PrusaSlicer logo](/resources/icons/PrusaSlicer.png?raw=true)

# PrusaSlicer AI Fork

This project is a PrusaSlicer fork with an integrated AI assistant for faster model and print setup workflows.

## Quick Start (Windows)

This repository includes a portable runtime build in the root directory.
After cloning or downloading the ZIP, run:

- `prusa-slicer.exe` (GUI)
- `prusa-slicer-console.exe` (CLI)
- `prusa-gcodeviewer.exe` (G-code viewer)

Keep these binaries next to `resources/` and the bundled DLL files.

## Core Features

- Full PrusaSlicer workflow for FFF and mSLA printing.
- 3D scene editing, slicing, presets, and G-code export.
- Command-line support for automation and batch use.
- Source tree for Windows/macOS/Linux builds.

## AI Features

- Sidebar AI chat with single-line input (`Enter` to send).
- `Agent` mode to execute in-app actions with built-in safety checks.
- `Vision` mode to attach the current viewport snapshot (captured as-is).
- AI settings for provider, model, base URL, API key, and snapshot size.
- Factory reset option for all AI settings and stored key state.
- Structured action engine for common edit/slice/import/export workflows.

Supported providers:

- `openai_compatible`
- `claude` (Anthropic Messages API)
- `gemini` (Google Generative Language API)

Default endpoints:

- `openai_compatible`: `https://api.openai.com/v1/chat/completions`
- `claude`: `https://api.anthropic.com/v1/messages`
- `gemini`: `https://generativelanguage.googleapis.com/v1beta/models`

## Build from Source

- [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
- [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)
- [Windows](doc/How%20to%20build%20-%20Windows.md)

## Links

- PrusaSlicer project: https://www.prusa3d.com/prusaslicer/
- Base project: [Slic3r](https://github.com/Slic3r/Slic3r)
- CLI reference: https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface
- License: GNU AGPLv3
