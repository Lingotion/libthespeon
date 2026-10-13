// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/utils/utf8_path.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "core/module/catalog.h"
#include "core/package/importer.h"
#include "test_support.h"

namespace thespeon {
namespace {

// Outside every single-byte Windows code page, so path.string() would throw.
const std::string kName = "\xC3\x85sa \xE6\xB5\x8B\xE8\xAF\x95";

TEST(Utf8PathTest, RoundTripsNonAsciiNames) {
  const auto path = PathFromUtf8(kName) / "voice.lingotion";
  EXPECT_EQ(PathToUtf8(path.parent_path()), kName);
  EXPECT_EQ(PathToUtf8(path.filename()), "voice.lingotion");
#ifdef _WIN32
  EXPECT_EQ(path.parent_path().wstring(), L"Åsa 测试");
#endif
}

class NonAsciiDirectoryTest : public ::testing::Test {
 protected:
  std::filesystem::path Root() const {
    return directory.path() / PathFromUtf8(kName);
  }

  test::TempDirectory directory;
};

TEST_F(NonAsciiDirectoryTest, ImportHandsLibzipTheUtf8Path) {
  const auto pack = Root() / "voice.lingotion";
  test::WriteFile(pack, "not a zip");
  try {
    ImportPack(pack, Root() / "data");
    FAIL() << "Import accepted a non-ZIP pack";
  } catch (const std::runtime_error& error) {
    // A mangled path would make libzip report a missing file instead.
    EXPECT_EQ(std::string(error.what()),
              "Cannot open " + PathToUtf8(pack) +
                  " as a ZIP archive: Not a zip archive");
  }
}

TEST_F(NonAsciiDirectoryTest, CatalogNamesProblemsInUtf8) {
  test::WriteFile(Root() / "configs" / PathFromUtf8(kName + ".json"), "{");
  const auto catalog = Catalog::Load(Root());
  ASSERT_EQ(catalog.problems().size(), 1u);
  EXPECT_EQ(catalog.problems()[0].rfind(kName + ".json: ", 0), 0u);
}

}  // namespace
}  // namespace thespeon
