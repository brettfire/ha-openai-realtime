# Audio pre-roll + cold-start latency reduction — plan

Status: **planning** (not implemented). Captures the observed
problem, the three independent fixes that address it, and a
recommended landing order.

## Problem

Today, between wake-word fire and the satellite's "ready to send
audio" state, mic data is dropped. Users who start speaking
immediately after the wake word ("Hey Nabu, turn on the lights")
lose the leading portion of their command, and the model gets a
truncated phrase that's hard to interpret.

The drop happens in `voice_assistant_websocket.cpp`:

```cpp
void VoiceAssistantWebSocket::on_microphone_data_(const std::vector<uint8_t> &data) {
  // Only process if connected and running
  if (!this->is_connected() || this->state_ != VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    return;  // ← bug: dropped, not buffered
  }
  ...
```

## The gap, in numbers

End-to-end "wake word fires" → "first PCM sample reaches OpenAI":

| Stage | Cold | Warm |
|---|---|---|
| Satellite TCP + TLS + WS upgrade to addon | 100-300ms | 100-300ms |
| Addon: fetch MCP tools, Assist prompt, live-context snapshot from HA | 500-1500ms | (cached, 50-200ms) |
| Addon: open OpenAI WS, send `session.create`, await ack | 200-500ms | (pre-warmed, ~0ms) |
| **Total (today)** | **~1-2 seconds** | **~150ms** |

"Today" = cold path; nothing is cached or pre-warmed.

The cold path is what users actually hit, because most wake-word
events are separated by enough idle time that nothing useful is
cached.

## Three independent fixes

Each one helps, and they compose. Listed in order of
bang-for-buck.

### Fix 1 — Satellite-side pre-roll buffer (RECOMMENDED FIRST)

Ring buffer the most recent ~1500ms of mic audio at all times. On
wake-word fire, flush the buffer over the WebSocket the instant
the send path becomes legal, then continue with real-time
streaming.

This alone covers the worst case even if Fixes 2 and 3 never
land — because the buffer holds *more* audio than the cold-start
gap, the user's first words are always captured.

**Storage.** Buffer post-mono-conversion, pre-resample (the
"stage 2" format inside `on_microphone_data_`): 16kHz, 16-bit,
mono. 1500ms × 16000 × 2 = **48 KB**. Allocate in PSRAM
(`ps_malloc` / `heap_caps_malloc(MALLOC_CAP_SPIRAM)`); Voice PE
has 8MB free. Internal SRAM is precious for micro_wake_word, don't
take from it.

(Buffering post-resample at 24kHz would be 72 KB and bypass one
small CPU step at flush time, but post-mono is the smallest
faithful representation, and resample-at-flush is the same total
CPU work — just shifted.)

**Concurrency.** Mic callback task pushes; main task drains. Use
a FreeRTOS `StreamBuffer` (lock-free, designed for exactly this
pattern) sized to 48KB. No mutex needed.

**Always-on filling.** The buffer fills continuously whenever the
mic is running (which is "always" because micro_wake_word needs
it). Cost: mono-conversion runs even when idle (~1ms per chunk,
negligible).

**Pause during bot speech.** In wake-word-only mode we already
gate mic data while the bot is speaking to prevent echo
self-trigger. The ring buffer should observe the same gate —
otherwise it could capture a snippet of bot audio (leaked through
the mic) and flush that as "user input" on the next wake-word
event. Concretely: `is_bot_speaking() == true` → drop on the
floor instead of buffering.

