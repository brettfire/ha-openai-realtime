# Multi-satellite support — plan

Status: **planning** (not implemented). This document captures the
target architecture, decisions, and unknowns so future work has a
single place to anchor against. Update it as we discover more.

## Goal

Allow N Voice PE satellites to share one addon instance and hold
concurrent or overlapping conversations, with sane arbitration when
multiple satellites detect the same wake word simultaneously.

Today: one addon = one active WebSocket = one active OpenAI session.
A second satellite connecting drops the first (`"Only one client
allowed, using new connection"`). For 1-3 satellites that rarely
talk at the same time, this is workable but ugly. For 5+ rooms it's
not.

## Constraints from the existing code

The two pieces that lock us to single-client today:

1. **`pipecat.transports.websocket.server.WebsocketServerTransport`**
   stores exactly one `self._websocket`. On a new connection it
   closes the old. The `_client_handler` loop is also bound to that
   single slot.
2. **The pipeline is built once at addon startup** in `Application.run()`
   with that single transport, a single `OpenAIRealtimeLLMService`,
   and a single `PipelineRunner`. Even if pipecat let multiple
   websockets share the transport, the OpenAI session and pipeline
   processors are shared too — audio from any satellite would mix.

So multi-satellite isn't just "remove the `if` that drops the old
client." It's a per-connection lifecycle.

## Target architecture

Single addon, many satellites. Each satellite gets its own private
pipeline and OpenAI session.

```
                                       ┌─ pipeline A (sat A's OpenAI session)
                                       │
WebsocketAcceptor ──► dispatch ────────┼─ pipeline B (sat B's OpenAI session)
(websockets.serve)    by client_id     │
                                       └─ pipeline C (sat C's OpenAI session)

WakeWordArbiter — separate, peeks at incoming wake-word events
                  to decide which satellite "wins" before
                  any pipeline opens an OpenAI session
```

Concretely:

1. **Multi-client WebSocket accept loop.** Replace the use of
   `WebsocketServerTransport` (or wrap it with our own
   `websockets.serve()`-based handler) so we accept arbitrary
   connections without dropping older ones.
2. **Per-connection transport pair.** Subclass pipecat's
   `BaseInputTransport` / `BaseOutputTransport` so each WebSocket
   has its own audio path. Each pair is bound to a single
   `WebSocketServerProtocol` and pushes/consumes frames against it.
3. **Per-connection pipeline factory.** For each new connection,
   build a fresh pipeline: input transport → session activity
   tracker → context aggregator → `OpenAIRealtimeLLMService` →
   output transport. Spawn its `PipelineRunner` as its own asyncio
   task.
4. **Per-connection lifecycle.** Track `{client_id: ClientSession}`
   in an `Application.clients` dict. On disconnect: cancel the
   pipeline task, `_shutdown_service_safely(session.openai_service)`
   (the existing per-instance helper works fine), remove from dict.
   Context caching via `session_manager` already keyed by
   `client_id` — keep using it.
5. **Wake-word arbiter** (see next section).

## Wake-word arbitration ("two satellites in earshot")

When two satellites in earshot detect the same wake word, both will
fire `voice_assistant_websocket.start` and open WebSockets to the
addon. Without arbitration we'd start two concurrent OpenAI sessions
and the bot would respond on both speakers simultaneously — terrible.

### How HA's native voice solution handles it (verified)

HA's `assist_pipeline` integration has a **`DuplicateWakeUpDetectedError`**
specifically for this case. The algorithm is simpler than I expected:

- Global `hass.data[DATA_LAST_WAKE_UP]: dict[str, float]` keyed by
  **wake-word phrase** (the literal string, e.g. `"okay_nabu"`).
- On a wake-word detection, the pipeline checks
  `last_wake_up[phrase]`. If a previous detection of the same
  phrase happened within `WAKE_WORD_COOLDOWN = 2` seconds,
  `DuplicateWakeUpDetectedError` is raised — the second pipeline
  bails and the satellite gets the error code.
