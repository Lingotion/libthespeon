// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "core/module/module.h"

#include <gtest/gtest.h>

namespace thespeon {
namespace {

TEST(ParseVersionString, AcceptsPlainAndPrefixedVersions) {
  EXPECT_EQ(ParseVersionString("3.0.1"), (Version{3, 0, 1}));
  EXPECT_EQ(ParseVersionString("v10.20.30"), (Version{10, 20, 30}));
}

TEST(ParseVersionString, RejectsMalformedVersions) {
  for (const char* value :
       {"", "v", "3", "3.0", "3.0.1x", "3,0,1", "3.0.1.4", "-1.0.0", "a.b.c"}) {
    EXPECT_FALSE(ParseVersionString(value).IsValid()) << value;
  }
}

TEST(Version, DefaultIsInvalid) { EXPECT_FALSE(Version{}.IsValid()); }

TEST(Version, OrdersByMajorThenMinorThenPatch) {
  EXPECT_LT((Version{1, 9, 9}), (Version{2, 0, 0}));
  EXPECT_LT((Version{2, 0, 9}), (Version{2, 1, 0}));
  EXPECT_LT((Version{2, 1, 0}), (Version{2, 1, 1}));
  EXPECT_GE((Version{2, 1, 1}), (Version{2, 1, 1}));
}

TEST(Version, FormatsWithOptionalPrefix) {
  EXPECT_EQ((Version{3, 0, 1}).ToString(), "v3.0.1");
  EXPECT_EQ((Version{3, 0, 1}).ToString(false), "3.0.1");
}

TEST(ModuleTypeRank, RanksSizesCaseInsensitively) {
  EXPECT_EQ(ModuleTypeRank("XS"), 1);
  EXPECT_EQ(ModuleTypeRank("s"), 2);
  EXPECT_EQ(ModuleTypeRank("M"), 3);
  EXPECT_EQ(ModuleTypeRank("l"), 4);
  EXPECT_EQ(ModuleTypeRank("Xl"), 5);
  EXPECT_EQ(ModuleTypeRank(""), 0);
  EXPECT_EQ(ModuleTypeRank("XXL"), 0);
}

TEST(ModuleTypeName, MapsConfigQualitiesToSizes) {
  EXPECT_EQ(ModuleTypeName("ultralow"), "XS");
  EXPECT_EQ(ModuleTypeName("Low"), "S");
  EXPECT_EQ(ModuleTypeName("mid"), "M");
  EXPECT_EQ(ModuleTypeName("HIGH"), "L");
  EXPECT_EQ(ModuleTypeName("ultrahigh"), "XL");
}

TEST(ModuleTypeName, PassesUnknownQualitiesThrough) {
  EXPECT_EQ(ModuleTypeName("experimental"), "experimental");
}

TEST(Language, DisplayIncludesCountryWhenSet) {
  Language language;
  language.iso639_2 = "eng";
  EXPECT_EQ(language.Display(), "eng");
  language.country = "US";
  EXPECT_EQ(language.Display(), "eng-US");
}

TEST(Language, DisplayFullListsOnlySetFieldsInConfigOrder) {
  Language language;
  language.iso639_2 = "eng";
  language.country = "GB";
  language.name_in_english = "English";
  EXPECT_EQ(language.DisplayFull(),
            "iso639_2=eng iso3166_1=GB nameinenglish=English");
}

TEST(FileReference, ResolvesByHashAndExtension) {
  const FileReference reference{"abc123", "onnx"};
  EXPECT_EQ(reference.Resolve("bin"), std::filesystem::path("bin") / "abc123.onnx");
}

}  // namespace
}  // namespace thespeon
