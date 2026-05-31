#include "voice_assistant_websocket.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include <cstring>
#include <algorithm>

#ifdef USE_ESP_IDF
#include "esp_system.h"
#endif

static const char *TAG = "voice_assistant_websocket";

namespace esphome {
namespace voice_assistant_websocket {

#ifdef USE_ESP_IDF
// Tiny RAII helper so every audio_queue_ touch goes through the mutex.
class AudioQueueLock {
 public:
  AudioQueueLock(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY) : mutex_(mutex) {
    locked_ = (mutex_ != nullptr) && (xSemaphoreTake(mutex_, timeout) == pdTRUE);
  }
  ~AudioQueueLock() {
    if (locked_) {
      xSemaphoreGive(mutex_);
    }
  }
  bool acquired() const { return locked_; }
  // Non-copyable / non-movable.
  AudioQueueLock(const AudioQueueLock &) = delete;
  AudioQueueLock &operator=(const AudioQueueLock &) = delete;
 private:
  SemaphoreHandle_t mutex_;
  bool locked_;
};
#endif

void VoiceAssistantWebSocket::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Assistant WebSocket...");
  this->mono_buffer_.reserve(INPUT_BUFFER_SIZE / 2);   // 32-bit stereo -> 16-bit mono
  this->resampled_buffer_.reserve(INPUT_BUFFER_SIZE * 3 / 2);  // 1.5x upsampling for 16kHz -> 24kHz
#ifdef USE_ESP_IDF
  this->audio_queue_mutex_ = xSemaphoreCreateMutex();
  if (this->audio_queue_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio_queue_mutex_");
  }
#endif
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;

  // Register microphone data callback
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data);
    });
  }
}

void VoiceAssistantWebSocket::loop() {
  // Handle pending disconnect (must be done in main task, not websocket task).
  // After cleanup we keep reconnect_pending_ alone — if a network event set
  // it, the reconnect path below will fire on the next loop iteration.
  // (The old code reset it to false here, which was the reconnect-deadlock
  // bug — events would set reconnect_pending_, then our own disconnect path
  // would immediately clear it, and no reconnect would ever happen.)
  if (this->pending_disconnect_) {
    this->pending_disconnect_ = false;
    this->disconnect_websocket_();

    this->state_ = VOICE_ASSISTANT_WEBSOCKET_IDLE;
    if (!this->reconnect_pending_) {
      // Pure stop (no reconnect intent) — reset attempt counter.
      this->reconnect_attempts_ = 0;
    }

    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }

    // Only treat as a "stopped" event if there's no reconnect coming.
    // Otherwise consumers (LEDs etc.) would flash IDLE in between.
    if (!this->reconnect_pending_) {
      this->stopped_trigger_.trigger();
      ESP_LOGI(TAG, "Voice Assistant WebSocket stopped");
    } else {
      ESP_LOGI(TAG, "Voice Assistant WebSocket cleaned up, reconnect queued");
    }
    return;  // Skip other loop operations after disconnect
  }

  // Drain queued audio if the speaker is running. The actual play()
  // call happens OUTSIDE the mutex to keep the critical section short.
#ifdef USE_ESP_IDF
  if (this->speaker_ != nullptr && this->speaker_->is_running()) {
    std::vector<uint8_t> chunk;
    {
      AudioQueueLock lock(this->audio_queue_mutex_);
      if (lock.acquired() && !this->audio_queue_.empty()) {
        chunk = std::move(this->audio_queue_.front());
        this->audio_queue_.pop_front();
      }
    }
    if (!chunk.empty()) {
      size_t written = this->speaker_->play(chunk.data(), chunk.size());
      if (written == chunk.size()) {
        ESP_LOGD(TAG, "Sent queued audio chunk from loop (%zu bytes)", chunk.size());
      } else if (written > 0 && written < chunk.size()) {
        // Push unsent remainder to the FRONT to preserve PCM order —
        // newer chunks (added by the websocket task) must NOT play
        // before this remainder. std::deque::push_front does what
        // std::queue::push couldn't.
        std::vector<uint8_t> remainder(chunk.begin() + written, chunk.end());
        AudioQueueLock lock(this->audio_queue_mutex_);
        if (lock.acquired()) {
          this->audio_queue_.push_front(std::move(remainder));
        }
        ESP_LOGD(TAG, "Partial play (%zu/%zu), remainder re-queued at front", written, chunk.size());
      } else if (written == 0) {
        // Speaker buffer is full again. Put the whole chunk back at
        // the front so we re-attempt on the next loop iteration.
        AudioQueueLock lock(this->audio_queue_mutex_);
        if (lock.acquired()) {
          this->audio_queue_.push_front(std::move(chunk));
        }
      }
    }
  }
