// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/stream_buffer.h"

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace thespeon {
namespace {

constexpr std::size_t kPrebufferSamples = 22050;

bool IsFinalAudioChunk(const metagraph::CallbackPacket& packet) {
  const auto found = packet.metadata.find("is_final");
  if (found == packet.metadata.end()) return false;
  const auto* value = std::get_if<bool>(&found->second);
  return value != nullptr && *value;
}

}  // namespace

metagraph::CallbackHandler PrebufferAudio(metagraph::CallbackHandler handler) {
  return [handler = std::move(handler), prebuffered = false,
          pending = std::vector<float>{}](
             metagraph::CallbackPacket packet) mutable {
    auto* samples = std::get_if<std::vector<float>>(&packet.payload);
    if (!prebuffered && packet.type == metagraph::CallbackType::Audio &&
        samples != nullptr) {
      pending.insert(pending.end(), samples->begin(), samples->end());
      if (pending.size() < kPrebufferSamples && !IsFinalAudioChunk(packet))
        return;
      prebuffered = true;
      *samples = std::move(pending);
    }
    if (handler) handler(std::move(packet));
  };
}

}  // namespace thespeon
