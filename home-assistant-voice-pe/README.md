# OpenAI Realtime Voice Agent - Client

ESPHome configuration for ESP32/ESP32-S3 devices to connect to the OpenAI Realtime Voice Agent server.
It is mainly based on https://github.com/esphome/home-assistant-voice-pe.

## Prerequisites

- ESPHome 2025.11.0 or higher
- Voice PE Hardware (Home Assistant Voice Pod Edition) or compatible ESP32-S3 device
- Home Assistant with the OpenAI Realtime Addon installed and running
- Python 3.11+ with Poetry

## Installation

### 1. Install Dependencies

```bash
cd home-assistant-voice-pe
poetry install
```

### 2. Configure Secrets

Copy `secrets.yaml.example` to `secrets.yaml` and fill in your values:

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml`:
- `wifi_ssid` / `wifi_password`: WiFi network (shared across devices)
- `api_encryption_key`: HA API encryption key for the default
  speaker (`ha-voice-openai`). Per-device entries (e.g.
  `api_encryption_key_voicepe2`) are added when you onboard
  additional speakers — see [Multi-device setup](#multi-device-setup).
- `ota_password`: Password for OTA updates (shared)
- `server_url`: WebSocket URL for the OpenAI Realtime addon
  (e.g., `ws://homeassistant.local:10245`)

### 3. Compile and Flash

The repo ships with a `justfile` wrapping the common ESPHome
commands. Run `just` (no args) from the repo root to see all
recipes.

```bash
just validate                   # validate the YAML
just compile                    # compile only
just flash                      # compile + OTA-flash + stream logs
just port=/dev/cu.usbmodem101 flash-usb   # USB flash
just logs                       # stream logs from a running device
just clean                      # wipe build cache
```

Or run `poetry run esphome ...` directly if you prefer.

## Multi-device setup

For a second speaker, create a tiny per-device wrapper next to
`voice_pe_config.yaml` (template: `voice_pe_example.yaml`):

```yaml
# voice_pe_voicepe2.yaml
packages:
  base: !include voice_pe_config.yaml

substitutions:
  device_name: voicepe2

api:
  encryption:
    key: !secret api_encryption_key_voicepe2
```

Add the per-device key to `secrets.yaml`:

```yaml
api_encryption_key_voicepe2: "<unique base64 32 bytes>"
# Generate with: openssl rand -base64 32
```

Flash:

```bash
just flash device=voicepe2
just logs device=voicepe2
```

If instead you want to onboard the device via HA's ESPHome Device
Builder addon (no local repo needed), use the **git package**
flavor — `voice_pe_example.yaml` shows both. Pasted into the HA
ESPHome dashboard with the per-device key added to
`/config/esphome/secrets.yaml`, ESPHome fetches the base + custom
component from this repo at compile time.

## Configuration

The main configuration file is `voice_pe_config.yaml`. Key settings:

- Device name: Change `esphome.name` if desired
- Wake words: Configured wake words ("Okay Nabu", "Hey Jarvis", "Hey Mycroft")
- Audio settings: Microphone and speaker configuration
- LED ring: Visual feedback for device states

## Features

- **Voice Assistant**: Real-time voice interaction with OpenAI Realtime API
- **Wake Word Detection**: Multiple wake words supported
- **LED Feedback**: Visual status indicators
- **Hardware Controls**: Button controls and mute switch
- **Auto Gain Control**: Hardware-based AGC for consistent audio levels
- **Echo Cancellation**: Hardware-based AEC prevents feedback

## Troubleshooting

### Connection Issues

- **Device doesn't connect**: Check `server_url` in `secrets.yaml` matches your addon configuration
- **No audio**: Check hardware mute switch and verify microphone initialization in logs
- **View logs**: `poetry run esphome logs voice_pe_config.yaml`

