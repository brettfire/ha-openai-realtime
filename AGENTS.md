# AGENTS.md

Operational notes for AI agents (Claude Code etc.) working in this repo.
Humans should read [README.md](README.md) first.

## Repo layout

Two coupled components ship from this repo. Both must be in sync for
anything to work end-to-end:

| Path | What it is | Deploys to |
|---|---|---|
| `openai_realtime_voice_agent/` | Home Assistant **add-on**. Pipecat-based Python server that bridges WebSocket audio from the satellite to OpenAI's Realtime API and to HA's MCP server. | HAOS, via Docker image published from `.github/workflows/build-addon.yml` to `ghcr.io/brettfire/ha-openai-realtime/openai-realtime-voice-agent-{arch}`. |
| `home-assistant-voice-pe/` | ESPHome firmware for the **Voice PE satellite**. Includes a custom `voice_assistant_websocket` component that streams mic audio to the addon and plays back the response. | ESP32-S3 voice device, via OTA from the HA ESPHome addon or locally via `esphome run`. |

The two communicate over a custom WebSocket protocol: binary frames =
raw PCM audio (24 kHz, 16-bit, mono), text frames = JSON control
messages (currently just `{"type": "interrupt"}`).

## Debugging the addon on a running HAOS

The **recommended MCP server for AI agents working on Home Assistant**
is [homeassistant-ai/ha-mcp](https://github.com/homeassistant-ai/ha-mcp)
— the same one this repo uses for all addon debugging. Install it as
an HA addon (slug `81f33d0f_ha_mcp` once added). It exposes ~70+
tools covering addons, integrations, entities, areas, services,
automations, scripts, dashboards, backups, logs, etc., and ships with
LLM-facing skills that document HA best practices. Far richer than
HA's built-in `mcp_server` integration (which is more focused on the
end-user Assist experience).

Key tools for this project:

- `ha_get_addon(slug=...)` — config, state, options, network. Use
  `ha_get_addon(source="installed")` first to discover the actual slug
  (e.g. `24a8fe2c_openai_realtime_voice_agent` for brettfire fork).
- `ha_get_logs(source="supervisor", slug=<addon_slug>, search=<term>, limit=N)`
  — addon stdout/stderr. The `search` filter is a literal substring
  (not regex / not `|`-OR). Run multiple calls if you need OR-of-terms.
- `ha_get_logs(source="system_service", slug="supervisor", search=...)` —
  the Supervisor's own logs (image pulls, build attempts, errors when
  HAOS itself rejects something).
- `ha_get_logs(source="error_log", search=...)` — HA Core's
  `home-assistant.log` (where HA-side errors live, e.g. mcp_server 401s).
- `ha_manage_addon(slug=..., options={...})` — update addon options.
  Returns `pending_restart`; an addon restart is required for them to
  take effect. **Caveat:** the supervisor logs the *current* options
  (including secrets) in plaintext when you `ha_get_addon` — don't
  paste them anywhere.
- `ha_call_service(domain="hassio", service="addon_restart", data={"addon": "<slug>"}, wait=false)`
  — restart the addon after a config change. Sometimes fails with a
  generic `400 Bad Request` for opaque reasons; ask the user to click
  Restart in the UI as fallback.
- `ha_manage_addon(slug=..., port=..., path=..., method=..., body=...)`
  — proxy HTTP into an addon's own container (useful for hitting an
  MCP server addon, an ESPHome dashboard, etc.). The `port=` argument
  bypasses Ingress; on HAOS this works because the calling addon
  shares the supervisor's network.

### Restart loop that works

The supervisor service `hassio.addon_restart` works fine for most
addons but occasionally rejects with a 400. The reliable manual path
is to ask the user to click Restart in the addon UI. Configuration
changes via `ha_manage_addon(options=...)` always require a restart;
the response will say `pending_restart`.

## Working with the addon (server-side)

### Local build + smoke test on the Mac

Builds happen with `docker buildx`. Docker Desktop is at
`/Applications/Docker.app/Contents/Resources/bin/docker`; prepend it
to `PATH` because the CLI isn't symlinked.

