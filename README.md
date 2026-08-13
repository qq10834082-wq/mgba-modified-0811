# mGBA Modified (0.10-modified-0811)

[mGBA](https://mgba.io/) is a well-known GBA emulator recognized for its high accuracy and rich feature set.

- **Official website**: [https://mgba.io/](https://mgba.io/)
- **Official repository**: [https://github.com/mgba-emu/mgba](https://github.com/mgba-emu/mgba)

This repository contains a modified version based on the official **0.10.5** branch `0.10-modified-0811`.

Its primary purpose is to **integrate an Agent workflow**, allowing agents to read memory and export same-frame snapshots while a game is running, which helps locate text, pointer, and runtime issues.

Not sure what to name this, so I used the github repo creation date.

## New Features

### 1. Remote RPC Debug Port

When the Qt frontend starts, it listens for a local **TCP JSON-RPC** service if **Settings → Agent** is enabled (on by default). The default bind address is `127.0.0.1:8765`. The service is localhost-only unless you change the host; it is newline-delimited JSON and accepts **one request object per line**. Each response is one compact JSON object followed by a newline.

`params`, when present, must be a JSON object. Integer fields must be finite, non-negative whole numbers. Without a loaded ROM, only `ping`, `get_info`, and `load_rom` succeed; every other method returns `no ROM loaded`.

`load_rom` accepts only an **absolute local path** and loads through the Qt path `CoreManager::loadGame` → `Window::setController` → `CoreController::start`. After that path finishes, the agent loader waits until emulation is paused and returns `paused: true`. Native GUI loaders (File → Load ROM and the other built-in entry points) are not wired to that pause: with Autoload enabled (the default), they restore save slot 0 and then **run**.

GBA controller input is separate from Qt keyboard shortcuts and uses the native GBA key path. CPU instruction/register injection is intentionally not part of this interface. CPU/DMA event tracing is also not exposed; `watch_add` is the documented frame-boundary alternative.

#### Methods

Control methods that advance or pause the core typically return `{ "frame", "paused", "input" }`. Frame-advance helpers also return `start_frame`, `end_frame`, and `watch_hits`. `input` lists keys currently held through **this RPC connection**.

| Method | Params | Result |
| --- | --- | --- |
| `ping` | none | `"pong"` |
| `get_info` | none | With a ROM: `title`, `code`, `platform`, `frame`, `paused`, `rom_loaded: true`, `input`, and `pc` / `lr` when the core exposes them. With no ROM: `rom_loaded: false`, `frame: 0`, `paused: true`, `input`. |
| `load_rom` | `{ "path": "<absolute local ROM>" }` | ROM info plus `rom_loaded: true`, `paused: true`, `input`, `rom_path`. |
| `read8` / `read16` / `read32` | `{ "address": integer }` | Unsigned integer from the GBA bus. |
| `read_range` | `{ "address": integer, "length": 1..4096 }` | Array of bytes. |
| `read_block` | `{ "address": integer, "length": 1..65536 }` | `{ "address", "length", "base64" }`. |
| `write8` / `write16` / `write32` | `{ "address": integer, "value": integer }` | `true`. `value` must fit the selected width (8/16/32-bit). |
| `pause` | none | `{ "frame", "paused", "input" }` after pausing at a frame boundary. |
| `unpause` / `resume` | none | Same shape after continuing emulation. `resume` is an alias of `unpause`. |
| `reset` | none | Same shape after `CoreController::reset()`. |
| `step_frame` | optional `{ "fps" }` or `{ "frame_interval_ms" }` | Runs **exactly one** frame, then remains paused. `count` is rejected. `pause_after` must be omitted or `true`. |
| `advance_frames` | same as `run_frames` | Compatibility alias of `run_frames`. Default `count` is 1. |
| `run_frames` | `{ "count": 1..1000, "pause_after": bool, "fps": 0..240 }` **or** `{ ..., "frame_interval_ms": 0..1000 }` | Runs `count` frames (default 1). `pause_after` defaults to `true`. Specify `fps` or `frame_interval_ms`, not both. `0` means run as quickly as possible. Requested waiting is capped at 60 seconds and is performed on the RPC/UI thread, never inside the emulation callback. |
| `key_down` | `{ "key": "<name>" }` | `{ "frame", "paused", "input" }` after pressing one whitelisted key. |
| `key_up` | `{ "key": "<name>" }` | Same shape after releasing that key. |
| `press_key` | `{ "key", "hold_frames": 0..1000, "wait_frames": 0..1000, "pause_after" }` | Atomically presses, advances `hold_frames` (default 1), releases, then advances `wait_frames` (default 0). `hold_frames + wait_frames` must be ≤ 1000. `pause_after` defaults to `true`. Returns `start_frame`, `end_frame`, `frame`, `paused`, `input`, `watch_hits`. |
| `save_state` | `{ "slot": 1..9 }` | ROM info plus `slot` and `operation: "save_state"`. |
| `load_state` | `{ "slot": 1..9 }` | Restores the slot, then pauses. Returns ROM info plus `slot` and `operation: "load_state"`. |
| `export_snapshot` | optional `{ "focus_addresses": [up to 16 integers] }` | `{ "path", "frame", "paused" }`. See section 2 for the files written into `path`. |
| `memory_diff` | `{ "address", "length": 1..4096, "max_changes": 1..1024 }` | First sample of a range (or a changed address/length) returns `initialized: true` and an empty `changes` array. Later samples of the same range return byte diffs, capped by `max_changes` (default 256). |
| `watch_add` | `{ "address", "length": 1..4096, "access": "write"\|"read"\|"rw", "pause_on_hit": bool }` | At most 32 watches. `access` defaults to `"write"`; `pause_on_hit` defaults to `true`. Returns the watch plus `precision`. These are **frame-boundary polling** watches: they compare sampled bytes after a frame and may miss changes inside a frame. They do not intercept every CPU access. `access` is a stored label; all modes detect sampled differences. |
| `watch_remove` | `{ "id": integer }` | `true`, or an error if the watch is missing. |
| `watch_list` | none | `{ "watches": [...], "precision": "frame-boundary polling; changes may be missed within a frame" }`. |

#### GBA key whitelist

`key_down`, `key_up`, and `press_key` accept these names (case-insensitive). Unknown keys are rejected.

`A`, `B`, `L`, `R`, `Start`, `Select`, `Up`, `Down`, `Left`, `Right`

`input` reports held keys in uppercase: `A`, `B`, `SELECT`, `START`, `RIGHT`, `LEFT`, `UP`, `DOWN`, `R`, `L`.

#### Cursor debugging workflow

Typical single-character / dialogue inspection after a ROM is already running:

```json
{"id":1,"method":"pause","params":{}}
{"id":2,"method":"press_key","params":{"key":"A","hold_frames":6,"wait_frames":120}}
{"id":3,"method":"export_snapshot","params":{}}
{"id":4,"method":"read_range","params":{"address":33554432,"length":64}}
```

Or load through RPC (this path finishes paused; GUI Load ROM does not):

```json
{"id":1,"method":"load_rom","params":{"path":"C:\\roms\\game.gba"}}
{"id":2,"method":"press_key","params":{"key":"A","hold_frames":6,"wait_frames":120}}
{"id":3,"method":"export_snapshot","params":{"focus_addresses":[33558592]}}
{"id":4,"method":"read_block","params":{"address":33558592,"length":64}}
```

#### MCP wrapper (outside this repository)

A Cursor MCP wrapper lives **outside this Git repository** at `C:\Users\ochib\Desktop\gba\mcp-agent`. Do not copy that tree into this repo. It speaks the same newline-delimited TCP JSON-RPC on `127.0.0.1:8765` and exposes tools named `mgba_<method>` (`mgba_ping`, `mgba_press_key`, `mgba_export_snapshot`, and so on).

In **Settings → Agent**, you can enable or disable the service, change the host / port, and configure which memory regions are included when exporting snapshots.

### 2. Same-Frame Screenshot + Memory Export

**Audio/Video → Export agent snapshot** (you can bind the shortcut to `agentSnapshot` in **Settings → Shortcuts**) atomically exports the following from the **same frame**:

- `screenshot.png` — The current screen
- `memory.bin` — A memory snapshot of the configurable regions (EWRAM / IWRAM / VRAM by default)
- `metadata.json` — Frame number, ROM path/title/code, `paused`, CPU input mask, PC/LR when the core exposes them, optional same-frame 32-bit `focus_values`, and offsets of the memory blocks

Output directory: `{ROM directory}/{ROM main filename}/{timestamp}_f{frame number}/`

This is primarily intended to let **multimodal models** analyze issues using the screen and memory context, such as missing dialogue or invalid pointers.

It can also be triggered through the `export_snapshot` RPC method without manually selecting the menu item. That call accepts at most 16 optional `focus_addresses` and records their same-frame 32-bit values in `metadata.json`. Capture is atomic for one emulated frame; PNG encode and disk I/O run after the core is no longer frozen.

## Build

```powershell
cmake -B build-qt -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DBUILD_QT=ON
cmake --build build-qt --config Release
```
