#include "node/kuebler_registers.h"

#include <nlohmann/json.hpp>

namespace mm::node::somanet {

namespace {

const char* toString(KueblerAccess access) {
  switch (access) {
    case KueblerAccess::kReadOnly:
      return "ro";
    case KueblerAccess::kWriteOnly:
      return "wo";
    case KueblerAccess::kReadWrite:
      return "rw";
  }
  return "unknown";
}

const char* toString(KueblerFormat format) {
  switch (format) {
    case KueblerFormat::kUnsigned:
      return "unsigned";
    case KueblerFormat::kSigned:
      return "signed";
    case KueblerFormat::kBitField:
      return "bitField";
    case KueblerFormat::kSignedHalves:
      return "signedHalves";
  }
  return "unknown";
}

}  // namespace

void to_json(nlohmann::json& j, const KueblerRegister& r) {
  j = nlohmann::json{
      {"address", r.address},
      {"bits", r.bits},
      {"name", r.name},
      {"access", toString(r.access)},
      {"implemented", r.implemented},
      {"format", toString(r.format)},
      {"definition", r.definition},
      // Derived, and on the wire because a client would otherwise have to know that the command's
      // length byte caps at 4: this says outright whether one command can carry the register.
      {"readableInOneCommand", r.bits / 8 <= kMaxKueblerRegisterBytes},
  };
}

}  // namespace mm::node::somanet
