// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/utils/wav_writer.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "test_support.h"

namespace thespeon {
namespace {

std::uint32_t ReadU32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index)
    value = (value << 8) | static_cast<unsigned char>(bytes[offset + index]);
  return value;
}

std::int16_t ReadI16(const std::string& bytes, std::size_t offset) {
  return static_cast<std::int16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      (static_cast<unsigned char>(bytes[offset + 1]) << 8));
}

TEST(WriteWav, WritesMono44100Pcm16) {
  test::TempDirectory directory;
  const auto path = directory.path() / "out.wav";
  WriteWav(path, metagraph::Tensor::Float32(
                     {1, 5}, {0.0f, 0.5f, 1.0f, -1.0f, 2.0f}));

  const auto bytes = test::ReadFile(path);
  ASSERT_EQ(bytes.size(), 44u + 10u);
  EXPECT_EQ(bytes.substr(0, 4), "RIFF");
  EXPECT_EQ(ReadU32(bytes, 4), 36u + 10u);
  EXPECT_EQ(bytes.substr(8, 8), "WAVEfmt ");
  EXPECT_EQ(ReadU32(bytes, 24), 44100u);
  EXPECT_EQ(ReadU32(bytes, 28), 88200u);
  EXPECT_EQ(bytes.substr(36, 4), "data");
  EXPECT_EQ(ReadU32(bytes, 40), 10u);

  EXPECT_EQ(ReadI16(bytes, 44), 0);
  EXPECT_EQ(ReadI16(bytes, 46), 16384);
  EXPECT_EQ(ReadI16(bytes, 48), 32767);
  EXPECT_EQ(ReadI16(bytes, 50), -32768);
  EXPECT_EQ(ReadI16(bytes, 52), 32767);
}

TEST(WriteWav, RejectsEmptyOrNonFloatAudio) {
  test::TempDirectory directory;
  const auto path = directory.path() / "out.wav";
  EXPECT_THROW(WriteWav(path, metagraph::Tensor::Float32({0}, {})),
               std::runtime_error);
  EXPECT_THROW(WriteWav(path, metagraph::Tensor::Int64({1}, {1})),
               std::runtime_error);
}

TEST(WriteWav, ReportsUnwritablePaths) {
  test::TempDirectory directory;
  EXPECT_THROW(WriteWav(directory.path() / "missing" / "out.wav",
                        metagraph::Tensor::Float32({1}, {0.0f})),
               std::runtime_error);
}

TEST(WriteWav, ReplacesAnExistingFileWithoutLeavingAPartialOne) {
  test::TempDirectory directory;
  const auto path = directory.path() / "out.wav";
  WriteWav(path, metagraph::Tensor::Float32({1, 5}, {0, 0, 0, 0, 0}));
  WriteWav(path, metagraph::Tensor::Float32({1, 1}, {0.0f}));

  EXPECT_EQ(test::ReadFile(path).size(), 44u + 2u);
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "out.wav.partial"));
}

TEST(WriteWav, LeavesTheTargetAloneWhenItCannotBeReplaced) {
  test::TempDirectory directory;
  const auto path = directory.path() / "out.wav";
  std::filesystem::create_directories(path / "occupied");

  EXPECT_ANY_THROW(WriteWav(path, metagraph::Tensor::Float32({1}, {0.0f})));
  EXPECT_TRUE(std::filesystem::is_directory(path / "occupied"));
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "out.wav.partial"));
}

}  // namespace
}  // namespace thespeon
