#pragma once

#include "esphome.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#ifdef USE_ESP_IDF
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#endif
#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace esphome {
namespace voice_assistant_websocket {

enum VoiceAssistantWebSocketState {
  VOICE_ASSISTANT_WEBSOCKET_IDLE = 0,
  VOICE_ASSISTANT_WEBSOCKET_STARTING,
  VOICE_ASSISTANT_WEBSOCKET_RUNNING,
  VOICE_ASSISTANT_WEBSOCKET_STOPPING,
  VOICE_ASSISTANT_WEBSOCKET_ERROR,
  VOICE_ASSISTANT_WEBSOCKET_DISCONNECTED
};

// How the satellite handles the user interrupting the bot mid-speech.
// WAKE_WORD: mic is muted while the bot is speaking, so playback echo
//   can't trigger server VAD; barge-in only happens when the local
//   wake-word detector fires (which then sends a JSON interrupt
//   message to the addon). This is the safe default — no ambient-noise
//   false-triggers.
// FULL_DUPLEX: mic keeps streaming during playback (relies on the
//   Voice PE's hardware AEC). Server-side VAD on the OpenAI Realtime
//   API detects user speech and cancels the bot response
//   automatically. Snappier barge-in but vulnerable to false triggers
//   from background noise, like the ChatGPT app.
enum BargeInMode {
  BARGE_IN_WAKE_WORD = 0,
  BARGE_IN_FULL_DUPLEX = 1,
};

class VoiceAssistantWebSocket : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_server_url(const std::string &url) { this->server_url_ = url; }
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
  void set_speaker(speaker::Speaker *spkr) { this->speaker_ = spkr; }

  void start();
  void stop();
  void request_start();
  void interrupt();  // Send interrupt message to server and stop speaker

  bool is_running() const { return this->state_.load(std::memory_order_relaxed) == VOICE_ASSISTANT_WEBSOCKET_RUNNING; }
  bool is_connected() const;
  bool is_bot_speaking() const;  // Check if bot is currently speaking (within 500ms of last audio)

  // Barge-in mode controls whether mic audio is suppressed while the
  // bot is speaking. See BargeInMode enum docs.
  void set_barge_in_mode(BargeInMode mode);
  BargeInMode get_barge_in_mode() const { return this->barge_in_mode_; }
  
  void set_state_callback(std::function<void(VoiceAssistantWebSocketState)> &&callback) {
    this->state_callback_ = std::move(callback);
  }
  
  // Automation triggers
  Trigger<> *get_connected_trigger() { return &this->connected_trigger_; }
  Trigger<> *get_disconnected_trigger() { return &this->disconnected_trigger_; }
  Trigger<> *get_error_trigger() { return &this->error_trigger_; }
  Trigger<> *get_stopped_trigger() { return &this->stopped_trigger_; }

 protected:
  void connect_websocket_();
  void disconnect_websocket_();
  void send_audio_chunk_(const uint8_t *data, size_t len);
  void process_received_audio_(const uint8_t *data, size_t len);
  void on_microphone_data_(const std::vector<uint8_t> &data);
  bool speaker_is_running_() const;
  bool speaker_is_stopped_() const;
  void start_speaker_if_stopped_();
  void stop_speaker_();
  size_t play_speaker_(const uint8_t *data, size_t len);
  void stop_speaker_after_interrupt_();
  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  
  std::string server_url_;
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  
#ifdef USE_ESP_IDF
  esp_websocket_client_handle_t websocket_client_{nullptr};
  mutable SemaphoreHandle_t websocket_client_mutex_{nullptr};
  mutable SemaphoreHandle_t speaker_mutex_{nullptr};
#else
  void *websocket_client_{nullptr};
#endif
  std::atomic<VoiceAssistantWebSocketState> state_{VOICE_ASSISTANT_WEBSOCKET_IDLE};
  
  std::function<void(VoiceAssistantWebSocketState)> state_callback_;
  
  // Automation triggers
  Trigger<> connected_trigger_{};
  Trigger<> disconnected_trigger_{};
  Trigger<> error_trigger_{};
  Trigger<> stopped_trigger_{};
  
  // Queue for audio data when the speaker buffer is full.
  // Touched from the ESPHome main loop AND the websocket task — every
  // access must hold audio_queue_mutex_. std::deque (not std::queue)
  // so we can push_front the remainder of a partial write to preserve
  // PCM order. Pop pattern: move-from-front THEN pop, never use a
  // reference to .front() after pop (that's UB).
  std::deque<std::vector<uint8_t>> audio_queue_;
#ifdef USE_ESP_IDF
  SemaphoreHandle_t audio_queue_mutex_{nullptr};
#endif
  static const size_t MAX_QUEUE_SIZE = 10;  // Max 10 chunks (~40KB) to prevent memory overflow
  static const size_t MIN_FREE_HEAP_BYTES = 15000;  // Minimum free heap required before queuing audio
  
