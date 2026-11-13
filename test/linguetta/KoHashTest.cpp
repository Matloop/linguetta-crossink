// The work id (contract section 9) is upstream's KOReaderDocumentId::calculate, unchanged.
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <gtest/gtest.h>

#include "TestUtil.h"

TEST(KoHash, MatchesContractVectors) {
  JsonDocument doc = loadJsonFixture("kosync-hash-vectors.json");
  JsonArrayConst cases = doc["cases"];
  ASSERT_GT(cases.size(), 5u);
  for (JsonObjectConst c : cases) {
    const size_t size = c["size"];
    std::string bytes(size, '\0');
    for (size_t k = 0; k < size; k++) bytes[k] = static_cast<char>((k * 31 + 7) & 0xff);
    fake::reset();
    fake::add("/books/b.epub", bytes);
    EXPECT_EQ(KOReaderDocumentId::calculate("/books/b.epub"), c["hash"].as<std::string>()) << "size " << size;
  }
}
