// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "test_support.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace thespeon {
namespace {

using nlohmann::json;

struct Result {
  int exit_code = -1;
  std::string out;
  std::string err;
};

// Runs the built thespeon binary, which CMake names in THESPEON_CLI_PATH.
class CliTest : public ::testing::Test {
 protected:
  Result Thespeon(const std::filesystem::path& data_directory,
               const std::string& arguments) const {
    const auto out = scratch.path() / "stdout.txt";
    const auto err = scratch.path() / "stderr.txt";
    std::string command = Quote(THESPEON_CLI_PATH) + " --data-dir " +
                          Quote(data_directory) + ' ' + arguments + " >" +
                          Quote(out) + " 2>" + Quote(err);
#ifdef _WIN32
    // cmd strips the outer quotes when the line starts with one.
    command = '"' + command + '"';
#endif
    const int status = std::system(command.c_str());
    Result run;
#ifdef _WIN32
    run.exit_code = status;
#else
    run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    run.out = Unix(test::ReadFile(out));
    run.err = Unix(test::ReadFile(err));
    return run;
  }

  // The CLI writes text-mode streams, which end lines with \r\n on Windows.
  static std::string Unix(std::string text) {
#ifdef _WIN32
    std::erase(text, '\r');
#endif
    return text;
  }

  static std::string Quote(const std::filesystem::path& path) {
    return '"' + path.string() + '"';
  }

  void AddCharacter(const std::string& file, const std::string& name) const {
    const json config = {
        {"type", "lara"},
        {"module_identifier", "c-" + name},
        {"version", {{"major", 2}, {"minor", 1}, {"patch", 1}}},
        {"character", {{"charactername", name}}},
        {"tags", {{"module_type", "XL"}}},
        {"languages", {{{"iso639_2", "eng"}, {"iso3166_1", "US"}}}},
        {"emotionsets",
         {{{"emotionsetname", "Joy"},
           {"emotionsetkey", 7},
           {"emotionsetguide", "Feeling good."}},
          {{"emotionsetname", "Interest"}, {"emotionsetkey", 24}}}}};
    test::WriteFile(data.path() / "configs" / file, config.dump());
  }

  void AddLanguage(const std::string& file) const {
    const json config = {{"type", "phonemizer"},
                         {"module_identifier", "l-eng"},
                         {"name", "English"},
                         {"version", {{"major", 1}, {"minor", 0}, {"patch", 0}}},
                         {"languages", {{{"iso639_2", "eng"}}}}};
    test::WriteFile(data.path() / "configs" / file, config.dump());
  }