#endif

  // Handle pending start request
  if (this->pending_start_ && this->state_ == VOICE_ASSISTANT_WEBSOCKET_IDLE) {
    this->pending_start_ = false;
    this->start();
  }

  // Handle reconnection (only if not pending disconnect and websocket client is cleaned up)
  if (this->reconnect_pending_ &&
      !this->pending_disconnect_ &&
      this->websocket_client_ == nullptr &&
      (millis() - this->last_reconnect_attempt_) > RECONNECT_DELAY_MS &&
      this->reconnect_attempts_ < MAX_RECONNECT_ATTEMPTS) {
    this->reconnect_pending_ = false;
    this->last_reconnect_attempt_ = millis();
    this->reconnect_attempts_++;
    ESP_LOGW(TAG, "Attempting to reconnect (attempt %u/%u)...", this->reconnect_attempts_, MAX_RECONNECT_ATTEMPTS);
    this->connect_websocket_();
  }

  // Auto-stop: stop the session after AUTO_STOP_INACTIVITY_MS (20s) of
  // no speaker audio. last_speaker_audio_time_ is seeded in start() so
  // a session that opens but never produces audio still times out
  // (previously a 0 value made this branch a no-op).
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    uint32_t last_audio = this->last_speaker_audio_time_.load(std::memory_order_relaxed);
    if (last_audio != 0) {
      uint32_t elapsed = millis() - last_audio;
      if (elapsed > AUTO_STOP_INACTIVITY_MS) {
        ESP_LOGI(TAG, "Auto-stopping: %u ms with no speaker audio (threshold %u ms)",
                 elapsed, AUTO_STOP_INACTIVITY_MS);
        this->stop();
      }
    }
  }
}

void VoiceAssistantWebSocket::dump_config() {
  ESP_LOGCONFIG(TAG, "Voice Assistant WebSocket:");
  ESP_LOGCONFIG(TAG, "  Server URL: %s", this->server_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Microphone Sample Rate: %u Hz", MICROPHONE_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Input Sample Rate (after resampling): %u Hz", INPUT_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Output Sample Rate: %u Hz", OUTPUT_SAMPLE_RATE);
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->microphone_ ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "Yes" : "No");
  ESP_LOGCONFIG(TAG, "  Max Queue Size: %zu chunks", MAX_QUEUE_SIZE);
}

void VoiceAssistantWebSocket::start() {
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    ESP_LOGW(TAG, "Already running");
    return;
  }
  
  ESP_LOGI(TAG, "Starting Voice Assistant WebSocket...");
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_STARTING;

  // Seed last_speaker_audio_time_ to "now" rather than 0. The auto-stop
  // check in loop() ignores the 0 sentinel, so a session that never
  // produces speaker audio would otherwise run forever — fixes the
  // cold-start-with-no-response edge case caught in code review.
  this->last_speaker_audio_time_.store(millis(), std::memory_order_relaxed);

  // Reset explicit disconnect flag for new session
  this->explicit_disconnect_ = false;

  // Reset interrupt time
  this->interrupt_time_.store(0, std::memory_order_relaxed);
  
  // Start microphone first (if not already running)
  // Note: micro_wake_word also uses this microphone, so it might already be running
  if (this->microphone_ != nullptr) {
    if (this->microphone_->is_stopped()) {
      this->microphone_->start();
    } else {
      ESP_LOGD(TAG, "Microphone already running (likely used by micro_wake_word)");
    }
  }
  
  // Start speaker - the resampler will handle format conversion
  if (this->speaker_ != nullptr) {
    // IMPORTANT: Set audio stream info BEFORE starting the speaker!
    // The resampler uses audio_stream_info_ to determine the input sample rate.
    // OpenAI sends 24kHz, 16-bit, mono audio - let the resampler convert to 48kHz
    audio::AudioStreamInfo input_stream_info(16, 1, 24000);  // 16-bit, mono, 24kHz (OpenAI output)
    this->speaker_->set_audio_stream_info(input_stream_info);
    
    // Only start speaker if it's not already running
    // For streaming audio, we want continuous playback without restarting
    if (this->speaker_->is_stopped()) {
      this->speaker_->start();
    }
  }
  
  if (this->state_callback_) {
    this->state_callback_(this->state_);
  }
  
  this->connect_websocket_();
}

