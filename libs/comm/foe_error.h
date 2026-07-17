#pragma once

// ── Currently unused: a reference implementation, not yet wired into any surface. ──────────────
//
// Today every FieldbusDriver operation — readFile/writeFile included — reports failure as
// std::expected<T, std::string>. That is the deliberate default (see CLAUDE.md, the no-exceptions
// mandate): std::string is right everywhere a caller only logs, forwards, or shows the error, which
// is every FoE caller in the tree right now (the HTTP handlers just put the text in a 500 body).
//
// This header exists so the *promotion path* is concrete rather than hypothetical. The moment a
// caller must **branch** on why a FoE transfer failed — the canonical case is a firmware flasher
// that retries a transient failure (packet desync, or a slave still warming up after BOOT) but
// aborts immediately on a permanent one (no such file, undersized buffer) — string-matching the
// message to decide control flow is the smell that says that surface has earned a structured error.
// At that point readFile/writeFile change their error type from std::string to FoeError; because
// FoeError keeps a string face (operator<<, .message, .what()), the forwarding callers that only
// display it change by at most one word (.error() → .error().message) and nothing ripples further.
//
// This is the same design split the wider ecosystem settled on, and the mandate is a deliberate
// pick of one side of it:
//   • Rust codified it as two libraries — anyhow::Error (a message you propagate and Display) for
//     leaf/application code, thiserror (typed enums you `match` on) for a library whose callers
//     branch. "std::string by default, FoeError where you branch" is that rule verbatim.
//   • Go is the same shape: fmt.Errorf("...: %w", err) (string + wrap) almost everywhere, and only
//     sentinel errors + errors.Is/errors.As at the specific call site that must distinguish a
//     cause.
// The rejected third option is a single generic error type swept across the whole codebase: its
// enum is either too coarse to serve the branching caller (who then string-matches anyway) or a
// grab-bag union of every layer's failure modes (coupling with no abstraction win). So the rule is
// "typed per surface, or a string" with nothing in between — and this type is named for its surface
// (FoeError, not OpError) so both the type and the coupling to it stay local to FoE callers. See
// NEXTGEN.md, Session 2026-07-17.
//
// The `retry` tag is the one axis worth sharing across surfaces (an SDO error would carry the same
// Transient/Permanent distinction), so it rides *alongside* the FoE-specific kind rather than being
// folded into it — the absl::Status insight (a universal code + operation-specific detail), sized
// down to the one distinction callers here actually act on.
// ──────────────────────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <format>
#include <ostream>
#include <string>
#include <string_view>

namespace mm::comm {

/// @brief Retry disposition of a failed operation: whether re-issuing the identical call could
/// plausibly succeed. This is the one axis callers branch on uniformly — a firmware flasher retries
/// @c Transient failures and aborts on @c Permanent ones — so it rides alongside the FoE-specific
/// kind rather than being folded into it.
enum class Retry {
  Transient,  ///< A fresh attempt may succeed (protocol desync, slave not yet ready after BOOT).
  Permanent,  ///< The identical call will fail the same way (no such file, undersized buffer).
};

/// @brief Specific File-over-EtherCAT failure kind. Transport-agnostic: a SOEM driver decodes it
/// from a negated @c ec_err_type, a future SPoE driver from its own protocol codes.
enum class FoeErrorKind {
  NoResponse,      ///< The slave answered nothing (work counter 0 — timeout or not ready).
  FileNotFound,    ///< Firmware recognises no such filename.
  BufferTooSmall,  ///< The slave's buffer cannot hold the requested transfer.
  PacketMismatch,  ///< Packet-number desync mid-transfer.
  Protocol,        ///< Generic FoE-protocol error with no finer classification.
};

/// @brief Short human-readable reason for a FoE kind (no surrounding punctuation).
inline std::string_view foeReason(FoeErrorKind kind) {
  switch (kind) {
    case FoeErrorKind::NoResponse:
      return "no response";
    case FoeErrorKind::FileNotFound:
      return "file not found";
    case FoeErrorKind::BufferTooSmall:
      return "buffer too small";
    case FoeErrorKind::PacketMismatch:
      return "packet number mismatch";
    case FoeErrorKind::Protocol:
      return "FoE error";
  }
  return "FoE error";
}

/// @brief Retry disposition implied by a FoE kind. A missing file or an undersized buffer is a
/// fixed condition that will recur identically; a desync or a not-yet-ready slave (the BOOT
/// bootloader warm-up path) is worth another attempt.
inline Retry foeRetry(FoeErrorKind kind) {
  switch (kind) {
    case FoeErrorKind::FileNotFound:
    case FoeErrorKind::BufferTooSmall:
      return Retry::Permanent;
    case FoeErrorKind::NoResponse:
    case FoeErrorKind::PacketMismatch:
    case FoeErrorKind::Protocol:
      return Retry::Transient;
  }
  return Retry::Transient;
}

/// @brief A structured FoE failure. String-like where a caller only forwards it (@c operator<<,
/// @c .message, @c what()); branchable where a caller must react (@c kind, @c retry).
struct FoeError {
  FoeErrorKind kind = FoeErrorKind::Protocol;
  Retry retry = Retry::Transient;
  std::string message;

  /// @brief Exception-shaped accessor for the message, for call sites that prefer @c .what().
  const std::string& what() const { return message; }
};

/// @brief Streams the message, so `ASSERT_TRUE(r) << r.error()` and spdlog `{}` work unchanged.
inline std::ostream& operator<<(std::ostream& os, const FoeError& e) { return os << e.message; }

/// @brief Builds a fully-formed FoeError from a decoded @p kind plus call context. Keeps the
/// message wording identical to the pre-promotion string ("<op> slave N '<file>' failed
/// (<reason>)") so nothing downstream that logs or displays it changes shape.
inline FoeError makeFoeError(FoeErrorKind kind, std::string_view op, uint16_t slave,
                             std::string_view filename) {
  return {kind, foeRetry(kind),
          std::format("{} slave {} '{}' failed ({})", op, slave, filename, foeReason(kind))};
}

}  // namespace mm::comm
