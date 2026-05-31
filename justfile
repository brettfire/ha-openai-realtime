# justfile — common dev commands for ha-openai-realtime
#
# Run `just` (no args) to see the list. Requires `just`:
#   brew install just
# All paths are repo-relative.

# Show all recipes.
default:
    @just --list

# ---------------------------------------------------------------------------
# Variables (override on the command line)
# ---------------------------------------------------------------------------

# Upstream HA Voice PE repo we diff against.
upstream_repo := "esphome/home-assistant-voice-pe"
upstream_file := "home-assistant-voice.yaml"
# Tag we currently pin in external_components. Use "main" for latest upstream.
upstream_ref := "25.11.0"

# Path to Docker CLI on macOS Docker Desktop (not in default PATH).
docker := "/Applications/Docker.app/Contents/Resources/bin/docker"

# Per-device YAML selector. Default `config` flashes the base
# voice_pe_config.yaml, which produces hostname `voice-<MAC suffix>`
# (matching stock HA Voice PE naming, so this firmware can OTA-replace
# a factory-fresh Voice PE without a USB step). For an additional
# speaker, create a tiny per-device wrapper named
# `voice_pe_<name>.yaml` (template: voice_pe_example.yaml) and call:
#   just flash device=<name>
# Recipes that touch the firmware all honor this variable.
device := "config"

# Serial port for USB flashing. Override per recipe call.
port := "/dev/cu.usbmodem*"

# IP/hostname for OTA flashing when mDNS resolution of `<name>.local`
# fails (guest WiFi, VLANs, multi-AP setups without mDNS forwarding).
# Empty = let ESPHome default to mDNS lookup. Override per recipe:
#   just flash device=voicepe2 address=192.168.1.45
address := ""

# ---------------------------------------------------------------------------
# ESPHome firmware (Voice PE satellite)
# ---------------------------------------------------------------------------

# Validate the YAML without compiling (e.g. `just validate device=voicepe2`).
validate:
    cd home-assistant-voice-pe && poetry run esphome config voice_pe_{{device}}.yaml

# Compile only (no flash, no logs).
compile:
    cd home-assistant-voice-pe && poetry run esphome compile voice_pe_{{device}}.yaml

# Pass address=... if mDNS lookup of `<device>.local` fails on your
# network (`just flash device=voicepe2 address=192.168.1.45`).
# Compile, OTA-flash the device, and stream logs (default dev loop).
flash:
    cd home-assistant-voice-pe && poetry run esphome run \
        {{ if address == "" { "" } else { "--device " + address } }} \
        voice_pe_{{device}}.yaml

# USB-flash via serial. Override port: `just port=/dev/cu.usbserial-1234 flash-usb`.
flash-usb:
    cd home-assistant-voice-pe && poetry run esphome run --device {{port}} voice_pe_{{device}}.yaml

# Stream logs from the already-running device. Pass address=... if mDNS fails.
logs:
    cd home-assistant-voice-pe && poetry run esphome logs \
        {{ if address == "" { "" } else { "--device " + address } }} \
        voice_pe_{{device}}.yaml

# Wipe esphome build cache for the selected device (forces a fresh compile).
clean:
    cd home-assistant-voice-pe && poetry run esphome clean voice_pe_{{device}}.yaml

# Default ref is the version we pin in external_components; override with
# `just upstream_ref=main diff-upstream` to compare against the latest.
# The base file is the apples-to-apples comparison after our split —
# the 33-line voice_pe_config.yaml wrapper just supplies per-device
# identity + encryption key.
# Diff our voice_pe_base.yaml against upstream HA Voice PE YAML.
diff-upstream:
    @echo "Diffing voice_pe_base.yaml vs {{upstream_repo}}@{{upstream_ref}}/{{upstream_file}}"
    @echo
    @curl -fsSL "https://raw.githubusercontent.com/{{upstream_repo}}/{{upstream_ref}}/{{upstream_file}}" \
        | git diff --no-index --color=always -- - home-assistant-voice-pe/voice_pe_base.yaml \
        || true

# ---------------------------------------------------------------------------
# Addon (server side)
# ---------------------------------------------------------------------------

# Tags as `local-test:dev`. Native ~75s on Apple Silicon, cached few seconds.
# Local docker build of the aarch64 addon image (matches HAOS).
addon-build:
    cd openai_realtime_voice_agent && \
        {{docker}} buildx build \
            --platform linux/arm64 \
            --build-arg BUILD_FROM=ghcr.io/home-assistant/aarch64-base-debian:trixie \
            --build-arg BUILD_ARCH=aarch64 \
            -t local-test:dev .

# Smoke-test the locally built image: import core modules.
addon-smoke:
    {{docker}} run --platform linux/arm64 --rm \
        --entrypoint /usr/bin/python3 local-test:dev -c \
        'from app.main import Application; from app.mcp_service import HomeAssistantMCPService; \
         svc = HomeAssistantMCPService("x", ""); \
         print("imports OK; has fetch_assist_prompt_and_snapshot:", \
               hasattr(svc, "fetch_assist_prompt_and_snapshot"))'

# Run the addon's lightweight contract tests.
addon-test:
    cd openai_realtime_voice_agent && poetry run python -m unittest discover -s tests

# Pull + verify the latest published addon image from ghcr.io.
addon-verify-published:
    {{docker}} pull --platform linux/arm64 \
        ghcr.io/brettfire/ha-openai-realtime/openai-realtime-voice-agent-aarch64:latest
    {{docker}} run --platform linux/arm64 --rm \
        --entrypoint /usr/bin/python3 \
        ghcr.io/brettfire/ha-openai-realtime/openai-realtime-voice-agent-aarch64:latest -c \
        'from app.mcp_service import HomeAssistantMCPService; print("published image OK")'