void VoiceAssistantWebSocket::stop() {
  if (this->state_ == VOICE_ASSISTANT_WEBSOCKET_IDLE) {
    return;
  }
  
  ESP_LOGI(TAG, "Stopping Voice Assistant WebSocket...");
  this->state_ = VOICE_ASSISTANT_WEBSOCKET_STOPPING;
  
  // Don't stop microphone - micro_wake_word needs it to continue running
  // The microphone can be shared between multiple components in ESPHome
  // micro_wake_word will continue to work even when voice_assistant_websocket is stopped
  ESP_LOGD(TAG, "Keeping microphone running for micro_wake_word");
  // Stop speaker if it's running
  if (this->speaker_ != nullptr) {
    this->speaker_->stop();
  }

  // Clear audio queue under the mutex (websocket task may still be
  // pushing for another millisecond or two while stop() runs).
#ifdef USE_ESP_IDF
  {
    AudioQueueLock lock(this->audio_queue_mutex_);
    if (lock.acquired()) {
      this->audio_queue_.clear();
    }
  }
#endif
  
  if (this->state_callback_) {
    this->state_callback_(this->state_);
  }
  
  // IMPORTANT: Cannot call disconnect_websocket_() from websocket task/event handler
  // Set flag to disconnect in loop() instead (which runs in main task)
  this->pending_disconnect_ = true;
  
  // Note: Rest of cleanup (buffers, state, triggers) will be done in loop() after disconnect
}

void VoiceAssistantWebSocket::request_start() {
  this->pending_start_ = true;
}

void VoiceAssistantWebSocket::connect_websocket_() {
  if (this->websocket_client_ != nullptr) {
    ESP_LOGW(TAG, "WebSocket client already exists, cleaning up...");
    // Use pending_disconnect_ instead of direct call to avoid blocking
    // Set reconnect_pending_ so we retry after disconnect completes
    this->pending_disconnect_ = true;
    this->reconnect_pending_ = true;
    this->last_reconnect_attempt_ = millis();  // Reset timer so we retry after disconnect
    return;  // Exit early, will retry connection after disconnect completes in loop()
  }
  
  if (this->server_url_.empty()) {
    ESP_LOGE(TAG, "Server URL not set!");
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
    return;
  }
  
  ESP_LOGI(TAG, "Connecting to WebSocket server: %s", this->server_url_.c_str());
  
  esp_websocket_client_config_t websocket_cfg = {};
  websocket_cfg.uri = this->server_url_.c_str();
  websocket_cfg.user_context = this;
  websocket_cfg.buffer_size = 4096;
  websocket_cfg.task_prio = 5;
  websocket_cfg.task_stack = 8192;
  websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;  // Use TCP (not SSL) for ws://
  websocket_cfg.network_timeout_ms = 30000;  // 30 second timeout for network operations
  websocket_cfg.reconnect_timeout_ms = 10000;  // 10 second reconnect timeout
  websocket_cfg.ping_interval_sec = 20;  // Send ping every 20 seconds (matches server)
  websocket_cfg.pingpong_timeout_sec = 10;  // 10 second timeout for pong (matches server)
  
  this->websocket_client_ = esp_websocket_client_init(&websocket_cfg);
  if (this->websocket_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
    return;
  }
  
  // Register event handler
  esp_websocket_register_events(this->websocket_client_, 
                                 (esp_websocket_event_id_t) WEBSOCKET_EVENT_ANY,
                                 websocket_event_handler_,
                                 this);
  
  // Start connection
  esp_err_t err = esp_websocket_client_start(this->websocket_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
    this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
    if (this->state_callback_) {
      this->state_callback_(this->state_);
    }
  }
}

