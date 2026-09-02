#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "etg/eni.h"

namespace mm::node {

class DeviceManager;

/// @brief What an ENI needs that no device can answer for.
///
/// Everything else is read off the bus. These three come from the process the master runs in: the
/// name it calls itself, the interface its frames leave by, and the period its loop runs at. The
/// composition root knows all three; @c DeviceManager knows none of them.
struct EniCollectorOptions {
  std::string masterName = "Motion Master";  ///< Written to @c Master/Info/Name.
  std::vector<std::uint8_t> sourceMac;  ///< Source MAC of the cyclic frames; exactly six bytes.
  std::uint32_t cycleTimeUs = 0;  ///< Cycle period in microseconds; 0 omits @c Cyclic/CycleTime.
};

/// @brief A collected network, and what could not be read while collecting it.
///
/// A warning never fails the collection. A device whose SII will not read still belongs in the
/// document with everything else about it intact, because the missing part is one optional element
/// and a master that never learns the port layout still brings the bus up.
struct EniCollection {
  mm::etg::EniNetwork network;        ///< Ready for @c mm::etg::writeEni.
  std::vector<std::string> warnings;  ///< One per part that was asked for and not answered.
};

/// @brief Reads the live bus and builds an ENI network from it.
///
/// The document describes the bus as this master has configured it, which is why it needs the bus
/// configured: FMMUs and logical addresses come into being at the SAFE-OP transition, and before
/// that there is no mapping to describe. So a bus in PRE-OP is refused rather than guessed at.
///
/// **This drives the bus.** Each device's SII is read for its port layout and bootstrap mailbox,
/// and its PDO assignment is read over CoE, so the call costs one SII read and a short burst of
/// SDO uploads per device. It is an export action, not an accessor.
///
/// Distributed clocks are left out. The @c DC element needs an @c AssignActivate word, and a
/// SOMANET drive does not carry the SII category that holds one, so it would have to come from the
/// device's ESI. The generated configuration therefore runs in free-run, which is how Motion Master
/// runs the bus itself.
///
/// @param manager  The device manager, with a published process image.
/// @param options  What the bus cannot answer for.
/// @return The collected network, or an error when the bus has no image, has no devices, or when
///         @p options is incomplete.
std::expected<EniCollection, std::string> collectEni(DeviceManager& manager,
                                                     const EniCollectorOptions& options);

}  // namespace mm::node
