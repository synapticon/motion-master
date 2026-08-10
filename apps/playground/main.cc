// clang-format off
// On Windows, <soem/soem.h> drags in the bundled WinPcap headers, which
// #define inline -> __inline. A C++ stdlib header included afterwards then trips
// MSVC's xkeycheck.h ("forbids macroizing the keyword inline"). Include the
// stdlib headers first so they are parsed before that macro exists.
#include <cerrno>
#include <cstring>
#include <print>

#include <soem/soem.h>
// clang-format on

namespace {

void list_adapters() {
  ec_adaptert* head = ec_find_adapters();
  std::println("Available adapters:");
  for (ec_adaptert* a = head; a != nullptr; a = a->next) {
    std::println("  {}  ({})", a->name, a->desc);
  }
  ec_free_adapters(head);
}

void init_devices(const char* ifname) {
  // Static storage avoids stack overflow — ecx_contextt holds EC_MAXSLAVE entries.
  static ecx_contextt ctx{};
  static uint8 iomap[4096]{};

  std::println("Initializing EtherCAT on {}...", ifname);

  if (!ecx_init(&ctx, ifname)) {
    // strerror's shared buffer needs a second thread to be a hazard, and this scratch binary has
    // none. mm::core::errnoMessage() is the answer in the shipped code; reaching for it here would
    // make playground link mm::core for one message.
    std::println("Failed to open socket on {}: {} (wrong interface name, or run as root)", ifname,
                 std::strerror(errno));  // NOLINT(concurrency-mt-unsafe)
    return;
  }

  int count = ecx_config_init(&ctx);
  if (count <= 0) {
    std::println("No slaves found");
    ecx_close(&ctx);
    return;
  }

  ecx_config_map_group(&ctx, iomap, 0);
  std::println("{} slave(s):", count);

  for (int i = 1; i <= count; i++) {
    std::println("  [{}] {}  man:{:08x}  id:{:08x}  rev:{:08x}", i, ctx.slavelist[i].name,
                 static_cast<uint32_t>(ctx.slavelist[i].eep_man),
                 static_cast<uint32_t>(ctx.slavelist[i].eep_id),
                 static_cast<uint32_t>(ctx.slavelist[i].eep_rev));
  }

  ecx_close(&ctx);
}

}  // namespace

// See the shipped main(): the standard library can still raise bad_alloc, and terminating is the
// right answer for a scratch binary.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
  if (argc < 2) {
    list_adapters();
    std::println("\nUsage: playground <ifname>");
    return 0;
  }
  init_devices(argv[1]);
  return 0;
}