void VoiceAssistantWebSocket::disconnect_websocket_() {
  if (this->websocket_client_ != nullptr) {
    ESP_LOGI(TAG, "Disconnecting WebSocket...");
    
    // Check if client is actually connected before trying graceful close
    bool is_connected = esp_websocket_client_is_connected(this->websocket_client_);
    
    if (is_connected) {
      // Try graceful close first (sends close frame)
      // Use shorter timeout (1 second) to avoid blocking too long
      esp_err_t close_err = esp_websocket_client_close(this->websocket_client_, pdMS_TO_TICKS(1000));
      if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "Graceful close failed (%s), forcing stop", esp_err_to_name(close_err));
        // Fallback to immediate stop if graceful close fails
        esp_websocket_client_stop(this->websocket_client_);
      }
    } else {
      // Client not connected, just stop and destroy immediately
      ESP_LOGD(TAG, "Client not connected, stopping immediately");
      esp_websocket_client_stop(this->websocket_client_);
    }
    
    // Always destroy the client to free resources
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
  }
}

void VoiceAssistantWebSocket::send_audio_chunk_(const uint8_t *data, size_t len) {
  // Snapshot the client pointer so an interleaved
  // disconnect_websocket_() can't null it between our check and the
  // send call. Worst case we send to a stale handle and ESP-IDF
  // returns an error — much better than a NULL deref.
  esp_websocket_client_handle_t client = this->websocket_client_;
  if (client == nullptr || !esp_websocket_client_is_connected(client)) {
    return;
  }
  // Bounded timeout (was portMAX_DELAY). The mic callback path
  // calls this for every audio chunk; a network stall used to block
  // the callback indefinitely, starving the wake-word detector and
  // tripping the task watchdog. AUDIO_SEND_TIMEOUT_MS = 100ms is
  // long enough to absorb normal jitter but short enough that we
  // drop a chunk rather than hang.
  int sent = esp_websocket_client_send_bin(client, (const char *) data, len,
                                            pdMS_TO_TICKS(AUDIO_SEND_TIMEOUT_MS));
  if (sent < 0) {
    ESP_LOGW(TAG, "send_bin timed out or failed (dropping %zu byte chunk)", len);
  }
}

