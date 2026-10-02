// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/build_inputs.h"

#include <gtest/gtest.h>

#include "core/inference/control_characters.h"
#include "core/inference/text_preprocess.h"

namespace thespeon::inference_detail {
namespace {

CharacterConfig Config() {
  CharacterConfig config;
  config.languages = {{"eng", 1}};
  config.phoneme_ids = {{EncodeUtf8(control::kSequenceStart), 100},
                        {EncodeUtf8(control::kSequenceEnd), 101}};
  config.emotion_ids = {{"Interest", 24}, {"Joy", 7}};
  return config;
}

metagraph::TensorMap Build(const EmotionWeights& blend) {
  SynthInputSegmentV100 segment;
  segment.text = "ab";
  segment.start_emotion = blend;
  segment.end_emotion = blend;
  segment.start_speed = segment.end_speed = 1.0;
  segment.start_loudness = segment.end_loudness = 1.0;
  return BuildCharacterInputs({segment}, {{1, 2}}, {{}}, Config(), {"eng"});
}

TEST(BuildCharacterInputs, LeavesEmotionsToTheGraphWhenNoneIsNamed) {
  const auto inputs = Build({});
  EXPECT_FALSE(inputs.contains("emotions"));
  EXPECT_FALSE(inputs.contains("emotions_blending"));
}

TEST(BuildCharacterInputs, SuppliesOneRowPerNamedEmotion) {
  const auto inputs = Build({{"joy", 1.0}});
  const auto& emotions = inputs.at("emotions");
  const auto& blending = inputs.at("emotions_blending");
  EXPECT_EQ(emotions.shape(), (std::vector<std::int64_t>{1, 1, 4}));
  EXPECT_EQ(blending.shape(), emotions.shape());
  for (std::size_t i = 0; i < 4; ++i) EXPECT_EQ(emotions.int64_data()[i], 7);
}

}  // namespace
}  // namespace thespeon::inference_detail
