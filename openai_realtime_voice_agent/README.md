# OpenAI Realtime Voice Agent - Server

Home Assistant addon that provides OpenAI Realtime API integration with WebSocket support for ESP32 devices.

## Installation

### Option 1: Add Repository (Recommended)

1. In Home Assistant, go to **Supervisor** → **Add-on Store**
2. Click the **⋮** menu (top right) → **Repositories**
3. Add this repository: `https://github.com/fjfricke/ha-openai-realtime`
4. Find **OpenAI Realtime Voice Agent** in the addon store and install it

### Option 2: Manual Installation

1. Copy the `openai_realtime_voice_agent/` folder to your Home Assistant `addons/` directory
2. Restart Home Assistant Supervisor
3. Install the addon from **Supervisor** → **Add-on Store** → **Local Add-ons**

## Configuration

Configure the addon in Home Assistant:

1. Go to **Supervisor** → **Add-on Store** → **OpenAI Realtime Voice Agent** → **Configuration**
2. Set the following required option:
   - `openai_api_key`: Your OpenAI API key

3. MCP connection (defaults work out of the box on HAOS):
   - `ha_mcp_url`: defaults to `http://supervisor/core/api/mcp`. The addon's
     `homeassistant_api: true` permission grants access to this supervisor
     proxy route and the `SUPERVISOR_TOKEN` env var is used automatically
     as the bearer token (see [HA addon docs][ha-addon-comm]). Leave
     `longlived_token` blank.
   - To talk to an external Home Assistant instead, set `ha_mcp_url` to that
     URL (e.g. your Nabu Casa hostname) and provide a `longlived_token`.
   - Home Assistant's built-in **Model Context Protocol Server** integration
     must be enabled (Settings → Devices & Services → Add Integration).

[ha-addon-comm]: https://developers.home-assistant.io/docs/add-ons/communication

4. Optional settings:
   - `vad_threshold`: Voice activity detection threshold (0.0-1.0, default: 0.5)
   - `vad_prefix_padding_ms`: Audio padding before detection in milliseconds (default: 300)
   - `vad_silence_duration_ms`: Duration of silence before stopping in milliseconds (default: 500)
   - `instructions`: Custom instructions for the AI assistant (default: "You are the Home Assistant Voice Agent and can control the Smart Home.")
   - `session_reuse_timeout_seconds`: Timeout for session reuse in seconds (default: 300, max: 3600)
   - `enable_recording`: Enable audio recording for debugging (default: false)

4. Start the addon

## Features

- OpenAI Realtime API integration
- WebSocket server for ESP32 devices
- Home Assistant MCP (Model Context Protocol) integration
- Voice activity detection
- Session management with automatic reuse
- Optional audio recording for debugging

## Troubleshooting

- **MCP connection issues**:
  - Confirm the **Model Context Protocol Server** integration is enabled in HA.
  - With the default `ha_mcp_url` (`http://supervisor/core/api/mcp`), leave
    `longlived_token` blank — the supervisor proxy rejects user-issued bearer
    tokens; it requires the auto-provided `SUPERVISOR_TOKEN` instead. If you
    set a long-lived token here it overrides `SUPERVISOR_TOKEN` and the request
    returns 401.
  - To use a long-lived token, point `ha_mcp_url` at a Home Assistant URL that
    doesn't go through the supervisor (e.g. `http://homeassistant.local:8123/api/mcp`
    or a Nabu Casa hostname).
- **WebSocket connection**: Check that the port is accessible and not blocked by firewall
- **Check logs**: View addon logs in Home Assistant Supervisor