void VoiceAssistantWebSocket::process_received_audio_(const uint8_t *data, size_t len) {
  if (this->speaker_ == nullptr) {
    ESP_LOGW(TAG, "Speaker is null, cannot play audio");
    return;
  }

  if (this->state_ != VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    ESP_LOGD(TAG, "Skipping audio playback - voice assistant not in running state");
    return;
  }

  // Ignore audio for a short period after an interrupt to give the
  // server time to stop sending. Atomic load + compare-exchange so a
  // concurrent interrupt() can't tear the value.
  uint32_t interrupt_t = this->interrupt_time_.load(std::memory_order_relaxed);
  if (interrupt_t > 0) {
    uint32_t elapsed = millis() - interrupt_t;
    if (elapsed < INTERRUPT_IGNORE_AUDIO_MS) {
      ESP_LOGD(TAG, "Ignoring audio after interrupt (%u ms remaining)",
               INTERRUPT_IGNORE_AUDIO_MS - elapsed);
      return;
    }
    // Window expired — clear the sentinel if it's still ours.
    this->interrupt_time_.compare_exchange_strong(interrupt_t, 0u, std::memory_order_relaxed);
    ESP_LOGI(TAG, "Resuming audio processing after interrupt");
  }

  // OpenAI sends 24kHz, 16-bit, mono PCM. The resampler (set up in
  // start()) handles 24kHz -> 48kHz + mono->stereo for I2S.
  if (this->speaker_->is_stopped()) {
    ESP_LOGD(TAG, "Speaker is stopped, starting it");
    this->speaker_->start();
  }

  // Drain queued audio first. Pull one chunk at a time from the front
  // under the mutex; play OUTSIDE the mutex to keep the critical
  // section short. CRITICAL: this used to dereference a reference to
  // front() AFTER pop(), which is undefined behavior (the deque slot
  // was freed). Move-out then pop fixes it. Partial remainders go
  // back to the FRONT (not back) so PCM order is preserved against
  // newer chunks the next process_received_audio_ call might add.
#ifdef USE_ESP_IDF
  while (true) {
    std::vector<uint8_t> chunk;
    {
      AudioQueueLock lock(this->audio_queue_mutex_);
      if (!lock.acquired() || this->audio_queue_.empty()) {
        break;
      }
      chunk = std::move(this->audio_queue_.front());
      this->audio_queue_.pop_front();
    }
    size_t written = this->speaker_->play(chunk.data(), chunk.size());
    if (written == chunk.size()) {
      ESP_LOGD(TAG, "Drained queued audio chunk (%zu bytes)", chunk.size());
      continue;  // Try to drain the next chunk too
    }
    // Partial or zero write — speaker buffer is filling up. Put the
    // remainder (or the whole chunk) back at the FRONT and stop
    // draining; we'll resume on the next call.
    if (written > 0 && written < chunk.size()) {
      std::vector<uint8_t> remainder(chunk.begin() + written, chunk.end());
      AudioQueueLock lock(this->audio_queue_mutex_);
      if (lock.acquired()) {
        this->audio_queue_.push_front(std::move(remainder));
      }
      ESP_LOGD(TAG, "Partial play (%zu/%zu), remainder re-queued at front", written, chunk.size());
    } else if (written == 0) {
      AudioQueueLock lock(this->audio_queue_mutex_);
      if (lock.acquired()) {
        this->audio_queue_.push_front(std::move(chunk));
      }
    }
    break;
  }
#endif

  // Update last_speaker_audio_time_ for auto-stop tracking and
  // is_bot_speaking(). Atomic — read from main + mic tasks.
  this->last_speaker_audio_time_.store(millis(), std::memory_order_relaxed);

  // Send the new audio chunk.
  size_t bytes_written = this->speaker_->play(data, len);

  if (bytes_written == len) {
    return;  // Fully consumed; nothing to queue.
  }

  // Partial or zero write — queue what's left for the loop / next
  // call to drain. Build the remainder OUTSIDE the mutex so the
  // critical section is just the push_back.
  size_t remainder_offset = bytes_written;
#ifdef USE_ESP_IDF
  size_t free_heap = esp_get_free_heap_size();
  if (free_heap < MIN_FREE_HEAP_BYTES) {
    ESP_LOGW(TAG, "Low heap (%zu bytes), dropping %zu byte remainder", free_heap, len - remainder_offset);
    return;
  }
#endif
  std::vector<uint8_t> remainder(data + remainder_offset, data + len);
#ifdef USE_ESP_IDF
  AudioQueueLock lock(this->audio_queue_mutex_);
  if (!lock.acquired()) {
    ESP_LOGW(TAG, "Could not acquire audio_queue_mutex_, dropping remainder");
    return;
  }
  if (this->audio_queue_.size() >= MAX_QUEUE_SIZE) {
    ESP_LOGW(TAG, "Audio queue full (%zu/%zu), dropping remainder",
             this->audio_queue_.size(), MAX_QUEUE_SIZE);
    return;
  }
  this->audio_queue_.push_back(std::move(remainder));
  ESP_LOGD(TAG, "Queued %zu byte remainder (queue size: %zu/%zu)",
           len - remainder_offset, this->audio_queue_.size(), MAX_QUEUE_SIZE);
#endif
}

