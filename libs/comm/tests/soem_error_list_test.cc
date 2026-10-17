#include <gtest/gtest.h>
#include <soem/soem.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

// These tests cover the SOEM port's patch to the error list: the lock, and ecx_popslaveerror. A
// failed transfer must find the reason of its own slave and never the reason of another slave, also
// when the work on two slaves runs on two threads.
//
// No test opens a NIC. ecx_init would create the mutex, but it also opens a raw socket, so each
// test creates the mutex itself. The context is far too large for the stack.
struct ErrorList {
  std::unique_ptr<ecx_contextt> ctx = std::make_unique<ecx_contextt>();

  ErrorList() { ctx->elistmutex = static_cast<osal_mutext*>(osal_mutex_create()); }
  ~ErrorList() { osal_mutex_destroy(ctx->elistmutex); }
  ErrorList(const ErrorList&) = delete;
  ErrorList& operator=(const ErrorList&) = delete;

  // The abort code tells two entries of one slave apart.
  void push(uint16_t slave, int32_t abortCode) {
    ec_errort err{};
    err.Slave = slave;
    err.Etype = EC_ERR_TYPE_SDO_ERROR;
    err.AbortCode = abortCode;
    ecx_pusherror(ctx.get(), &err);
  }
};

TEST(SoemErrorList, PopsOnlyTheNamedSlave) {
  ErrorList list;
  list.push(1, 11);
  list.push(2, 21);
  list.push(1, 12);

  ec_errort err{};
  ASSERT_TRUE(ecx_popslaveerror(list.ctx.get(), 2, &err));
  EXPECT_EQ(err.Slave, 2);
  EXPECT_EQ(err.AbortCode, 21);
  EXPECT_FALSE(ecx_popslaveerror(list.ctx.get(), 2, &err));

  ASSERT_TRUE(ecx_popslaveerror(list.ctx.get(), 1, &err));
  EXPECT_EQ(err.AbortCode, 11);
  ASSERT_TRUE(ecx_popslaveerror(list.ctx.get(), 1, &err));
  EXPECT_EQ(err.AbortCode, 12);
  EXPECT_FALSE(ecx_popslaveerror(list.ctx.get(), 1, &err));

  EXPECT_FALSE(ecx_iserror(list.ctx.get()));
  EXPECT_FALSE(list.ctx->ecaterror);
}

// A pop from the middle closes the gap. The entries of the other slaves must keep their order,
// because ecx_poperror still returns the oldest entry.
TEST(SoemErrorList, KeepsTheOrderOfTheOtherSlaves) {
  ErrorList list;
  list.push(1, 11);
  list.push(2, 21);
  list.push(1, 12);
  list.push(2, 22);

  ec_errort err{};
  ASSERT_TRUE(ecx_popslaveerror(list.ctx.get(), 1, &err));
  EXPECT_EQ(err.AbortCode, 11);
  EXPECT_TRUE(list.ctx->ecaterror);

  for (const int32_t expected : {21, 12, 22}) {
    ASSERT_TRUE(ecx_poperror(list.ctx.get(), &err));
    EXPECT_EQ(err.AbortCode, expected);
  }
  EXPECT_FALSE(ecx_poperror(list.ctx.get(), &err));
}

// The list is a ring of EC_MAXELIST + 1 slots. Entries that wrap past the last slot must still be
// found, and the gap must close across the wrap.
TEST(SoemErrorList, FindsEntriesAcrossTheWrap) {
  ErrorList list;
  ec_errort err{};
  for (int i = 0; i < EC_MAXELIST - 1; ++i) {
    list.push(9, 0);
    ASSERT_TRUE(ecx_poperror(list.ctx.get(), &err));
  }
  for (int32_t i = 0; i < 6; ++i) {
    list.push(static_cast<uint16_t>(1 + (i % 2)), i);
  }

  for (const int32_t expected : {1, 3, 5}) {
    ASSERT_TRUE(ecx_popslaveerror(list.ctx.get(), 2, &err));
    EXPECT_EQ(err.AbortCode, expected);
  }
  for (const int32_t expected : {0, 2, 4}) {
    ASSERT_TRUE(ecx_poperror(list.ctx.get(), &err));
    EXPECT_EQ(err.AbortCode, expected);
  }
  EXPECT_FALSE(ecx_iserror(list.ctx.get()));
}

// Two threads stand in for the transfers of two slaves. Each pushes an entry and pops its own. A
// pop that returns nothing, or the entry of the other slave, is a misattributed reason.
TEST(SoemErrorList, TwoThreadsEachFindTheirOwnSlave) {
  ErrorList list;
  constexpr int kRounds = 20000;
  std::atomic<int> misses{0};

  auto transfer = [&](uint16_t slave) {
    for (int32_t round = 0; round < kRounds; ++round) {
      list.push(slave, round);
      ec_errort err{};
      if (!ecx_popslaveerror(list.ctx.get(), slave, &err) || err.Slave != slave ||
          err.AbortCode != round) {
        misses.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  {
    std::jthread first(transfer, uint16_t{1});
    std::jthread second(transfer, uint16_t{2});
  }

  EXPECT_EQ(misses.load(), 0);
  EXPECT_FALSE(ecx_iserror(list.ctx.get()));
}

}  // namespace
