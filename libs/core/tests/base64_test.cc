#include "core/base64.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using mm::core::base64Decode;
using mm::core::base64Encode;

std::vector<uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

// RFC 4648 §10 test vectors, which are also the three tail lengths: no padding, one '=', two '='.
TEST(Base64Test, EncodesTheRfcVectors) {
  EXPECT_EQ(base64Encode(bytesOf("")), "");
  EXPECT_EQ(base64Encode(bytesOf("f")), "Zg==");
  EXPECT_EQ(base64Encode(bytesOf("fo")), "Zm8=");
  EXPECT_EQ(base64Encode(bytesOf("foo")), "Zm9v");
  EXPECT_EQ(base64Encode(bytesOf("foob")), "Zm9vYg==");
  EXPECT_EQ(base64Encode(bytesOf("fooba")), "Zm9vYmE=");
  EXPECT_EQ(base64Encode(bytesOf("foobar")), "Zm9vYmFy");
}

TEST(Base64Test, DecodesTheRfcVectors) {
  EXPECT_EQ(base64Decode("").value(), bytesOf(""));
  EXPECT_EQ(base64Decode("Zg==").value(), bytesOf("f"));
  EXPECT_EQ(base64Decode("Zm8=").value(), bytesOf("fo"));
  EXPECT_EQ(base64Decode("Zm9v").value(), bytesOf("foo"));
  EXPECT_EQ(base64Decode("Zm9vYg==").value(), bytesOf("foob"));
  EXPECT_EQ(base64Decode("Zm9vYmE=").value(), bytesOf("fooba"));
  EXPECT_EQ(base64Decode("Zm9vYmFy").value(), bytesOf("foobar"));
}

// The property that actually matters for a firmware package: every byte value survives, at a size
// well past the encoder's internal 48-byte block and its 64-character line wrapping.
TEST(Base64Test, RoundTripsEveryByteValueAcrossManyBlocks) {
  std::vector<uint8_t> data(4096);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 256);
  }
  const std::string encoded = base64Encode(data);
  EXPECT_EQ(encoded.find('\n'), std::string::npos) << "the encoding must be a single line";

  auto decoded = base64Decode(encoded);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(*decoded, data);
}

// A payload that has been through a PEM-shaped tool arrives wrapped; that must still decode.
TEST(Base64Test, ToleratesLineBreaks) {
  auto decoded = base64Decode("Zm9v\nYmFy\n");
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(*decoded, bytesOf("foobar"));
}

TEST(Base64Test, RejectsCharactersOutsideTheAlphabet) {
  EXPECT_FALSE(base64Decode("Zm9v*mFy"));
  EXPECT_FALSE(base64Decode("Zm9vYmF!"));
}

TEST(Base64Test, RejectsATruncatedQuantum) {
  EXPECT_FALSE(base64Decode("Zm9vYmF"));
  EXPECT_FALSE(base64Decode("Z"));
}

}  // namespace