  test::TempDirectory data;
  test::TempDirectory scratch;
};

TEST_F(CliTest, ModelsListsCharactersWithTheVersionLast) {
  AddCharacter("1.json", "Ada");
  const auto run = Thespeon(data.path(), "models");
  EXPECT_EQ(run.exit_code, 0);
  EXPECT_NE(run.out.find("Data directory: " + data.path().string()),
            std::string::npos);
  EXPECT_NE(run.out.find("Character modules (1):"), std::string::npos);
  EXPECT_NE(run.out.find("\"Ada\"  XL  languages: eng-US  v2.1.1"),
            std::string::npos)
      << run.out;
  EXPECT_TRUE(run.err.empty()) << run.err;
}

TEST_F(CliTest, ModelsJsonPrintsTheCatalogDocument) {
  AddCharacter("1.json", "Ada");
  const auto run = Thespeon(data.path(), "models --json");
  EXPECT_EQ(run.exit_code, 0);
  const auto listing = json::parse(run.out);
  ASSERT_EQ(listing["characterModules"].size(), 1u);
  EXPECT_EQ(listing["characterModules"][0]["name"], "Ada");
  EXPECT_TRUE(listing["languageModules"].empty());
  EXPECT_TRUE(listing["problems"].empty());
}

TEST_F(CliTest, ModelsWarnsOnStderrAndFailsWhenAConfigIsUnreadable) {
  AddCharacter("1.json", "Ada");
  test::WriteFile(data.path() / "configs" / "broken.json", "{");

  const auto text = Thespeon(data.path(), "models");
  EXPECT_EQ(text.exit_code, 1);
  EXPECT_NE(text.out.find("\"Ada\""), std::string::npos);
  EXPECT_EQ(text.out.find("broken.json"), std::string::npos);
  EXPECT_EQ(text.err.rfind("Warning: broken.json: ", 0), 0u) << text.err;

  const auto json_run = Thespeon(data.path(), "models --json");
  EXPECT_EQ(json_run.exit_code, 1);
  EXPECT_EQ(json::parse(json_run.out)["problems"].size(), 1u);
}

TEST_F(CliTest, ModelsHintsWhenTheDirectoryHoldsNoModules) {
  const auto run = Thespeon(data.path(), "models");
  EXPECT_EQ(run.exit_code, 0);
  EXPECT_NE(run.out.find("Character modules (0):"), std::string::npos);
  EXPECT_EQ(run.err.rfind("No modules installed", 0), 0u) << run.err;
}

TEST_F(CliTest, ModelsFailsWithNothingOnStdoutWhenTheDirectoryIsMissing) {
  const auto missing = data.path() / "missing";
  for (const std::string arguments : {"models", "models --json"}) {
    const auto run = Thespeon(missing, arguments);
    EXPECT_EQ(run.exit_code, 1) << arguments;
    EXPECT_TRUE(run.out.empty()) << arguments << ": " << run.out;
    EXPECT_NE(run.err.find("does not exist"), std::string::npos) << run.err;
  }
}

// A usage error names the problem on its first line, then prints the usage.
void ExpectUsageError(const Result& run, const std::string& message) {
  EXPECT_EQ(run.exit_code, 2);
  EXPECT_TRUE(run.out.empty()) << run.out;
  EXPECT_EQ(run.err.rfind("Error: " + message + '\n', 0), 0u) << run.err;
  EXPECT_NE(run.err.find("usage:"), std::string::npos) << run.err;
}

TEST_F(CliTest, ModelsRejectsUnknownArguments) {
  ExpectUsageError(Thespeon(data.path(), "models --bogus"),
                   "Unknown option --bogus");
  ExpectUsageError(Thespeon(data.path(), "models extra"),
                   "Unexpected argument extra");
}

TEST_F(CliTest, SynthesizeRejectsArgumentsItWouldOtherwiseIgnore) {
  ExpectUsageError(
      Thespeon(data.path(), "synthesize --model X --text hi --ouput f.wav"),
      "Unknown option --ouput");
  ExpectUsageError(Thespeon(data.path(), "synthesize --model X --text hi x"),
                   "Unexpected argument x");
  ExpectUsageError(
      Thespeon(data.path(), "synthesize --model X --text hi --output"),
      "--output needs a value");
  ExpectUsageError(
      Thespeon(data.path(), "synthesize --model X --text a --text b"),
      "--text was given more than once");
  ExpectUsageError(Thespeon(data.path(), "bench --model X --text hi --play"),
                   "Unknown option --play");
  ExpectUsageError(Thespeon(data.path(), "delete --model X --froce"),
                   "Unknown option --froce");
}

TEST_F(CliTest, RejectsAnOptionWhereAValueBelongs) {
  const std::string hint = "; use --model=VALUE if the value starts with --";
  ExpectUsageError(Thespeon(data.path(), "synthesize --model --text hi"),
                   "--model needs a value, but is followed by --text" + hint);
  ExpectUsageError(Thespeon(data.path(), "delete --model --all"),
                   "--model needs a value, but is followed by --all" + hint);
  ExpectUsageError(
      Thespeon(data.path(), "--data-dir --model models"),
      "--data-dir needs a value, but is followed by --model; use "
      "--data-dir=VALUE if the value starts with --");
}

TEST_F(CliTest, SynthesizeNamesWhatIsMissingFromTheInput) {
  ExpectUsageError(Thespeon(data.path(), "synthesize --model X --text \"\""),
                   "--text is empty");
  ExpectUsageError(Thespeon(data.path(), "synthesize --model X"),
                   "--model needs --text");
  ExpectUsageError(Thespeon(data.path(), "synthesize --text hi"),
                   "--text needs --model");
  ExpectUsageError(Thespeon(data.path(), "synthesize --module-type XL"),
                   "--module-type and --module-version narrow --model");
}

TEST_F(CliTest, SynthesizeRejectsANegativeThreadCount) {
  AddCharacter("1.json", "Ada");
  const auto run =
      Thespeon(data.path(), "synthesize --model Ada --text Hi --threads -1");
  EXPECT_EQ(run.exit_code, 1);
  EXPECT_NE(run.err.find("--threads takes a non-negative number"),
            std::string::npos)
      << run.err;
}

// The character has no language module, so a run whose arguments are accepted
// gets as far as loading and fails there rather than with a usage error.
void ExpectArgumentsAccepted(const Result& run) {
  EXPECT_EQ(run.exit_code, 1) << run.err;
  EXPECT_NE(run.err.find("No compatible language module"), std::string::npos)
      << run.err;
}

TEST_F(CliTest, SynthesizeAndBenchAcceptInferenceOptions) {
  AddCharacter("1.json", "Ada");
  const auto output = scratch.path() / "out.wav";
  ExpectArgumentsAccepted(Thespeon(
      data.path(), "synthesize --model Ada --text Hi --threads 2 --spinning "
                   "--low-memory --output " + Quote(output)));
  ExpectArgumentsAccepted(Thespeon(
      data.path(),
      "bench --model Ada --text Hi --threads 2 --spinning --low-memory"));
}

TEST_F(CliTest, BenchAcceptsSweep) {
  AddCharacter("1.json", "Ada");
  ExpectArgumentsAccepted(
      Thespeon(data.path(), "bench --model Ada --text Hi --sweep --repeats 1"));
}

TEST_F(CliTest, BenchSweepRejectsExplicitInferenceOptions) {
  AddCharacter("1.json", "Ada");
  const auto run =
      Thespeon(data.path(), "bench --model Ada --text Hi --sweep --threads 2");
  EXPECT_EQ(run.exit_code, 2);
  EXPECT_EQ(run.err.rfind("usage:", 0), 0u) << run.err;
}

TEST_F(CliTest, EmotionsListsNamesAndGuidesOnlyWhenVerbose) {
  AddCharacter("1.json", "Ada");
  const auto run = Thespeon(data.path(), "emotions --model Ada");
  EXPECT_EQ(run.exit_code, 0) << run.err;
  EXPECT_NE(run.out.find("c-Ada  \"Ada\"  XL  v2.1.1\n"), std::string::npos)
      << run.out;
  EXPECT_NE(run.out.find("Emotions (2):\n  Joy\n  Interest\n"),
            std::string::npos)
      << run.out;
  EXPECT_EQ(run.out.find("Feeling good."), std::string::npos) << run.out;

  for (const std::string flag : {"-v", "--verbose"}) {
    const auto verbose = Thespeon(data.path(), "emotions --model Ada " + flag);
    EXPECT_EQ(verbose.exit_code, 0) << verbose.err;
    EXPECT_NE(verbose.out.find("Emotions (2):\n  Joy\n      Feeling good.\n"
                               "  Interest\n"),
              std::string::npos)
        << flag << ": " << verbose.out;
  }

  const auto listed =
      json::parse(Thespeon(data.path(), "emotions --model Ada --json").out);
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0]["name"], "Joy");
  EXPECT_EQ(listed[0]["guide"], "Feeling good.");
}