  // Timing
  uint32_t last_audio_send_{0};
  uint32_t last_audio_receive_{0};
  static const uint32_t AUDIO_SEND_INTERVAL_MS = 100;  // Send 100ms chunks
  static const uint32_t MICROPHONE_SAMPLE_RATE = 16000;  // 16kHz from microphone (required by micro_wake_word)
  static const uint32_t INPUT_SAMPLE_RATE = 24000;       // 24kHz for OpenAI input (non-beta API requirement)
  static const uint32_t OUTPUT_SAMPLE_RATE = 24000;    // 24kHz for OpenAI output
  static const uint32_t BYTES_PER_SAMPLE = 2;          // 16-bit = 2 bytes
  static const uint32_t INPUT_BUFFER_SIZE = (INPUT_SAMPLE_RATE * BYTES_PER_SAMPLE * AUDIO_SEND_INTERVAL_MS) / 1000;
  
  // Auto-stop tracking. Written by the websocket task (when speaker
  // audio arrives) and the main task (initial seed in start()); read
  // by the main task (loop auto-stop check + is_bot_speaking) and the
  // microphone task (mic-mute decision). std::atomic so we don't rely
  // on 32-bit alignment + lack of compiler caching for correctness.
  std::atomic<uint32_t> last_speaker_audio_time_{0};
  static const uint32_t AUTO_STOP_INACTIVITY_MS = 20000;  // Stop after 20 seconds of speaker inactivity

  // Audio conversion buffers (microphone task only — no sync needed).
  std::vector<int16_t> mono_buffer_;       // 32-bit stereo -> 16-bit mono (input)
  std::vector<int16_t> resampled_buffer_;  // 16kHz -> 24kHz resampling (1.5x upsampling)

  std::atomic<bool> pending_start_{false};
  std::atomic<bool> pending_disconnect_{false};  // Flag to disconnect in loop() (cannot be called from websocket task)
  std::atomic<bool> reconnect_pending_{false};
  std::atomic<bool> explicit_disconnect_{false};  // Flag to prevent reconnection after explicit disconnect
  std::atomic<uint32_t> reconnect_attempts_{0};
  static const uint32_t MAX_RECONNECT_ATTEMPTS = 5;
  static const uint32_t RECONNECT_DELAY_MS = 5000;
  std::atomic<uint32_t> last_reconnect_attempt_{0};

  // Cross-task scalars — same reasoning as last_speaker_audio_time_.
  std::atomic<uint32_t> interrupt_time_{0};  // Time when interrupt was sent (to ignore audio for a short period)
  static const uint32_t INTERRUPT_IGNORE_AUDIO_MS = 500;  // Ignore audio for 500ms after interrupt
  static const uint32_t AUDIO_SEND_TIMEOUT_MS = 100;       // Bounded send_bin timeout; chunk dropped on miss
  // Default to the safer wake-word-only mode; YAML can override at
  // startup (e.g. via a restored template select) and at runtime.
  // Written by the action handler (main task), read by the mic
  // callback task.
  std::atomic<BargeInMode> barge_in_mode_{BARGE_IN_WAKE_WORD};
};

// Action classes for automations (defined outside the main class)
template<typename... Ts> class VoiceAssistantWebSocketStartAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketStartAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketStopAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketStopAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

// Condition classes for automations (defined outside the main class)
template<typename... Ts> class VoiceAssistantWebSocketIsRunningCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsRunningCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketIsConnectedCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsConnectedCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketIsBotSpeakingCondition : public Condition<Ts...> {
 public:
  VoiceAssistantWebSocketIsBotSpeakingCondition(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  bool check(const Ts &...x) override { return this->parent_->is_bot_speaking(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketInterruptAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketInterruptAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->interrupt(); }
 protected:
  VoiceAssistantWebSocket *parent_;
};

template<typename... Ts> class VoiceAssistantWebSocketSetBargeInModeAction : public Action<Ts...> {
 public:
  VoiceAssistantWebSocketSetBargeInModeAction(VoiceAssistantWebSocket *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(BargeInMode, mode)
  void play(const Ts &...x) override {
    this->parent_->set_barge_in_mode(this->mode_.value(x...));
  }
 protected:
  VoiceAssistantWebSocket *parent_;
};

}  // namespace voice_assistant_websocket
}  // namespace esphome
