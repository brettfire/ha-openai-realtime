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

### How HA's native voice solution handles it

HA Voice / Assist has at least some support for this (need to
verify — open question):

- Wyoming satellites can report **wake-word audio level** as part
  of the wake-word event.
- HA's pipeline coordinator can in principle use that to pick the
  closest satellite and tell the others to ignore. ESPHome's
  built-in voice_assistant component has hooks for this.
- The official HA Voice PE config sets up some of this plumbing
  but the exact arbitration logic / time window isn't documented
  prominently.
- Our custom firmware bypasses HA's voice pipeline entirely (we
  go straight to a WebSocket to our addon), so whatever HA does
  natively doesn't apply to us.

### What we need to build

A small arbiter in the addon, sitting in front of pipeline creation:

1. **Satellite reports wake-word event** before opening the
   conversation pipeline. New JSON control frame:
   ```json
   {"type":"wake_word_detected","level":-12,"ts":1234567890}
   ```
   `level` is the wake-word detector's confidence or peak audio
   level in dBFS. `ts` is the satellite's local millis at
   detection (for ordering ties).
2. **Arbiter collects events for a short window** (~250ms — long
   enough that satellites in earshot of the same speech all
   report, short enough to feel snappy). Keyed by something
   identifying the conversation, probably just "any wake-word
   event in the last 250ms" since multiple people talking
   simultaneously to different satellites is rare.
3. **Pick the winner** — highest `level`, tiebreak by earliest
   `ts`.
4. **Winner gets `{"type":"proceed"}`** — opens the audio stream
   for OpenAI as usual.
5. **Losers get `{"type":"yield"}`** — cancel locally, return to
   idle (their LEDs stop pulsing, no playback). The wake-word
   detector keeps running so they can win the next turn.

### Open question: do we need explicit arbitration?

Maybe not, if we use OpenAI's own server-side VAD as the arbiter:
- Both satellites open WS + OpenAI session
- Both start streaming mic
- Only one will produce coherent speech (the closest one); the
  other gets fragmented / quiet audio
- OpenAI VAD only fires "user started speaking" on the coherent
  stream — the other session gets nothing useful
- Bot responds via the satellite that got the actual speech

This would cost N × audio-input tokens for N satellites that
heard the wake word, but avoid the engineering of an arbiter.
Worth measuring once we have multi-client support — if the
per-second token cost during the listening window is trivial,
laziness might win.

The downside: even with VAD silence on the losing satellites,
**the bot would still respond on all of them** unless we
separately arbitrate the response. So we probably need at least
"winner of the speech goes to one satellite" logic. Which is
basically arbitration anyway.

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

Phase 2 arbiter: another ~150 LOC + a small firmware change to
emit `wake_word_detected` JSON before opening the conversation.

Total to fully multi-satellite: probably a focused weekend.

## Decisions to make before starting

- [ ] **One OpenAI API key shared across all clients, or per-client?**
  Shared is fine cost-wise but means all conversations are billed
  under one usage line. Per-client lets you set per-room limits.
- [ ] **Same instructions for all satellites or per-room?**
  Some users want "in the bedroom, default volume is lower" type
  behavior. Could pass a `room` field in the wake-word event and
  template instructions per-room.
- [ ] **Wake-word arbiter implementation**: own logic vs. delegate
  to HA's existing satellite coordinator (if usable from outside
  HA's native voice pipeline).
- [ ] **Failure mode for arbiter timeout**: if the arbiter window
  expires with no clear winner (close levels, tie ts), do we let
  both proceed or pick arbitrarily? Probably "earliest wins" with
  a warning log.

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
