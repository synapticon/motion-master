#include "cert_info.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// A self-signed fixture certificate carrying the same two subjectAltName entries the shipped
// certificate does: the loopback name and the off-loopback wildcard. Only the public certificate
// is here — no private key — so it authenticates nothing and is inert outside this test. Issued
// with a 100-year validity so the test never starts failing on the calendar.
constexpr char kTwoSanCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIICBDCCAaugAwIBAgIULTRO2qROpJMXwgnn8K8xGPA+8B0wCgYIKoZIzj0EAwIw
LTErMCkGA1UEAwwibG9jYWwubW90aW9uLW1hc3Rlci5zeW5hcHRpY29uLmNvbTAg
Fw0yNjA3MzExMTQ0MjhaGA8yMTI2MDcwNzExNDQyOFowLTErMCkGA1UEAwwibG9j
YWwubW90aW9uLW1hc3Rlci5zeW5hcHRpY29uLmNvbTBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABMFDReUUoqLMfy5aGrk9dcMmP8wuRQvWrGEWaQt928JMPLwTUmNA
JdYcliM9lpEJTzuBPNxa6W/stp5JEqCfEeijgaYwgaMwHQYDVR0OBBYEFJO1GlwW
U1thCONq3BHmITHV0AIyMB8GA1UdIwQYMBaAFJO1GlwWU1thCONq3BHmITHV0AIy
MA8GA1UdEwEB/wQFMAMBAf8wUAYDVR0RBEkwR4IibG9jYWwubW90aW9uLW1hc3Rl
ci5zeW5hcHRpY29uLmNvbYIhKi5pcC5tb3Rpb24tbWFzdGVyLnN5bmFwdGljb24u
Y29tMAoGCCqGSM49BAMCA0cAMEQCIAWYYNNasepr9i/UTIMWQICPRrOD+LHc0Jkg
dHudEBLsAiBTlLL3rGRmaPuqelNX89pPb0brT48UfqpFTd+vKpd88w==
-----END CERTIFICATE-----
)";

// Writes the fixture PEM to a uniquely-named file and removes it when the test ends. readCertInfo
// takes a path, so the fixture has to reach the filesystem.
class TempCertFile {
 public:
  explicit TempCertFile(const std::string& pem)
      : path_(std::filesystem::temp_directory_path() /
              ("mm_cert_info_test_" +
               std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".pem")) {
    std::ofstream f{path_, std::ios::binary | std::ios::trunc};
    f << pem;
  }
  ~TempCertFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempCertFile(const TempCertFile&) = delete;
  TempCertFile& operator=(const TempCertFile&) = delete;

  std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST(CertInfoTest, ReadsSubjectAndAltNames) {
  const TempCertFile cert{kTwoSanCertPem};
  auto info = mm::readCertInfo(cert.path());
  ASSERT_TRUE(info.has_value()) << info.error();

  EXPECT_EQ(info->subject, "local.motion-master.synapticon.com");
  // Both deployments are covered by one certificate: the loopback name a same-machine browser
  // uses, and the wildcard an off-loopback host is reached by.
  EXPECT_EQ(info->dnsNames, (std::vector<std::string>{"local.motion-master.synapticon.com",
                                                      "*.ip.motion-master.synapticon.com"}));
}

TEST(CertInfoTest, ParsesValidityWindow) {
  const TempCertFile cert{kTwoSanCertPem};
  auto info = mm::readCertInfo(cert.path());
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_LT(info->notBefore, info->notAfter);
  EXPECT_EQ(info->chain.size(), 1u);
}

TEST(CertInfoTest, MissingFileReportsError) {
  EXPECT_FALSE(mm::readCertInfo("/nonexistent/motion-master/cert.pem").has_value());
}

TEST(CertInfoTest, GarbageFileReportsError) {
  const TempCertFile cert{"not a certificate\n"};
  EXPECT_FALSE(mm::readCertInfo(cert.path()).has_value());
}
