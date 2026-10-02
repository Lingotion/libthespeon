// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/input/thespeon_input.h"

namespace thespeon {

// Segment boundary values are keypoints on a piecewise-linear curve over global
// character position; segments are resampled from it, so one with no opinion
// inherits the surrounding curve. Texts must already be normalized.

// False when the blend contributes no keypoint.
bool SanitizeEmotionBlend(EmotionWeights& blend);

EmotionWeights InterpolateEmotionBlends(const EmotionWeights& start,
                                        const EmotionWeights& end,
                                        double alpha);

// Leaves blends empty when nothing was supplied and no default was given.
void PopulateEmotionKeypoints(
    std::vector<SynthInputSegmentV100>& segments,
    const std::optional<std::string>& default_emotion);
void PopulateSpeedKeypoints(std::vector<SynthInputSegmentV100>& segments);
void PopulateLoudnessKeypoints(std::vector<SynthInputSegmentV100>& segments);

}  // namespace thespeon