```sh
export PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
cd openai_realtime_voice_agent
docker buildx build \
  --platform linux/arm64 \
  --build-arg BUILD_FROM=ghcr.io/home-assistant/aarch64-base:latest \
  --build-arg BUILD_ARCH=aarch64 \
  -t local-test:dev .
```

`--platform linux/arm64` is needed to match HAOS aarch64. On an Apple
Silicon Mac this is native, fast. On Intel it'll go through QEMU.

Smoke-test by importing the entry point:

```sh
docker run --platform linux/arm64 --rm --entrypoint /usr/bin/python3 local-test:dev -c '
from app.main import Application
from app.mcp_service import HomeAssistantMCPService
print("imports OK")
'
```

For ad-hoc Python inspection inside the image, pipe a script via stdin
— bind-mounting `/tmp` is unreliable with colima:

```sh
docker run --platform linux/arm64 --rm -i \
  --entrypoint /usr/bin/python3 \
  ghcr.io/brettfire/ha-openai-realtime/openai-realtime-voice-agent-aarch64:latest \
  < /tmp/inspect.py
```

### Frozen dependency stack

The build is intentionally pinned to the exact dep set from the last
known-good upstream image (`requirements-frozen.txt`, 110 packages).
`pipecat-ai==0.0.97` transitively requires `numba==0.61.2` which
requires `llvmlite==0.44.0`, and llvmlite's `setup.py` calls
`Distribution.spawn(dry_run=...)` which setuptools removed in 75.0.
The Dockerfile pre-installs `setuptools<70 + wheel<0.43` globally and
builds llvmlite with `--no-build-isolation` so the build env reuses
that older setuptools instead of pulling the latest. Don't loosen
these pins without testing — drift in any of these three packages
breaks the build immediately.

If you ever need to bump pipecat, regenerate `requirements-frozen.txt`
from a successful builder-stage container's `pip freeze`, validate
locally, then commit.

### CI: GHA publishes to ghcr.io

`.github/workflows/build-addon.yml` builds and publishes both
architectures:

- aarch64 on `ubuntu-24.04-arm` (native ARM runner, free for public
  repos — switched in commit `bc3e2d1`).
- amd64 on `ubuntu-latest`.
- GHA cache (`type=gha, mode=max`) is enabled. Cold builds: aarch64
  ~3 min, amd64 ~4 min. Warm (when only `app/` changes): <1 min.

Triggers on push to `main` when `openai_realtime_voice_agent/**` or
the workflow file changes. Also manually dispatchable.
`cancel-in-progress: true` concurrency means the latest push wins —
older in-flight runs are auto-cancelled.

Image visibility on ghcr.io should be **public** so HAOS can pull
without auth. Check with an anonymous `docker pull`.

### Deploying a new addon version

1. Bump `version:` in `openai_realtime_voice_agent/config.yaml` AND
   the matching `repository.json` entry. They must match.
2. Commit + push.
3. Wait for GHA to build + publish (visible at the Actions tab).
4. In HA: addon store → ⋮ → Check for updates. The addon shows
   "Update available → <new>". Click Update.

If the addon's `config.yaml` has an `image:` directive (it does), HAOS
pulls the prebuilt image from ghcr.io instead of building locally.
This is fast. Removing the `image:` line forces a local build, which
is fragile (the same setuptools/llvmlite issues hit again).

### Common addon-side gotchas

- **`http://supervisor/core/api/*`** only works if `config.yaml` has
  `homeassistant_api: true`. Without it the supervisor proxy refuses
  to forward and you get a generic connect failure. Fixed in commit
  `946b1de`.
- The supervisor proxy **only accepts the auto-provided
  `SUPERVISOR_TOKEN`** for `/core/*` calls. User-supplied long-lived
  tokens return 401 here. To use a long-lived token, point at HA core
  directly (not through supervisor).
- Pipecat 0.0.97's `WebsocketServerTransport` emits **only four events**
  (`on_client_connected`, `on_client_disconnected`, `on_session_timeout`,
  `on_websocket_ready`). There is no `on_client_message`. Inbound
  text/JSON control frames must be handled inside the serializer's
  `deserialize()`, which is where we emit `InterruptionFrame` for the
  wake-word interrupt path.
