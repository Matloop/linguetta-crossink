#include <gtest/gtest.h>

#include "Blend.h"

using namespace linguetta;

TEST(BlendHref, ParsesTheSpecExample) {
  BlendLink l;
  ASSERT_TRUE(parseBlendHref("#lb:il mare|o mar|masculino", l));
  EXPECT_EQ(l.lemma, "il mare");
  EXPECT_EQ(l.translation, "o mar");
  EXPECT_EQ(l.note, "masculino");
  ASSERT_TRUE(parseBlendHref("#lb:sorridere|sorrir", l));
  EXPECT_EQ(l.note, "");
  ASSERT_TRUE(parseBlendHref("#lb:sorridere|sorrir|", l));
  EXPECT_EQ(l.note, "");
}

TEST(BlendHref, DecodesEscapes) {
  BlendLink l;
  ASSERT_TRUE(parseBlendHref("#lb:a%7Cb|50%25 off|x%7cy", l));
  EXPECT_EQ(l.lemma, "a|b");
  EXPECT_EQ(l.translation, "50% off");
  EXPECT_EQ(l.note, "x|y");
  ASSERT_TRUE(parseBlendHref("#lb:caff\xC3\xA8|caf\xC3\xA9|%", l));
  EXPECT_EQ(l.note, "%");
}

TEST(BlendHref, RejectsInvalid) {
  BlendLink l;
  EXPECT_FALSE(parseBlendHref("#note1", l));
  EXPECT_FALSE(parseBlendHref("#lb:|o mar", l));
  EXPECT_FALSE(parseBlendHref("#lb:mare|", l));
  EXPECT_FALSE(parseBlendHref("#lb:mare", l));
  EXPECT_FALSE(parseBlendHref("#lb:a|b|c|d", l));
  EXPECT_FALSE(parseBlendHref(nullptr, l));
  const std::string longHref = "#lb:" + std::string(300, 'a') + "|b";
  EXPECT_FALSE(parseBlendHref(longHref.c_str(), l));
}

namespace {
std::vector<PageWord> page(std::initializer_list<const char*> ws, std::initializer_list<int> link) {
  std::vector<PageWord> out;
  for (const char* w : ws) {
    PageWord p;
    p.text = w;
    out.push_back(p);
  }
  for (int i : link) out[i].inLink = true;
  return out;
}
}  // namespace

TEST(BlendContext, SentenceAroundTheLink) {
  auto w = page({"fim.", "Ela", "ficou", "olhando", "para", "il", "mare", "e", "come\xC3\xA7ou", "a", "sorrir.", "Depois"},
                {5, 6});
  const LinkContext c = extractLinkContext(w);
  EXPECT_EQ(c.form, "il mare");
  EXPECT_EQ(c.sentence, "Ela ficou olhando para il mare e come\xC3\xA7ou a sorrir.");
}

TEST(BlendContext, StopsAtParagraphsAndJoinsHyphenation) {
  auto w = page({"T\xC3\xADtulo", "Era", "uma", "bel-", "la", "manh\xC3\xA3\xE2\x80\xA6\xC2\xBB", "Fim"}, {3, 4});
  w[1].paragraphStart = true;
  w[3].hyphenated = true;
  w[6].paragraphStart = true;
  const LinkContext c = extractLinkContext(w);
  EXPECT_EQ(c.form, "bella");
  EXPECT_EQ(c.sentence, "Era uma bella manh\xC3\xA3\xE2\x80\xA6\xC2\xBB");
}

TEST(BlendContext, PunctuationWithoutSpace) {
  auto w = page({"Disse", "\xE2\x80\x9C", "ciao", "\xE2\x80\x9D", "e", "saiu."}, {2});
  w[2].spaceBefore = false;
  w[3].spaceBefore = false;
  const LinkContext c = extractLinkContext(w);
  EXPECT_EQ(c.form, "ciao");
  EXPECT_EQ(c.sentence, "Disse \xE2\x80\x9C" "ciao\xE2\x80\x9D e saiu.");
  EXPECT_EQ(extractLinkContext(page({"a"}, {})).form, "");
}
