#include "core/base64.h"

#include <openssl/evp.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::core {

namespace {

/// EVP_ENCODE_CTX is opaque and heap-allocated, so it gets the same unique_ptr-with-deleter
/// treatment the certificate code gives its OpenSSL handles.
struct EncodeContextDeleter {
  void operator()(EVP_ENCODE_CTX* ctx) const { EVP_ENCODE_CTX_free(ctx); }
};
using EncodeContext = std::unique_ptr<EVP_ENCODE_CTX, EncodeContextDeleter>;

}  // namespace

std::string base64Encode(std::span<const uint8_t> bytes) {
  if (bytes.empty()) {
    return {};
  }
  EncodeContext ctx(EVP_ENCODE_CTX_new());
  if (!ctx) {
    return {};
  }
  EVP_EncodeInit(ctx.get());

  // EVP_EncodeUpdate inserts a newline every 64 characters and writes a NUL terminator, so the
  // output buffer must allow for both: 4 characters per 3 input bytes, a newline per 64 of those,
  // and one terminator. The newlines are stripped below — this is one JSON string value, not a PEM
  // block — but the buffer still has to be large enough for OpenSSL to write them.
  const std::size_t quanta = (bytes.size() + 2) / 3;
  std::string buffer(quanta * 4 + quanta / 16 + 2, '\0');

  int written = 0;
  auto* out = reinterpret_cast<unsigned char*>(buffer.data());
  if (EVP_EncodeUpdate(ctx.get(), out, &written, bytes.data(), static_cast<int>(bytes.size())) !=
      1) {
    return {};
  }
  int finalWritten = 0;
  EVP_EncodeFinal(ctx.get(), out + written, &finalWritten);
  buffer.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalWritten));

  std::erase(buffer, '\n');
  return buffer;
}

std::expected<std::vector<uint8_t>, std::string> base64Decode(std::string_view text) {
  EncodeContext ctx(EVP_ENCODE_CTX_new());
  if (!ctx) {
    return std::unexpected("base64: could not allocate a decoding context");
  }
  EVP_DecodeInit(ctx.get());

  // Decoding never expands: three bytes out per four characters in, and OpenSSL writes whole
  // quanta, so the input length is a safe upper bound. The vector is shrunk to what was written.
  std::vector<uint8_t> out(text.size() / 4 * 3 + 3, 0);

  int written = 0;
  const int update = EVP_DecodeUpdate(ctx.get(), out.data(), &written,
                                      reinterpret_cast<const unsigned char*>(text.data()),
                                      static_cast<int>(text.size()));
  if (update < 0) {
    return std::unexpected("base64: invalid character or malformed input");
  }
  int finalWritten = 0;
  if (EVP_DecodeFinal(ctx.get(), out.data() + written, &finalWritten) != 1) {
    return std::unexpected("base64: truncated input (length is not a multiple of four)");
  }
  out.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalWritten));
  return out;
}

}  // namespace mm::core