void VoiceAssistantWebSocket::on_microphone_data_(const std::vector<uint8_t> &data) {
  // Only process if connected and running
  if (!this->is_connected() || this->state_ != VOICE_ASSISTANT_WEBSOCKET_RUNNING) {
    return;
  }
  
  // In wake-word-only mode, mute the mic while the bot is speaking so
  // playback echo (or normal room noise) can't trigger server VAD. In
  // full-duplex mode, keep streaming and rely on the Voice PE's
  // hardware AEC + server VAD on the OpenAI Realtime API for barge-in.
  // Atomic load: the action handler that flips the mode runs on the
  // main task, the read here runs on the microphone callback task.
  if (this->barge_in_mode_.load(std::memory_order_relaxed) == BARGE_IN_WAKE_WORD &&
      this->is_bot_speaking()) {
    return;
  }
  
  // Microphone is configured for 16kHz, 32-bit, stereo (required by micro_wake_word)
  // OpenAI expects 24kHz, 16-bit, mono (non-beta API requirement)
  // Convert: 32-bit stereo -> 16-bit mono (16kHz) -> resample to 24kHz
  
  size_t stereo_32bit_samples = data.size() / (4 * 2);  // 4 bytes per 32-bit sample, 2 channels
  size_t mono_16khz_samples = stereo_32bit_samples;
  
  if (this->mono_buffer_.size() < mono_16khz_samples) {
    this->mono_buffer_.resize(mono_16khz_samples);
  }
  
  const int32_t *stereo_32bit = reinterpret_cast<const int32_t *>(data.data());
  int16_t *mono_16bit = this->mono_buffer_.data();
  
  for (size_t i = 0; i < stereo_32bit_samples; i++) {
    int32_t left_sample = stereo_32bit[i * 2];
    mono_16bit[i] = static_cast<int16_t>((left_sample >> 16));
  }
  
  // Resample from 16kHz to 24kHz (1.5x upsampling)
  size_t resampled_24khz_samples = (mono_16khz_samples * INPUT_SAMPLE_RATE) / MICROPHONE_SAMPLE_RATE;
  if (this->resampled_buffer_.size() < resampled_24khz_samples) {
    this->resampled_buffer_.resize(resampled_24khz_samples);
  }
  
  int16_t *resampled_24khz = this->resampled_buffer_.data();
  
  // Linear interpolation resampling: 16kHz -> 24kHz
  for (size_t i = 0; i < resampled_24khz_samples; i++) {
    float source_pos = (float)i * (float)MICROPHONE_SAMPLE_RATE / (float)INPUT_SAMPLE_RATE;
    size_t source_idx = (size_t)source_pos;
    float fraction = source_pos - source_idx;
    
    if (source_idx + 1 < mono_16khz_samples) {
      int16_t sample0 = mono_16bit[source_idx];
      int16_t sample1 = mono_16bit[source_idx + 1];
      resampled_24khz[i] = static_cast<int16_t>(sample0 + (sample1 - sample0) * fraction);
    } else if (source_idx < mono_16khz_samples) {
      resampled_24khz[i] = mono_16bit[source_idx];
    } else {
      resampled_24khz[i] = mono_16bit[mono_16khz_samples - 1];
    }
  }
  
  size_t resampled_bytes = resampled_24khz_samples * BYTES_PER_SAMPLE;
  this->send_audio_chunk_(reinterpret_cast<const uint8_t *>(resampled_24khz), resampled_bytes);
}

void VoiceAssistantWebSocket::set_barge_in_mode(BargeInMode mode) {
  BargeInMode previous = this->barge_in_mode_.exchange(mode, std::memory_order_relaxed);
  if (previous != mode) {
    ESP_LOGI(TAG, "Barge-in mode set to %s",
             mode == BARGE_IN_FULL_DUPLEX ? "full_duplex" : "wake_word");
  }
}

bool VoiceAssistantWebSocket::is_bot_speaking() const {
  // Bot is considered speaking if we received audio within the last 500ms.
  // Atomic load — written by the websocket task, read here from the
  // main task and the microphone task.
  uint32_t last = this->last_speaker_audio_time_.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  return (millis() - last) < 500;
}

