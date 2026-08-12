# mGBA Modified (0.10-modified-0811)

[mGBA](https://mgba.io/) is a well-known GBA emulator recognized for its high accuracy and rich feature set.

- **Official website**: [https://mgba.io/](https://mgba.io/)
- **Official repository**: [https://github.com/mgba-emu/mgba](https://github.com/mgba-emu/mgba)

This repository contains a modified version based on the official **0.10.5** branch `0.10-modified-0811`.
Its primary purpose is to **integrate an Agent workflow**, allowing agents to read memory and export same-frame snapshots while a game is running, which helps locate text, pointer, and runtime issues.
Not sure what to name this, so I used the github repo creation date.   

## New Features



### 1. Remote RPC Debug Port

After mGBA starts and loads a ROM, it automatically starts a local **TCP JSON-RPC** service (defaulting to `127.0.0.1:8765`) that allows Agents and scripts to query the state of the running emulator, including:

- Reading memory at a specified address (`read8` / `read16` / `read32` / `read_range`)
- Writing memory, advancing frames one at a time, pausing / resuming, and resetting
- Getting the current ROM information and frame number (`get_info`)

In **Settings → Agent**, you can enable or disable the service, change the Host / port, and configure which memory regions are included when exporting snapshots.

### 2. Same-Frame Screenshot + Memory Export

**Audio/Video → Export agent snapshot** (you can bind the shortcut to `agentSnapshot` in **Settings → Shortcuts**) atomically exports the following from the **same frame**:

- `screenshot.png` — The current screen
- `memory.bin` — A memory snapshot of the configurable regions (EWRAM / IWRAM / VRAM by default)
- `metadata.json` — Metadata such as the frame number, ROM path, and offsets of the memory blocks

Output directory: `{ROM directory}/{ROM main filename}/{timestamp}_f{frame number}/`

This is primarily intended to let **multimodal models** analyze issues using the screen and memory context, such as missing dialogue or invalid pointers.

It can also be triggered through the `export_snapshot` RPC method without manually selecting the menu item.

## Build

```powershell
cmake -B build-qt -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DBUILD_QT=ON
cmake --build build-qt --config Release
```

