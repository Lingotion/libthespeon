// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/utils/wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace thespeon {
namespace {

void WriteU16(std::ostream& output, std::uint16_t value) {
  const char bytes[]{static_cast<char>(value),
                     static_cast<char>(value >> 8)};
  output.write(bytes, 2);
}

void WriteU32(std::ostream& output, std::uint32_t value) {
  const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8),
                     static_cast<char>(value >> 16),
                     static_cast<char>(value >> 24)};
  output.write(bytes, 4);
}

void WritePcm(std::ostream& output, const metagraph::Tensor& audio) {
  const auto bytes = static_cast<std::uint32_t>(audio.size() * 2);
  output.write("RIFF", 4);
  WriteU32(output, 36 + bytes);
  output.write("WAVEfmt ", 8);
  WriteU32(output, 16);
  WriteU16(output, 1);
  WriteU16(output, 1);
  WriteU32(output, 44100);
  WriteU32(output, 88200);
  WriteU16(output, 2);
  WriteU16(output, 16);
  output.write("data", 4);
  WriteU32(output, bytes);

  for (std::size_t index = 0; index < audio.size(); ++index) {
    const auto sample =
        std::clamp(audio.float32_data()[index], -1.0f, 1.0f);
    const auto pcm =
        sample <= -1.0f
            ? std::numeric_limits<std::int16_t>::min()
            : static_cast<std::int16_t>(std::min<long>(
                  std::lround(sample * 32768.0f),
                  std::numeric_limits<std::int16_t>::max()));
    WriteU16(output, static_cast<std::uint16_t>(pcm));
  }
}

}  // namespace

void WriteWav(const std::filesystem::path& path,
              const metagraph::Tensor& audio) {
  if (audio.dtype() != metagraph::DType::Float32 || audio.size() == 0)
    throw std::runtime_error("Character module produced invalid audio");
  if (audio.size() >
      (std::numeric_limits<std::uint32_t>::max() - 36) / 2)
    throw std::runtime_error("Audio is too large for a WAV file");

  // Written beside the target and renamed over it, so a failed write never
  // leaves a truncated file or destroys the one already there.
  auto partial = path;
  partial += ".partial";
  try {
    {
      std::ofstream output(partial, std::ios::binary);
      if (!output)
        throw std::runtime_error("Could not create WAV: " + path.string());
      WritePcm(output, audio);
      output.close();
      if (!output) throw std::runtime_error("Failed while writing WAV");
    }
    std::filesystem::rename(partial, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
    throw;
  }
}

}  // namespace thespeon
