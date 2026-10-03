#include "comm/vendor_ids.h"

#include <gtest/gtest.h>

namespace mm::comm {
namespace {

TEST(IsSomanetDevice, AcceptsSynapticon) { EXPECT_TRUE(isSomanetDevice(kSynapticonVendorId)); }

// Sensodrive devices run SOMANET firmware under their own vendor ID.
TEST(IsSomanetDevice, AcceptsSensodrive) { EXPECT_TRUE(isSomanetDevice(kSensodriveVendorId)); }

TEST(IsSomanetDevice, RejectsAnotherVendor) {
  EXPECT_FALSE(isSomanetDevice(0x00000002));
  EXPECT_FALSE(isSomanetDevice(0x00000000));
}

}  // namespace
}  // namespace mm::comm