void VoiceAssistantWebSocket::interrupt() {
  esp_websocket_client_handle_t client = this->websocket_client_;
  if (client == nullptr || !esp_websocket_client_is_connected(client)) {
    ESP_LOGW(TAG, "Cannot send interrupt - not connected");
    return;
  }

  ESP_LOGI(TAG, "Sending interrupt message to server");

  // JSON control frame — addon's raw_audio_serializer recognises this
  // substring and pushes a pipecat InterruptionFrame upstream. The
  // string is intentionally simple (not a structured JSON build) and
  // the substring match on the server is intentional: both ends are
  // ours and we never change the wire format.
  const char *interrupt_msg = "{\"type\":\"interrupt\"}";
  int sent = esp_websocket_client_send_text(client, interrupt_msg, strlen(interrupt_msg),
                                             pdMS_TO_TICKS(AUDIO_SEND_TIMEOUT_MS));

  if (sent < 0) {
    ESP_LOGW(TAG, "Failed to send interrupt message (network stall?)");
    return;
  }

  ESP_LOGI(TAG, "Interrupt message sent successfully");
  if (this->speaker_ != nullptr) {
    this->speaker_->stop();
  }
  // Clear audio queue under the mutex (other tasks may be pushing).
#ifdef USE_ESP_IDF
  {
    AudioQueueLock lock(this->audio_queue_mutex_);
    if (lock.acquired()) {
      this->audio_queue_.clear();
    }
  }
#endif
  // Atomic set — read concurrently by process_received_audio_ on the
  // websocket task. millis() == 0 is the "no interrupt" sentinel, so
  // if we happen to land there, bump by 1ms.
  uint32_t now = millis();
  if (now == 0) now = 1;
  this->interrupt_time_.store(now, std::memory_order_relaxed);
  ESP_LOGI(TAG, "Cleared audio queue and ignoring incoming audio for %u ms", INTERRUPT_IGNORE_AUDIO_MS);
}

void VoiceAssistantWebSocket::websocket_event_handler_(void *handler_args, 
                                                       esp_event_base_t base, 
                                                       int32_t event_id, 
                                                       void *event_data) {
  VoiceAssistantWebSocket *instance = static_cast<VoiceAssistantWebSocket *>(handler_args);
  esp_websocket_event_id_t ws_event_id = (esp_websocket_event_id_t) event_id;
  esp_websocket_event_data_t *ws_event_data = (esp_websocket_event_data_t *) event_data;
  
  instance->handle_websocket_event_(ws_event_id, ws_event_data);
}

