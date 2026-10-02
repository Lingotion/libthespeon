#include "metagraph_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void WriteU16(std::ostream& output, std::uint16_t value) {
  const char bytes[] = {static_cast<char>(value & 0xff),
                        static_cast<char>((value >> 8) & 0xff)};
  output.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream& output, std::uint32_t value) {
  const char bytes[] = {
      static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff),
      static_cast<char>((value >> 16) & 0xff),
      static_cast<char>((value >> 24) & 0xff)};
  output.write(bytes, sizeof(bytes));
}

void WriteWav(const std::filesystem::path& path, const metagraph::Tensor& audio) {
  if (audio.dtype() != metagraph::DType::Float32)
    throw std::runtime_error("Lara returned non-float audio");
  if (audio.size() > (std::numeric_limits<std::uint32_t>::max() - 36) / 2)
    throw std::runtime_error("Audio is too large for a RIFF WAV file");

  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("Could not create WAV: " + path.string());
  const auto data_bytes = static_cast<std::uint32_t>(audio.size() * 2);
  output.write("RIFF", 4);
  WriteU32(output, 36 + data_bytes);
  output.write("WAVEfmt ", 8);
  WriteU32(output, 16);
  WriteU16(output, 1);
  WriteU16(output, 1);
  WriteU32(output, 44100);
  WriteU32(output, 44100 * 2);
  WriteU16(output, 2);
  WriteU16(output, 16);
  output.write("data", 4);
  WriteU32(output, data_bytes);

  for (std::size_t i = 0; i < audio.size(); ++i) {
    const float sample = std::clamp(audio.float32_data()[i], -1.0F, 1.0F);
    const auto pcm = sample <= -1.0F
                         ? std::numeric_limits<std::int16_t>::min()
                         : static_cast<std::int16_t>(std::min<long>(
                               std::lround(sample * 32768.0F),
                               std::numeric_limits<std::int16_t>::max()));
    WriteU16(output, static_cast<std::uint16_t>(pcm));
  }
  if (!output) throw std::runtime_error("Failed while writing WAV: " + path.string());
}

metagraph::TensorMap LaraInputs() {
  const std::vector<std::int64_t> phonemes = {
      145, 7,   108, 1,   62,  122, 121, 12,  108, 1,   62,  121, 14,  108,
      5,   62,  12,  121, 108, 62,  26,  121, 4,   108, 50,  110, 76,  62,
      14,  125, 121, 108, 27,  14,  4,   121, 108, 1,   62,  121, 38,  4,
      12,  108, 1,   62,  48,  121, 29,  108, 27,  20,  121, 29,  108, 62,
      19,  121, 22,  108, 42,  62,  19,  121, 108, 27,  10,  20,  62,  32,
      121, 10,  108, 27,  14,  121, 2,   108, 8,   110, 121, 4,   96,  108,
      49,  14,  48,  76,  109, 5,   62,  80,  118, 4,   121, 108, 62,  14,
      121, 76,  108, 8,   110, 47,  11,  121, 20,  108, 1,   62,  12,  121,
      62,  14,  19,  108, 1,   62,  4,   121, 29,  108, 47,  121, 54,  108,
      5,   62,  12,  121, 108, 49,  14,  4,   96,  62,  14,  126, 121, 109,
      1,   62,  20,  108, 8,   110, 121, 108, 8,   110, 22,  47,  14,  121,
      23,  108, 50,  110, 10,  19,  121, 108, 39,  110, 14,  121, 11,  108,
      15,  88,  121, 16,  48,  6,   108, 15,  110, 76,  12,  47,  14,  19,
      121, 12,  108, 15,  88,  2,   47,  11,  121, 6,   108, 15,  88,  14,
      26,  121, 23,  62,  29,  108, 1,   88,  20,  121, 108, 49,  14,  8,
      121, 6,   76,  108, 5,   62,  12,  121, 4,   76,  108, 39,  110, 16,
      19,  121, 108, 62,  14,  121, 29,  108, 47,  121, 54,  76,  108, 27,
      6,   62,  10,  19,  121, 76,  108, 49,  14,  4,   48,  76,  62,  32,
      122, 146};
  const auto length = static_cast<std::int64_t>(phonemes.size());

  std::vector<std::int64_t> emotions;
  emotions.reserve(phonemes.size() * 2);
  emotions.insert(emotions.end(), phonemes.size(), 3);
  emotions.insert(emotions.end(), phonemes.size(), 4);

  std::vector<float> blending(phonemes.size() * 2);
  for (std::size_t i = 0; i < phonemes.size(); ++i) {
    const auto value = static_cast<float>(static_cast<double>(i) /
                                          static_cast<double>(phonemes.size() - 1));
    blending[i] = value;
    blending[phonemes.size() + i] = static_cast<float>(
        static_cast<double>(phonemes.size() - 1 - i) /
        static_cast<double>(phonemes.size() - 1));
  }

  metagraph::TensorMap inputs;
  inputs["phoneme_keys"] = metagraph::Tensor::Int64({1, length}, phonemes);
  inputs["emotions"] =
      metagraph::Tensor::Int64({1, 2, length}, std::move(emotions));
  inputs["emotions_blending"] =
      metagraph::Tensor::Float32({1, 2, length}, std::move(blending));
  inputs["actors"] = metagraph::Tensor::Int64({1, 1}, {1});
  inputs["languages"] = metagraph::Tensor::FullInt64({1, length}, 1);
  inputs["text_lengths"] = metagraph::Tensor::Int64({1}, {length});
  inputs["speed"] = metagraph::Tensor::FullFloat32({1, length * 2}, 1.0F);
  inputs["loudness"] = metagraph::Tensor::FullFloat32({1, 1, length}, 1.0F);
  inputs["target_phoneme_indices"] = metagraph::Tensor::Int64({2}, {2, 5});
  return inputs;
}

struct Arguments {
  std::filesystem::path graph;
  std::filesystem::path models;
  std::filesystem::path output = "test_lara_cpp.wav";
};

Arguments ParseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if ((option == "-g" || option == "--graph_file_path") && i + 1 < argc) {
      arguments.graph = argv[++i];
    } else if ((option == "-m" || option == "--model_files_path") &&
               i + 1 < argc) {
      arguments.models = argv[++i];
    } else if ((option == "-o" || option == "--output") && i + 1 < argc) {
      arguments.output = argv[++i];
    } else if (option == "-h" || option == "--help") {
      std::cout << "Usage: run_lara -g GRAPH -m MODEL_DIRECTORY "
                   "[-o test_lara_cpp.wav]\n";
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
  }
  if (arguments.graph.empty() || arguments.models.empty())
    throw std::runtime_error("Both -g GRAPH and -m MODEL_DIRECTORY are required");
  return arguments;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = ParseArguments(argc, argv);
    metagraph::MetaGraphRunner runner(arguments.models);
    const auto audio =
        runner.ExecuteGraphWithResult(arguments.graph, LaraInputs());
    if (audio.size() == 0) throw std::runtime_error("MetaGraph produced no audio");
    WriteWav(arguments.output, audio);
    std::cout << "Wrote " << audio.size() << " samples to "
              << arguments.output.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
  }
  return 1;
}
