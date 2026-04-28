
![PrusaSlicer logo](/resources/icons/PrusaSlicer.png?raw=true)

# PrusaSlicer

You may want to check the [PrusaSlicer project page](https://www.prusa3d.com/prusaslicer/).
Prebuilt Windows, OSX and Linux binaries are available through the [git releases page](https://github.com/prusa3d/PrusaSlicer/releases) or from the [Prusa3D downloads page](https://www.prusa3d.com/drivers/). There are also [3rd party Linux builds available](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-on-Linux---binary-distributions).

PrusaSlicer takes 3D models (STL, OBJ, AMF) and converts them into G-code
instructions for FFF printers or PNG layers for mSLA 3D printers. It's
compatible with any modern printer based on the RepRap toolchain, including all
those based on the Marlin, Prusa, Sprinter and Repetier firmware. It also works
with Mach3, LinuxCNC and Machinekit controllers.

## Run from this repository (Windows)

This repository includes a portable Windows runtime build in the root directory.
After cloning or downloading the ZIP, launch:

- `prusa-slicer.exe` (GUI)
- `prusa-slicer-console.exe` (CLI)
- `prusa-gcodeviewer.exe` (G-code viewer)

Keep these binaries next to `resources/` and the bundled DLL files in the repository root.

## AI Extension Features

This fork includes an integrated AI assistant in the right sidebar panel.

- Sidebar chat panel with conversation history, status line, `New Chat`, and single-line input (`Enter` sends).
- Agent execution mode toggle (`Agent`) to let AI run in-app actions, with chat-only fallback when disabled.
- First-time Agent enable confirmation dialog, centered on screen, including a `Why?` expandable explanation.
- Vision toggle (`Vision`) to attach an image of the current 3D viewport.
- Viewport capture is taken as currently shown (`current_viewport_as_is`) without auto-centering.
- Configurable viewport snapshot size in AI settings (`256`, `384`, `448`, `512`, `640` px).
- AI settings page in Preferences: provider, model, base URL, API key, vision options, and persisted AI mode flags.
- `Factory reset AI` button clears API key and restores all AI settings to defaults.
- Multi-provider integration: `openai_compatible`, `claude` (Anthropic Messages API), and `gemini` (Google Generative Language API).
- Robust provider retry handling for truncated responses, including OpenAI-compatible token-parameter fallback (`max_tokens` / `max_completion_tokens`).
- Scene-aware runtime context (selection/object geometry and project state) plus optional viewport image context.
- Structured tool/action layer (98 actions) for common workflows: select/move/rotate/scale/arrange, import/export, split/cut/repair, slicing, project I/O, preset/setting updates, print-host actions, and undo/redo.
- Action safety guards to reduce unintended destructive/file/printer operations unless explicitly requested in the prompt.

Provider defaults:

- `openai_compatible`: `https://api.openai.com/v1/chat/completions`
- `claude`: `https://api.anthropic.com/v1/messages`
- `gemini`: `https://generativelanguage.googleapis.com/v1beta/models`

PrusaSlicer is based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [project homepage](https://www.prusa3d.com/slic3r-prusa-edition/) and
the [documentation directory](doc/) for more information.

### What language is it written in?

All user facing code is written in C++.
The slicing core is the `libslic3r` library, which can be built and used in a standalone way.
The command line interface is a thin wrapper over `libslic3r`.

### What are PrusaSlicer's main features?

Key features are:

* **multi-platform** (Linux/Mac/Win) and packaged as standalone-app with no dependencies required
* complete **command-line interface** to use it with no GUI
* multi-material **(multiple extruders)** object printing
* multiple G-code flavors supported (RepRap, Makerbot, Mach3, Machinekit etc.)
* ability to plate **multiple objects having distinct print settings**
* **multithread** processing
* **STL auto-repair** (tolerance for broken models)
* wide automated unit testing

Other major features are:

* combine infill every 'n' perimeters layer to speed up printing
* **3D preview** (including multi-material files)
* **multiple layer heights** in a single print
* **spiral vase** mode for bumpless vases
* fine-grained configuration of speed, acceleration, extrusion width
* several infill patterns including honeycomb, spirals, Hilbert curves
* support material, raft, brim, skirt
* **standby temperature** and automatic wiping for multi-extruder printing
* [customizable **G-code macros**](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-Macro-Language) and output filename with variable placeholders
* support for **post-processing scripts**
* **cooling logic** controlling fan speed and dynamic print speed

### Development

If you want to compile the source yourself, follow the instructions on one of
these documentation pages:
* [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
* [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)
* [Windows](doc/How%20to%20build%20-%20Windows.md)

### Can I help?

Sure! You can do the following to find things that are available to help with:
* Add an [issue](https://github.com/prusa3d/PrusaSlicer/issues) to the github tracker if it isn't already present.
* Look at [issues labeled "volunteer needed"](https://github.com/prusa3d/PrusaSlicer/issues?utf8=%E2%9C%93&q=is%3Aopen+is%3Aissue+label%3A%22volunteer+needed%22)

### What's PrusaSlicer license?

PrusaSlicer is licensed under the _GNU Affero General Public License, version 3_.
The PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

### How can I use PrusaSlicer from the command line?

Please refer to the [Command Line Interface](https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface) wiki page.