- Pipecat 0.0.97's `MCPClient` has no persistent session — it opens a
  fresh `streamablehttp_client` per `_list_tools` call. To call
  `list_prompts` / `list_resources`, open your own session using the
  url/token already on the service (don't reach into pipecat
  internals). See `mcp_service.fetch_assist_prompt_and_snapshot`.
- HA's `mcp_server` capability advertising changes between minor
  versions. The Assist context snapshot resource
  (`homeassistant://assist/context-snapshot`) was only added in HA
  2026.5 (PR #167396). Use `init_result.capabilities` to gate calls.

## Working with the ESPHome firmware (satellite-side)

### Local toolchain

The user has esphome installed via poetry in `home-assistant-voice-pe/`.
Always cd into that directory first:

```sh
cd home-assistant-voice-pe

# Compile only (validate + build firmware.bin)
poetry run esphome compile voice_pe_config.yaml

# Compile + flash + open logs
poetry run esphome run voice_pe_config.yaml

# Just stream logs from a device that's already running
poetry run esphome logs voice_pe_config.yaml
```

For OTA flashing the device must be reachable on the network with the
encryption key in `secrets.yaml`. For initial USB flashing the device
must be in download mode (hold BOOT, plug in USB).

### After a `.cpp` / `.h` change in the custom component

The change is to `home-assistant-voice-pe/esphome/components/voice_assistant_websocket/`.
ESPHome picks it up automatically on the next `compile`/`run`. No
external publishing step — the source is built into the firmware
image. The change is live the moment the device boots the new
firmware.

### Common firmware-side gotchas

- ESPHome `Action<Ts...>::play()` is declared `void play(const Ts &...x)
  = 0`. If you override with `void play(Ts... x)` (no const-ref) the
  override silently doesn't satisfy the pure virtual, the class stays
  abstract, and you get `error: invalid new-expression of abstract
  class type` at codegen time. Always mirror the signature of
  existing action classes in the same file.
- Custom-component **codegen lives in `__init__.py`**. New actions
  need both the C++ template class in the header AND a
  `@automation.register_action(...)` block in `__init__.py` exposing
  it to YAML.
- HA discovery of new entities is **automatic** when the device
  re-establishes the API connection after an OTA. No need to remove
  + re-add the device. The same encryption key is reused.
- The on-device log buffer is small; use `esphome logs` immediately
  after a reboot to catch startup messages, or filter aggressively.

### Getting device logs

```sh
cd home-assistant-voice-pe
poetry run esphome logs voice_pe_config.yaml
```

This connects to the device's native API (port 6053) and streams the
ESPHome logger output. Press Ctrl+C to stop.

You can also see logs in **HA → Settings → Devices & Services →
ESPHome → <device> → Logs**, but the CLI is faster.

## Quick reference: which version did what

For context when debugging history.

| Addon ver | Commit | Key change |
|---|---|---|
| 0.2.4 | `aa9246c` | Switch addon to pip + frozen reqs; verified locally |
| 0.2.5 | `d07eea9` | Pull from ghcr.io image |
| 0.2.6 | `24231f9` | `fetch_assist_prompt` opens own MCP session |
| 0.2.7 | `83b27cd` | Add snapshot fetch |
| 0.2.9 | `ab41885` | Isolate prompt/snapshot, unwrap TaskGroup |
| 0.2.10 | `4e3149b` | Capability-gate snapshot, document HA 2026.5+ req |
| 0.2.11 | `e123f4b` | INFO log when snapshot capability missing |
| 0.2.12 | `b9724ed` | GHA `concurrency` block |
| 0.2.13 | `4b6a91d` | Serializer emits `InterruptionFrame` on JSON `{"type":"interrupt"}` |

Firmware:
- `5c821a2` — Barge-in Mode select (Wake Word Only / Full Duplex)
- `11506b1` — Fix action signature (const-ref on `play(Ts...)`)
