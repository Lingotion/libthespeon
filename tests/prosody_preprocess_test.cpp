// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/inference/prosody_preprocess.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace thespeon {
namespace {

void ExpectBlend(const EmotionWeights& actual, const EmotionWeights& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (const auto& [name, weight] : expected) {
    const auto found = actual.find(name);
    ASSERT_NE(found, actual.end()) << name;
    EXPECT_NEAR(found->second, weight, 1e-9) << name;
  }
}

std::vector<SynthInputSegmentV100> Segments(
    std::initializer_list<const char*> texts) {
  std::vector<SynthInputSegmentV100> result;
  for (const auto* text : texts) result.push_back({.text = text});
  return result;
}

TEST(SanitizeEmotionBlend, FoldsCaseDropsNoneClampsAndNormalizes) {
  EmotionWeights blend{{"Joy", 0.5}, {"joy", 1.0}, {"None", 5.0},
                       {"Anger", 0.5}, {"Fear", 0.0}};
  EXPECT_TRUE(SanitizeEmotionBlend(blend));
  ExpectBlend(blend, {{"joy", 2.0 / 3.0}, {"anger", 1.0 / 3.0}});
}

TEST(SanitizeEmotionBlend, TreatsNonFiniteWeights) {
  EmotionWeights blend{{"joy", std::numeric_limits<double>::infinity()},
                       {"anger", std::nan("")}};
  EXPECT_TRUE(SanitizeEmotionBlend(blend));
  ExpectBlend(blend, {{"joy", 1.0}});
}

TEST(SanitizeEmotionBlend, ClearsBlendsWithNoWeight) {
  EmotionWeights blend{{"joy", 0.0}, {"none", 1.0}};
  EXPECT_FALSE(SanitizeEmotionBlend(blend));
  EXPECT_TRUE(blend.empty());
}

TEST(InterpolateEmotionBlends, MixesAndClampsAlpha) {
  const EmotionWeights joy{{"joy", 1.0}};
  const EmotionWeights anger{{"anger", 1.0}};
  ExpectBlend(InterpolateEmotionBlends(joy, anger, 0.25),
              {{"joy", 0.75}, {"anger", 0.25}});
  ExpectBlend(InterpolateEmotionBlends(joy, anger, 2.0), {{"anger", 1.0}});
}

TEST(PopulateEmotionKeypoints, InterpolatesAcrossSegmentsWithoutOpinion) {
  auto segments = Segments({"abcd", "efgh", "ijkl"});
  segments[0].start_emotion = EmotionWeights{{"Joy", 1.0}};
  segments[2].end_emotion = EmotionWeights{{"Anger", 1.0}};
  PopulateEmotionKeypoints(segments, std::nullopt);

  // Keypoints sit at characters 0 and 11; every boundary samples that line.
  ExpectBlend(*segments[0].start_emotion, {{"joy", 1.0}});
  ExpectBlend(*segments[0].end_emotion,
              {{"joy", 8.0 / 11.0}, {"anger", 3.0 / 11.0}});
  ExpectBlend(*segments[1].start_emotion,
              {{"joy", 7.0 / 11.0}, {"anger", 4.0 / 11.0}});
  ExpectBlend(*segments[1].end_emotion,
              {{"joy", 4.0 / 11.0}, {"anger", 7.0 / 11.0}});
  ExpectBlend(*segments[2].end_emotion, {{"anger", 1.0}});
}

TEST(PopulateEmotionKeypoints, HoldsTheOnlyKeypointFlat) {
  auto segments = Segments({"abcd", "efgh"});
  segments[1].end_emotion = EmotionWeights{{"Joy", 1.0}};
  PopulateEmotionKeypoints(segments, std::string("Interest"));
  ExpectBlend(*segments[0].start_emotion, {{"joy", 1.0}});
}

TEST(PopulateEmotionKeypoints, FallsBackToDefaultEmotion) {
  auto segments = Segments({"abcd"});
  PopulateEmotionKeypoints(segments, std::string("Interest"));
  ExpectBlend(*segments[0].start_emotion, {{"interest", 1.0}});
  ExpectBlend(*segments[0].end_emotion, {{"interest", 1.0}});
}

TEST(PopulateEmotionKeypoints, LeavesBlendsEmptyWithoutAnyEmotion) {
  auto segments = Segments({"abcd"});
  PopulateEmotionKeypoints(segments, std::nullopt);
  EXPECT_TRUE(segments[0].start_emotion->empty());
  EXPECT_TRUE(segments[0].end_emotion->empty());
}

TEST(PopulateSpeedKeypoints, DefaultsToUnitSpeed) {
  auto segments = Segments({"abcd", "efgh"});
  PopulateSpeedKeypoints(segments);
  for (const auto& segment : segments) {
    EXPECT_EQ(segment.start_speed, 1.0);
    EXPECT_EQ(segment.end_speed, 1.0);
  }
}

TEST(PopulateSpeedKeypoints, KeepsExplicitEndpoints) {
  auto segments = Segments({"abcd", "efgh"});
  segments[0].start_speed = 0.5;
  segments[0].end_speed = 2.0;
  segments[1].start_speed = 3.0;
  segments[1].end_speed = 4.0;
  PopulateSpeedKeypoints(segments);
  EXPECT_EQ(segments[0].start_speed, 0.5);
  EXPECT_EQ(segments[0].end_speed, 2.0);
  EXPECT_EQ(segments[1].start_speed, 3.0);
  EXPECT_EQ(segments[1].end_speed, 4.0);
}

TEST(PopulateLoudnessKeypoints, RejectsEmptySegments) {
  auto segments = Segments({"abcd", ""});
  EXPECT_THROW(PopulateLoudnessKeypoints(segments), std::runtime_error);
}

}  // namespace
}  // namespace thespeon
