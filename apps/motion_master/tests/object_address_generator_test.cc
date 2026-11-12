#include "object_address_generator.h"

#include <gtest/gtest.h>

namespace {

using mm::clientIdentifier;

TEST(ClientIdentifier, DropsTheKAndWritesOneWordInCapitals) {
  EXPECT_EQ(clientIdentifier("kStatusword"), "STATUSWORD");
}

TEST(ClientIdentifier, PutsAnUnderscoreBetweenWords) {
  EXPECT_EQ(clientIdentifier("kCoreTemperatureMeasuredTemperature"),
            "CORE_TEMPERATURE_MEASURED_TEMPERATURE");
}

TEST(ClientIdentifier, KeepsARunOfCapitalsAsOneWord) {
  EXPECT_EQ(clientIdentifier("kIdentityObjectVendorID"), "IDENTITY_OBJECT_VENDOR_ID");
  EXPECT_EQ(clientIdentifier("kOSCommandCommand"), "OS_COMMAND_COMMAND");
}

TEST(ClientIdentifier, PutsAnUnderscoreBeforeAndAfterANumber) {
  EXPECT_EQ(clientIdentifier("kEncoder1Configuration"), "ENCODER_1_CONFIGURATION");
  EXPECT_EQ(clientIdentifier("kStoreParametersSubIndex001"), "STORE_PARAMETERS_SUB_INDEX_001");
}

// An ESI name in capitals has no word boundaries left in the C++ name, so the client name has none
// either. The client name is the C++ name in capitals, whatever the C++ name is.
TEST(ClientIdentifier, FollowsTheCxxNameWhenTheEsiNameIsInCapitals) {
  EXPECT_EQ(clientIdentifier("kCOBIDSYNC"), "COBIDSYNC");
}

}  // namespace
