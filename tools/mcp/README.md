# QIDI Studio MCP on macOS

This development branch starts from official QIDI Studio and adds eleven inspection tools for the open project. It can report objects, plates, edited settings, selected presets, and slice status/results. It exposes no model edits, slicing commands, file operations, or physical printer controls.

The integration is opt-in at both build time (`QIDI_ENABLE_MCP=ON`) and runtime. The endpoint is `http://127.0.0.1:8765/mcp`, with a required bearer token. It binds only IPv4 loopback, validates Host and any Origin header, and supports MCP `2025-06-18` over HTTP POST. GET returns 405; the server does not provide SSE or persistent sessions. A remote ChatGPT connector cannot reach this endpoint directly. Remote access and tunnels are outside this first stage.

## Build on Apple Silicon

The build uses the macOS Command Line Tools, CMake 3.31.6, and Ninja. Most dependency archives are built inside this checkout. CMake and Ninja may be installed in the ignored `.build-tools` environment:

```sh
python3 -m venv .build-tools
.build-tools/bin/python -m pip install cmake==3.31.6 ninja==1.13.2
sh tools/mcp/build-macos.sh
```

Run these commands from the repository root. `QIDI_BUILD_JOBS` defaults to 4. The script accepts `deps` or `app` to resume one phase. Network access is required for the pinned dependency archives and the DeviceWeb frontend build. Certificate validation remains enabled.

The script selects `QDT_RELEASE_TO_PUBLIC=0`, because QIDI's public repository omits private cloud-network source files. It also selects `QDT_INTERNAL_TESTING=1`, which uses the separate `QIDIStudioInternal` application-data directory. The original `BuildMac.sh` still selects the private release configuration; use this development script for the MCP branch.

The output is `build/arm64-mcp/src/QIDIStudio.app`. This workflow does not install it over the existing application. The verified output is a local development bundle: its Resources directory points into this checkout, and its executable links the installed Homebrew Imath and zstd libraries. Those libraries were built for macOS 15 even though the CMake deployment target is 14.0. This artifact has been tested on this Mac; it is not a standalone distribution or proof of macOS 14 compatibility.

## Enable the local endpoint

After the native app has built, run its executable with a token created for this development session:

```sh
export QIDI_STUDIO_MCP=1
export QIDI_STUDIO_MCP_TOKEN="$(openssl rand -hex 32)"
build/arm64-mcp/src/QIDIStudio.app/Contents/MacOS/QIDIStudio
```

Launch the executable directly and keep the command running until the app exits. Append an editable `.3mf` path to open a project. Upstream does not implement `--datadir` or `--no-single-instance` in this CLI; isolation comes from the internal build's separate data directory and executable path. A fresh internal profile shows the ordinary setup wizard before project inspection is useful.

Keep the token local. Clients send `Authorization: Bearer <token>` and `MCP-Protocol-Version: 2025-06-18`. Initialization can be checked from another terminal that has those environment variables:

```sh
curl --silent --show-error http://127.0.0.1:8765/mcp \
  -H "Authorization: Bearer $QIDI_STUDIO_MCP_TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"local-check","version":"1"}}}'
```

The app starts the service after GUI initialization and stops it before destroying the project. Inspections run on the GUI thread, reject project reads during modal dialogs or GUI reconstruction, and have bounded waits that can be cancelled during shutdown. `get_ui_state` can report an open modal dialog. Edited preset values are read from the active editor state rather than assuming the saved preset matches current settings.

## Validation

Run the standalone protocol checks with:

```sh
sh tools/mcp/run-protocol-tests.sh
```

These checks exercise request types, argument validation, the tool allowlist, notification handling, malformed requests, Host/Origin/token/version checks, and parser size bounds.

September 30, 2026 validation on Apple Silicon completed:

- Full dependency and application build, including all 710 application targets and the frontend.
- Native HTTP initialization and eleven-tool listing; all inspection tools exercised against the existing 20 mm PETG cube project.
- Geometry and effective settings matched the project: 0.2 mm layers, two walls, 15% grid infill, 250°C nozzle, 70°C textured plate, and 8 mm³/s volumetric limit. The fresh internal profile mapped missing custom preset names to base presets with edited values; the tool reported those active names and edits.
- A temporary layer-height edit to 0.24 mm appeared through MCP before saving; it was restored to 0.2 mm. The source project was not saved or replaced.
- The native sliced result reported 100 layers. Studio also reported its `bed_temperature_too_high_than_filament` warning; this test did not send the newly sliced file to a printer.
- Missing/wrong authentication, foreign Host/Origin, unsupported protocol versions, GET requests, malformed JSON, and an unknown printer-control tool were rejected as expected. Initialization notifications returned 202 without a body.
- An open modal was reported by `get_ui_state`; project inspection was blocked until it was dismissed.
- An incomplete request timed out at five seconds; another pending request was cancelled when the development app exited. The port closed, the app exited with code 0, and the installed QIDI Studio process remained running.
- Relaunch succeeded and read the Q2 printer model and 0.4 mm nozzle settings. The second development instance also exited with code 0.

Evidence is saved in the ignored `.build-tools/runtime` directory and `build/arm64-mcp/native-build.log`. Packaging, connection to an MCP client application, and broader multi-plate/multi-filament coverage remain future work. This branch exposes local inspection, not printer control.

## Source and reuse

The base is [official QIDI Studio](https://github.com/QIDITECH/QIDIStudio), commit `c58fdc5`. The project/plate inspection serializers and GUI dispatch pattern are adapted from [sslinkyy/Anthony Yorba's MCP fork](https://github.com/sslinkyy/QIDIStudio), commit `d85c905619f6637eed24b6967a4a8659ab3641bf` (v1.11.0). The restricted protocol boundary, authentication, and teardown were modified September 30, 2026. The existing AGPL v3 license and upstream notices remain in place.

Fork source: [davidrd123/QIDIStudio](https://github.com/davidrd123/QIDIStudio), branch `codex/macos-mcp`.