**Drain trigger.** When state transitions to
`VOICE_ASSISTANT_WEBSOCKET_RUNNING` (in the WS `connected`
event handler), drain the entire pre-roll buffer in one batched
send (or a few large chunks if the buffer's contents would exceed
the addon's `buffer_size = 4096` per frame). Then unblock normal
real-time streaming.

**Including the wake word itself is fine and probably desirable.**
Don't try to strip it — micro_wake_word doesn't tell us exactly
where in the buffer the trigger fired, and the OpenAI Realtime
model handles "Hey Nabu, turn on the kitchen light" with no
semantic harm. Server VAD treats the wake-word phoneme as part of
the speech turn; nothing breaks.

**Edge cases:**

- *Connection takes longer than 1500ms.* Buffer ring-wraps and the
  oldest audio is overwritten. User's command got dropped anyway
  in this case, so we're no worse off — and probably better,
  because we still capture the tail of what they said.
- *User speaks during wake-word detection itself.* Buffer captures
  it. Win.
- *Pre-roll contains silence or background noise.* OpenAI VAD
  handles silence fine; it just sees a longer prefix. No harm.
- *Buffer overflow during draining.* Shouldn't happen — we drain
  on the same task that builds new frames, and draining is fast.
  If it does, push-side drops new audio gracefully (FreeRTOS
  StreamBuffer's documented behavior).

**LOC estimate:** ~80 lines of C++ — a small RingBuffer wrapper,
3-line change in `on_microphone_data_` to push instead of drop,
~15-line drain routine in the WS connected handler.

### Fix 2 — Addon-side MCP cache

Cache the assist prompt and live-context snapshot returned by
`mcp_service.fetch_assist_prompt_and_snapshot()`.

```python
class MCPCache:
    PROMPT_TTL_SECONDS = 300.0   # entities list rarely changes
    SNAPSHOT_TTL_SECONDS = 5.0   # state changes, but rapid re-asks
                                 # benefit and a 5s stale read is fine
                                 # because the model can call
                                 # GetLiveContext when it needs fresh
    ...
```

The model already calls `GetLiveContext` when it needs fresh state
for a specific entity, so the snapshot's primary value is
*priming* — giving the LLM a sense of "what exists" so it can
disambiguate "the kitchen light" without an extra tool call. A 5s
TTL covers the common "user fires two wake words in a row" case
and stays fresh enough that bedside-lamp brightness won't lie.

The prompt (entity names + areas) genuinely changes slowly — 5
minutes is fine, longer is fine.

**Cold-start impact:** turns the 500-1500ms MCP fetch into
~50-200ms on cache hit, ~0ms on subsequent fast calls.

**Risks:** very minor. If the user adds a new device and
immediately tries to control it by voice, they may need to wait
up to 5 minutes for the prompt cache to expire — but the model
can still discover the new entity via `GetLiveContext`. Could
expose a "flush cache" service call if this turns out to be
annoying.

**LOC estimate:** ~40 lines of Python in `mcp_service.py`.

### Fix 3 — Addon-side OpenAI session pre-warming

Keep a small pool of fully-initialized `OpenAIRealtimeLLMService`
instances ready to go (instructions + tools loaded, WS connected,
`session.create` acked). On client connect, take one from the
pool, attach it to the pipeline, refill the pool in the
background.

```python
class SessionPool:
    POOL_SIZE = 1  # for single-satellite; bump when multi lands

    async def acquire(self) -> OpenAIRealtimeLLMService:
        # take an idle, fully-initialized service; refill async
        ...

    async def _create_warm(self) -> OpenAIRealtimeLLMService:
        # build SessionProperties from cache (Fix 2), open WS,
        # await session.created
        ...
```

**Cold-start impact:** eliminates the ~200-500ms OpenAI handshake
from the critical path. Combined with Fix 2, addon-side cold
start drops from 1-2s to ~50ms (just the routing).

**Refresh policy:** OpenAI sessions have a 60-minute hard expiry.
Background task retires sessions at ~55min and replaces them.
Idle sessions also retire after some longer threshold (e.g. 30
min) to keep the cache fresh against config changes.

**Cost:** one extra always-open WS to OpenAI's API. No token
billing while idle (Realtime API bills per audio second sent or
received). Negligible.

**Risks:**

- *Configuration changes (instructions, VAD threshold) require
  flushing the pool.* Otherwise the next user gets a session
  built with the old config. Add a pool-flush hook to the addon's
  config-changed event.
- *MCP context drift.* The warm session was built with whatever
  Assist context was cached at pool-fill time. If that's 30
  minutes old when a user finally connects, the model may have a
  stale entity list. The Fix 2 cache TTLs bound this naturally,
  and we can also force a refill on pool acquisition if the
  cached context is older than N seconds.

**LOC estimate:** ~150 lines of Python — pool management,
refill loop, integration in `_ensure_openai_service`. Higher
because the existing flow is heavily "build fresh per client"
shaped and needs rewiring.

## Recommended landing order

1. **Fix 1 (pre-roll buffer)** alone. Ship it and measure.
   Expectation: "missed the first half" complaints go to zero.
   If they don't, the problem is downstream (e.g. addon dropping
   buffered audio because OpenAI WS isn't open yet — but the
   pre-roll *should* queue in the addon's pipecat input queue
   while session setup completes; worth verifying).

2. **Fix 2 (MCP cache)** when convenient. Small, isolated, no
   user-visible behavior change except faster.

3. **Fix 3 (pre-warm pool)** only if Fixes 1 and 2 aren't enough.
   It's the most invasive change and the smallest marginal win
   once Fix 1 is in (pre-roll buffer already covers the cold-
   start gap; pre-warming just makes the gap smaller for cases
   where the user speaks *before* the pre-roll buffer holds
   enough). Don't pre-emptively complicate the addon for this.

## What this does NOT fix

- **Wake-word detector false negatives** (model fails to detect a
  legitimate "Hey Nabu"). Different problem.
- **First-token TTS latency on the response** (OpenAI takes time
  to think). Different problem, requires model-side or
  prompt-side mitigations.
- **Audio quality issues** (echo, AEC tuning). Different problem.

## Open questions

- [ ] Pre-roll buffer length: 1500ms is a guess based on the
  observed cold-start range. Worth measuring real cold-start
  times against the current addon and tuning the buffer to
  comfortably exceed P99. Probably 1000ms is enough; 2000ms is
  safer; 3000ms wastes PSRAM.
- [ ] Should the pre-roll drain be paced (e.g. ~5x real-time) or
  fired as a single batched send? Single batched is simplest and
  pipecat's input queue should handle it; paced would be needed
  only if we see backpressure issues.
- [ ] Should we add a debug counter / sensor exposing
  "pre-roll bytes drained on last wake" so the user can verify
  the fix is working in the HA UI?

## References

- Current drop site: `home-assistant-voice-pe/esphome/components/voice_assistant_websocket/voice_assistant_websocket.cpp:504-507`
- Addon session creation (the heavy work): `openai_realtime_voice_agent/app/main.py:143-296` (`_ensure_openai_service`)
- MCP fetch (the slowest sync part of session creation): `openai_realtime_voice_agent/app/main.py:223-261` (calls `mcp_service.fetch_assist_prompt_and_snapshot`)
- FreeRTOS StreamBuffer docs: https://www.freertos.org/RTOS-stream-buffer-example.html