TEST_F(CliTest, EmotionsNeedsAnInstalledModel) {
  ExpectUsageError(Thespeon(data.path(), "emotions"), "emotions needs --model");
  const auto run = Thespeon(data.path(), "emotions --model Nobody");
  EXPECT_EQ(run.exit_code, 1);
  EXPECT_TRUE(run.out.empty()) << run.out;
}

TEST_F(CliTest, EmotionGoesOnlyWithModelAndText) {
  ExpectUsageError(
      Thespeon(data.path(), "synthesize --input \"{}\" --emotion Joy"),
      "--emotion goes with --model and --text; a document names its emotions "
      "itself");
  ExpectUsageError(Thespeon(data.path(), "synthesize --emotion Joy"),
                   "--emotion goes with --model and --text");
}

TEST_F(CliTest, ValidateChecksEmotionsAndLanguages) {
  AddCharacter("1.json", "Ada");
  const auto missing_language =
      Thespeon(data.path(), "validate --model Ada --text Hi");
  EXPECT_EQ(missing_language.exit_code, 1);
  EXPECT_NE(missing_language.err.find("No compatible language module"),
            std::string::npos)
      << missing_language.err;

  AddLanguage("2.json");
  const auto valid =
      Thespeon(data.path(), "validate --model Ada --text Hi --emotion joy");
  EXPECT_EQ(valid.exit_code, 0) << valid.err;
  EXPECT_EQ(valid.out, "OK: c-Ada  \"Ada\"  XL  v2.1.1\n");

  const auto unknown =
      Thespeon(data.path(), "validate --model Ada --text Hi --emotion Joyy");
  EXPECT_EQ(unknown.exit_code, 1);
  EXPECT_TRUE(unknown.out.empty()) << unknown.out;
  EXPECT_NE(unknown.err.find("Character \"Ada\" has no emotion \"Joyy\""),
            std::string::npos)
      << unknown.err;
}

}  // namespace
}  // namespace thespeon
