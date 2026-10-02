// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "audio_player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <miniaudio.h>

#include "core/engine.h"

namespace thespeon::cli {
namespace {

constexpr ma_uint32 kSampleRate = 44100;
constexpr ma_uint32 kChannels = 1;
// Room for the engine's prebuffered first chunk and then some, so Enqueue
// never spins waiting for playback while inference could be running.
constexpr ma_uint32 kRingBufferFrames = kSampleRate * 3;

class AudioPlayer {
 public:
  AudioPlayer() {
    auto result = ma_pcm_rb_init(ma_format_f32, kChannels, kRingBufferFrames,
                                 nullptr, nullptr, &ring_buffer_);
    if (result != MA_SUCCESS)
      throw std::runtime_error("Could not create audio playback buffer");
    ring_buffer_initialized_ = true;
    ma_pcm_rb_set_sample_rate(&ring_buffer_, kSampleRate);

    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kSampleRate;
    config.dataCallback = DataCallback;
    config.notificationCallback = NotificationCallback;
    config.pUserData = this;
    result = ma_device_init(nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
      ma_pcm_rb_uninit(&ring_buffer_);
      ring_buffer_initialized_ = false;
      throw std::runtime_error("Could not open the default audio device");
    }
    device_initialized_ = true;
  }

  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  ~AudioPlayer() {
    if (device_initialized_) {
      if (device_started_) ma_device_stop(&device_);
      ma_device_uninit(&device_);
    }
    if (ring_buffer_initialized_) ma_pcm_rb_uninit(&ring_buffer_);
  }

  void Enqueue(const std::vector<float>& samples, bool is_final) {
    if (final_chunk_received_)
      throw std::runtime_error("Received audio after the final chunk");

    std::size_t offset = 0;
    while (offset < samples.size()) {
      if (playback_failed_.load(std::memory_order_acquire))
        throw std::runtime_error("Audio playback failed");

      const auto remaining = samples.size() - offset;
      ma_uint32 frames = static_cast<ma_uint32>(
          std::min<std::size_t>(remaining,
                                std::numeric_limits<ma_uint32>::max()));
      void* destination = nullptr;
      const auto result =
          ma_pcm_rb_acquire_write(&ring_buffer_, &frames, &destination);
      if (result != MA_SUCCESS)
        throw std::runtime_error("Could not write to audio playback buffer");
      if (frames == 0) {
        Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      std::memcpy(destination, samples.data() + offset,
                  static_cast<std::size_t>(frames) * sizeof(float));
      if (ma_pcm_rb_commit_write(&ring_buffer_, frames) != MA_SUCCESS)
        throw std::runtime_error("Could not commit audio playback data");
      offset += frames;
    }

    if (!samples.empty()) {
      received_audio_ = true;
      Start();
    }
    if (is_final) {
      final_chunk_received_ = true;
      producer_finished_.store(true, std::memory_order_release);
    }
  }

  void Finish() {
    if (!received_audio_)
      throw std::runtime_error("Character module produced no audio");
    if (!final_chunk_received_)
      throw std::runtime_error(
          "Character-module audio did not include final-chunk metadata");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!drained_.load(std::memory_order_acquire)) {
      if (playback_failed_.load(std::memory_order_acquire))
        throw std::runtime_error("Audio playback failed");
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Timed out while draining audio playback");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (ma_device_stop(&device_) != MA_SUCCESS)
      throw std::runtime_error("Could not stop audio playback");
    device_started_ = false;
  }

 private:
  static void NotificationCallback(const ma_device_notification* notification) {
    if (notification->type != ma_device_notification_type_stopped) return;
    auto* player =
        static_cast<AudioPlayer*>(notification->pDevice->pUserData);
    player->playback_failed_.store(true, std::memory_order_release);
  }

  static void DataCallback(ma_device* device, void* output,
                           const void* input, ma_uint32 frame_count) {
    (void)input;
    auto* player = static_cast<AudioPlayer*>(device->pUserData);
    auto* samples = static_cast<float*>(output);
    std::fill_n(samples, frame_count, 0.0F);

    if (player->producer_finished_.load(std::memory_order_acquire) &&
        ma_pcm_rb_available_read(&player->ring_buffer_) == 0) {
      player->drained_.store(true, std::memory_order_release);
      return;
    }

    ma_uint32 copied = 0;
    while (copied < frame_count) {
      ma_uint32 frames = frame_count - copied;
      void* source = nullptr;
      if (ma_pcm_rb_acquire_read(&player->ring_buffer_, &frames, &source) !=
          MA_SUCCESS) {
        player->playback_failed_.store(true, std::memory_order_release);
        return;
      }
      if (frames == 0) return;
      std::memcpy(samples + copied, source,
                  static_cast<std::size_t>(frames) * sizeof(float));
      if (ma_pcm_rb_commit_read(&player->ring_buffer_, frames) != MA_SUCCESS) {
        player->playback_failed_.store(true, std::memory_order_release);
        return;
      }
      copied += frames;
    }
  }

  void Start() {
    if (device_started_) return;
    if (ma_device_start(&device_) != MA_SUCCESS)
      throw std::runtime_error("Could not start audio playback");
    device_started_ = true;
  }

  ma_pcm_rb ring_buffer_{};
  ma_device device_{};
  std::atomic<bool> producer_finished_{false};
  std::atomic<bool> drained_{false};
  std::atomic<bool> playback_failed_{false};
  bool ring_buffer_initialized_ = false;
  bool device_initialized_ = false;
  bool device_started_ = false;
  bool received_audio_ = false;
  bool final_chunk_received_ = false;
};

bool IsFinalAudioPacket(const metagraph::CallbackPacket& packet) {
  const auto found = packet.metadata.find("is_final");
  if (found == packet.metadata.end()) return false;
  const auto* value = std::get_if<bool>(&found->second);
  if (value == nullptr)
    throw std::runtime_error("Audio callback metadata 'is_final' is not boolean");
  return *value;
}

}  // namespace

void PlaySynthesis(const std::filesystem::path& data_directory,
                   const ThespeonInput& input,
                   const metagraph::CancelToken& cancel) {
  AudioPlayer player;
  Engine engine(data_directory);
  engine.SynthesizeWithCallbacks(
      input, [&](metagraph::CallbackPacket packet) {
        if (packet.type == metagraph::CallbackType::TriggerSample) {
          const auto* indices =
              std::get_if<std::vector<std::int64_t>>(&packet.payload);
          if (indices == nullptr) return;
          std::cerr << "audio sample requests:";
          for (const auto index : *indices) std::cerr << ' ' << index;
          std::cerr << '\n';
          return;
        }
        if (packet.type != metagraph::CallbackType::Audio) return;
        const auto* samples = std::get_if<std::vector<float>>(&packet.payload);
        if (samples == nullptr)
          throw std::runtime_error("Audio callback has an invalid payload");
        player.Enqueue(*samples, IsFinalAudioPacket(packet));
      },
      cancel);
  player.Finish();
}

}  // namespace thespeon::cli
