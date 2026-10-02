// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#pragma once

namespace thespeon {
namespace control {

// Inserted into the phoneme stream as a symbol the character module knows, so
// it reaches the model as a short silence rather than as parser state.
inline constexpr char32_t kPause = U'⏸';

// Stripped from the text before anything else reads it; its position is
// reported back as an output sample index.
inline constexpr char32_t kAudioSampleRequest = U'◎';

// Frame the phoneme sequence. Reserved: user text carrying them would displace
// the real frame.
inline constexpr char32_t kSequenceStart = U'⏩';
inline constexpr char32_t kSequenceEnd = U'⏪';

}  // namespace control
}  // namespace thespeon