- Otherwise the timestamp is recorded and the pipeline proceeds.

So it's **first-come, first-served with a 2-second per-phrase
cooldown**. Not audio-level voting, not "closest wins" — just
"first satellite to send the event wins, and any other satellite
sending the same wake-word phrase within 2s gets rejected."

ESPHome's built-in `voice_assistant` component handles the rejection
on the satellite side: it treats `wake_word_detection_aborted`,
`wake-word-timeout`, and `no_wake_word` as a benign "false trigger"
and returns the satellite to idle. So the losing satellite's LED
flashes briefly and stops.

References (HA Core, master branch):
- `homeassistant/components/assist_pipeline/error.py` —
  `DuplicateWakeUpDetectedError(WakeWordDetectionError)`
- `homeassistant/components/assist_pipeline/pipeline.py` —
  the `last_wake_up = self.hass.data[DATA_LAST_WAKE_UP].get(...)`
  block raises the error
- `homeassistant/components/assist_pipeline/const.py` —
  `WAKE_WORD_COOLDOWN = 2` (seconds)
- `esphome/components/voice_assistant/voice_assistant.cpp` —
  the satellite-side handler:
  `if (code == "wake-word-timeout" || code == "wake_word_detection_aborted" || code == "no_wake_word")`

### Why it works well enough

In practice "first to detect" correlates strongly with "closest to
user" because:
- Closer satellite hears louder audio → wake-word detector trips
  with less hysteresis → faster confidence threshold cross.
- Network latency from satellite to HA is roughly equal across the
  LAN, so the detection-time ordering survives the trip.