void VoiceAssistantWebSocket::handle_websocket_event_(esp_websocket_event_id_t event_id,
                                                      esp_websocket_event_data_t *event_data) {
  switch (event_id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
      ESP_LOGI(TAG, "WebSocket connection attempt starting...");
      break;

    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected");
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_RUNNING;
      this->reconnect_attempts_ = 0;
      this->reconnect_pending_ = false;
      this->last_audio_send_ = millis();

      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }

      // Trigger connected automation
      this->connected_trigger_.trigger();
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_DISCONNECTED;

      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }
      this->disconnected_trigger_.trigger();

      // We need the main loop to destroy the client handle (the WS
      // task can't safely call esp_websocket_client_destroy on itself
      // — deadlock). Setting BOTH flags asks loop() to clean up AND
      // queue a reconnect. Without this, the old code set
      // reconnect_pending_ but the client handle stayed alive, so
      // loop's reconnect check (`websocket_client_ == nullptr`) never
      // tripped and reconnects never happened.
      if (!this->explicit_disconnect_) {
        this->pending_disconnect_ = true;
        this->reconnect_pending_ = true;
        this->last_reconnect_attempt_ = millis();
      } else {
        ESP_LOGI(TAG, "Explicit disconnect received, staying in idle mode (no reconnection)");
        this->explicit_disconnect_ = false;
        this->pending_disconnect_ = true;  // still need cleanup
      }
      break;

    case WEBSOCKET_EVENT_DATA:
      if (event_data == nullptr) {
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DATA fired with null event_data, ignoring");
        break;
      }
      if (event_data->op_code == 0x02) {  // Binary frame
        this->process_received_audio_(reinterpret_cast<const uint8_t *>(event_data->data_ptr), event_data->data_len);
      } else if (event_data->op_code == 0x01) {  // Text frame
        ESP_LOGI(TAG, "Received text message: %.*s", event_data->data_len, event_data->data_ptr);
        
        // Handle JSON control messages
        std::string message((const char *) event_data->data_ptr, event_data->data_len);
        if (message.find("\"type\":\"interrupt\"") != std::string::npos ||
            message.find("\"type\": \"interrupt\"") != std::string::npos) {
          ESP_LOGI(TAG, "Interrupt received, stopping speaker");
          if (this->speaker_ != nullptr) {
            this->speaker_->stop();
          }
        } else if (message.find("\"type\":\"disconnect\"") != std::string::npos ||
                   message.find("\"type\": \"disconnect\"") != std::string::npos) {
          ESP_LOGI(TAG, "Disconnect message received, stopping voice assistant and going to idle");
          // Mark that we received an explicit disconnect to prevent reconnection
          this->explicit_disconnect_ = true;
          // Stop the voice assistant (will go to idle mode)
          this->stop();
        }
      }
      break;
      
    case WEBSOCKET_EVENT_ERROR:
      if (event_data != nullptr) {
        // Log error information - note: error_handle may not be fully populated for all error types
        int sock_errno = event_data->error_handle.esp_transport_sock_errno;
        esp_err_t tls_err = event_data->error_handle.esp_tls_last_esp_err;
        
        ESP_LOGE(TAG, "WebSocket error - Type: %d, ESP-TLS Error: %s (0x%x), Socket errno: %d, Handshake Status: %d",
                 event_data->error_handle.error_type,
                 esp_err_to_name(tls_err),
                 tls_err,
                 sock_errno,
                 event_data->error_handle.esp_ws_handshake_status_code);
        
        // Log specific error types
        if (event_data->error_handle.error_type != WEBSOCKET_ERROR_TYPE_NONE) {
          switch (event_data->error_handle.error_type) {
            case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT:
              ESP_LOGE(TAG, "TCP transport error - check network connectivity and server address");
              if (sock_errno == 119) {
                ESP_LOGE(TAG, "Connection refused (errno 119) - check: 1) Server IP/port correct, 2) Same network subnet, 3) Firewall rules");
              } else if (sock_errno != 0) {
                ESP_LOGE(TAG, "Socket error (errno %d) - network connectivity issue", sock_errno);
              }
              break;
            case WEBSOCKET_ERROR_TYPE_HANDSHAKE:
              ESP_LOGE(TAG, "WebSocket handshake failed - Status code: %d", 
                       event_data->error_handle.esp_ws_handshake_status_code);
              break;
            case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:
              ESP_LOGE(TAG, "Pong timeout - server not responding to ping");
              break;
            case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE:
              ESP_LOGE(TAG, "Server closed connection");
              break;
            default:
              ESP_LOGE(TAG, "Unknown WebSocket error type: %d", event_data->error_handle.error_type);
              break;
          }
        } else {
          // Error type is NONE, but we still have error codes from ESP-IDF logs
          if (sock_errno == 119) {
            ESP_LOGE(TAG, "Connection refused (errno 119) - check: 1) Server IP/port correct, 2) Same network subnet, 3) Firewall rules");
          } else if (tls_err != ESP_OK) {
            ESP_LOGE(TAG, "Transport error: %s (0x%x)", 
                     esp_err_to_name(tls_err),
                     tls_err);
          } else if (sock_errno != 0) {
            ESP_LOGE(TAG, "Socket error (errno %d) - check network connectivity", sock_errno);
          }
        }
      } else {
        ESP_LOGE(TAG, "WebSocket error (no event data available)");
      }
      this->state_ = VOICE_ASSISTANT_WEBSOCKET_ERROR;
      
      if (this->state_callback_) {
        this->state_callback_(this->state_);
      }
      
      // Trigger error automation
      this->error_trigger_.trigger();

      // Same pattern as DISCONNECTED — ask loop() to destroy the
      // handle (we're in the WS task, can't do it ourselves) AND
      // queue a reconnect. Without `pending_disconnect_` set,
      // `websocket_client_` stays non-null and the reconnect check
      // in loop() never trips.
      this->pending_disconnect_ = true;
      this->reconnect_pending_ = true;
      this->last_reconnect_attempt_ = millis();
      break;
      
    default:
      break;
  }
}

}  // namespace voice_assistant_websocket
}  // namespace esphome

