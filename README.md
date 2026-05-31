# OpenAI Realtime Voice Agent

Voice control system for Home Assistant using OpenAI Realtime API with ESP32/ESP32-S3 devices.

## Components

This repository contains two main components:

- **Server** (`openai_realtime_voice_agent/`): Home Assistant addon that provides OpenAI Realtime API integration and WebSocket server for ESP32 devices
- **Client** (`home-assistant-voice-pe/`): ESPHome configuration for ESP32/ESP32-S3 devices with custom WebSocket component

## Features

### Server Features

- **OpenAI Realtime API Integration**: Direct integration with OpenAI's Realtime API for natural language interactions
- **WebSocket Server**: Bidirectional WebSocket connection for ESP32 devices with low latency
- **Home Assistant MCP Integration**: Integration with Model Context Protocol for smart home control
- **Voice Activity Detection (VAD)**: Automatic detection of speech vs. silence for optimal conversation flow
- **Session Management**: Automatic session reuse for better performance and conversation continuity
- **Audio Recording**: Optional audio recording for debugging purposes

### Client Features

- **Voice Assistant**: Real-time voice interaction with OpenAI Realtime API via WebSocket
- **Wake Word Detection**: Multiple wake words supported ("Okay Nabu", "Hey Jarvis", "Hey Mycroft")
- **LED Feedback**: Visual status indicators via 12-LED ring for various states
- **Hardware Controls**: Button controls and hardware mute switch for privacy
- **Auto Gain Control (AGC)**: Hardware-based automatic volume adjustment for consistent audio quality
- **Echo Cancellation (AEC)**: Hardware-based echo suppression prevents feedback

### Conversation Behavior

- **Immediate Response**: After wake word detection, you can speak immediately without waiting
- **Natural Conversation Flow**: During silence, you can continue speaking naturally - the assistant listens continuously
- **Interruption Handling**: User input during assistant responses is ignored, except for wake words which can interrupt
- **Stop Words**: Conversation ends when a stop word is detected (e.g., "thank you", "stop") using a dedicated tool
- **Session Continuity**: Previous conversation history is maintained when a new wake word is spoken within the session reuse timeout period after the last conversation ended
- **Wake Word Restart**: After a conversation ends, a new wake word starts a fresh interaction while preserving context within the timeout window

## Documentation

- **Server Installation**: See [`openai_realtime_voice_agent/README.md`](openai_realtime_voice_agent/README.md)
- **Client Installation**: See [`home-assistant-voice-pe/README.md`](home-assistant-voice-pe/README.md)

## Quick Start

1. **Install the Server Addon**: Follow the [server documentation](openai_realtime_voice_agent/README.md)
2. **Configure ESP32 Device**: Follow the [client documentation](home-assistant-voice-pe/README.md)

## Prerequisites

- **Home Assistant Core 2026.5+ recommended.** The addon uses HA's
  built-in `mcp_server` integration via the supervisor proxy at
  `http://supervisor/core/api/mcp`. With 2026.5+ the addon can also
  pull the live-context snapshot resource
  (`homeassistant://assist/context-snapshot`, added in PR #167396)
  for richer per-turn entity-state context. On older 2026.x the
  snapshot fetch is silently skipped; everything else still works.
- Enable the **Model Context Protocol Server** integration in HA
  (Settings → Devices & Services → Add Integration).
- Expose the entities you want the voice agent to control via
  Settings → Voice assistants → Expose. Fewer, well-targeted
  exposures produce noticeably better recognition than dumping
  hundreds of low-signal entities.

## License

MIT License - see [LICENSE](LICENSE) file for details.