It can degenerate when two satellites are equidistant and one
happens to detect a frame earlier than the other — but that's a
coin flip that most users won't notice. The 2-second cooldown is
long enough that a true second wake word (user trying again because
the first time didn't work) doesn't get blocked.

### What we'd build

Same algorithm, scoped to our addon. The state is simpler than HA's
because we have one phrase per device today (no multi-pipeline
routing concerns).

```python
# in app/wake_word_arbiter.py (new)
class WakeWordArbiter:
    """First-come, first-served arbiter with a per-phrase cooldown.

    Mirrors home-assistant/assist_pipeline's
    DuplicateWakeUpDetectedError behavior (2-second cooldown).
    """
    COOLDOWN_SECONDS = 2.0

    def __init__(self):
        # phrase -> monotonic timestamp of last winning detection
        self._last_wake_up: dict[str, float] = {}

    def claim(self, phrase: str) -> bool:
        """True if this detection wins; False if it's a duplicate."""
        now = time.monotonic()
        last = self._last_wake_up.get(phrase)
        if last is not None and (now - last) < self.COOLDOWN_SECONDS:
            return False
        self._last_wake_up[phrase] = now
        return True
```

Wire-up:

1. Firmware sends a tiny pre-pipeline frame on wake word:
   ```json
   {"type":"wake_word_detected","phrase":"okay_nabu"}
   ```
   `phrase` is the wake-word name from micro_wake_word's model.
2. Addon's `RawAudioSerializer` (or a parallel handler) calls
   `arbiter.claim(phrase)`.
3. If `claim` returns True → addon emits `{"type":"proceed"}`
   back, and the per-client pipeline gets built / audio streams.
4. If False → addon emits `{"type":"yield"}` back, and the
   per-client pipeline never gets created. No OpenAI session
   opened.
5. Satellite on `yield`: stop listening, return to idle, brief LED
   flash. Wake-word detector keeps running for next turn.

Per-phrase keying means if you ever set up two satellites with
different wake words ("Hey Jarvis" in office, "Okay Nabu"
elsewhere), they don't conflict — both can fire concurrently for
different conversations.

### Why we'd skip the audio-level approach

My original plan (level voting) is more code, more state, and
likely no real-world quality improvement over the timestamp method
HA uses. Drop it.

### Open question: cooldown value

HA uses 2 seconds. We might want to tune for our usage pattern:
- Too short (<1s): the slower satellite occasionally wins because
  some network/scheduling jitter beat the faster one
- Too long (>3s): a user retrying after a missed wake word gets
  blocked, frustrating

2 seconds is probably fine — keep HA's value unless we observe
issues.

## Phasing

A "ship intermediate value" sequence rather than one big PR:

1. **Phase 1 — Per-connection plumbing (no arbitration).**
   Refactor to per-connection pipelines and a multi-client accept
   loop. Concurrent satellites now WORK — they just both respond
   if both fire. Test with 2 devices in different rooms (out of
   earshot) where the only-one-fires-at-a-time invariant holds
   naturally. Establishes the foundation.
2. **Phase 2 — Wake-word arbiter.** Add the 250ms arbiter +
   `proceed`/`yield` control messages. Test with 2 devices in
   earshot.
3. **Phase 3 — Resource caps.** Add a max-concurrent-sessions
   limit so a runaway condition (every satellite firing every
   minute) doesn't melt the OpenAI bill. Possibly per-satellite
   rate limits too.
4. **Phase 4 — Observability.** Per-client metrics (active
   sessions, total tokens this hour, average response latency).
   HA sensors so the user can see it.

## Estimated scope (for Phase 1)

| Piece | LOC | Risk |
|---|---|---|
| Multi-client accept loop replacing/wrapping `WebsocketServerTransport` | ~50 | Low |
| Per-connection `BaseInputTransport` / `BaseOutputTransport` subclasses | ~150 | Medium — pipecat 0.0.97 internals haven't been deeply tested for this |
| Pipeline factory + lifecycle in `main.py` | ~80 | Low |
| Per-client context caching wired through `session_manager` | ~20 | Low (already keyed by client_id) |
| End-to-end test with 2+ satellites | — | Need 2 reflashed devices |

Phase 2 arbiter (revised after HA research): ~40 LOC of Python
(the `WakeWordArbiter` class above + a serializer hook) + ~30 LOC
of C++ on the satellite (emit the `wake_word_detected` JSON before
opening the conversation, handle `yield` by stopping locally). Much
smaller than originally estimated because we copy HA's
2-second-cooldown algorithm instead of inventing a level-voting
scheme.

Total to fully multi-satellite: probably a focused weekend.

## Decisions to make before starting

- [ ] **One OpenAI API key shared across all clients, or per-client?**
  Shared is fine cost-wise but means all conversations are billed
  under one usage line. Per-client lets you set per-room limits.
- [ ] **Same instructions for all satellites or per-room?**
  Some users want "in the bedroom, default volume is lower" type
  behavior. Could pass a `room` field in the wake-word event and
  template instructions per-room.
- [x] **Wake-word arbiter implementation**: ~~own logic vs.
  delegate to HA's existing satellite coordinator~~ Resolved:
  reimplement HA's algorithm (`WakeWordArbiter` above) — copying
  is cheaper than wiring into HA's pipeline since we already
  bypass that pipeline.
- [x] ~~**Failure mode for arbiter timeout**~~ N/A — we don't have
  a window; HA's algorithm is stateless per-event.

## Non-goals (for now)

- Cross-satellite handoff mid-conversation ("follow me to the
  kitchen"). Real but complicated; later.
- Distributed addon (sharing state across multiple HAOS instances).
  One addon process, multiple satellites — not multiple addons.
- Streaming the same response to multiple speakers simultaneously
  (e.g. whole-house TTS). Different feature.

## References

- pipecat `WebsocketServerTransport`:
  `/usr/lib/python3.12/site-packages/pipecat/transports/websocket/server.py`
  in any built addon image. The `_client_handler` is the single-client
  bottleneck.
- The "Only one client allowed" log message is the canonical sign
  you're hitting it.
- Our existing per-client context cache: `app/session_manager.py`
  uses `client_id` as the key, so most of the per-client state
  shape is already correct.
