// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/stream_buffer.h"

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

namespace thespeon {
namespace {

using metagraph::CallbackPacket;
using metagraph::CallbackType;

CallbackPacket Audio(std::size_t samples, bool is_final = false) {
  CallbackPacket packet{CallbackType::Audio, std::vector<float>(samples, 0.25f)};
  if (is_final) packet.metadata["is_final"] = true;
  return packet;
}

std::size_t SampleCount(const CallbackPacket& packet) {
  return std::get<std::vector<float>>(packet.payload).size();
}

class PrebufferAudioTest : public ::testing::Test {
 protected:
  std::vector<CallbackPacket> received;
  metagraph::CallbackHandler handler = PrebufferAudio(
      [this](CallbackPacket packet) { received.push_back(std::move(packet)); });
};

TEST_F(PrebufferAudioTest, HoldsShortChunksUntilOneSecondIsBuffered) {
  handler(Audio(20000));
  handler(Audio(20000));
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(SampleCount(received[0]), 40000u);
}

TEST_F(PrebufferAudioTest, ForwardsLaterChunksUnchanged) {
  handler(Audio(22050));
  handler(Audio(10));
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(SampleCount(received[1]), 10u);
}

TEST_F(PrebufferAudioTest, FlushesOnTheFinalChunk) {
  handler(Audio(100));
  handler(Audio(50, true));
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(SampleCount(received[0]), 150u);
}

TEST_F(PrebufferAudioTest, PassesOtherPacketsThroughImmediately) {
  handler(Audio(100));
  handler({CallbackType::TriggerSample, std::vector<std::int64_t>{7}});
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].type, CallbackType::TriggerSample);
}

TEST(PrebufferAudio, ToleratesAnEmptyHandler) {
  auto handler = PrebufferAudio({});
  EXPECT_NO_THROW(handler(Audio(22050)));
}

}  // namespace
}  // namespace thespeon
