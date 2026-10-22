#include <CLI/CLI.hpp>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "capture.h"
#include "capture/pcap.h"
#include "capture/summary.h"

// Records every EtherCAT frame on an adapter for a number of seconds, writes them to a pcap file,
// prints a summary of how late each frame came back, and exits. With --summarise it reads a pcap
// instead of capturing, so a file from tcpdump on another host gets the same summary.
int main(int argc, char** argv) {
  CLI::App app{"Records EtherCAT frames and says how late they come back", "motion-master-capture"};
  std::string adapter;
  int seconds = 30;
  std::string out;
  std::string summarise;
  app.add_option("-i,--ifname", adapter, "The adapter the EtherCAT bus is on");
  app.add_option("-s,--seconds", seconds, "How long to capture")->check(CLI::Range(1, 3600));
  app.add_option("-o,--out", out, "The pcap file to write");
  app.add_option("--summarise", summarise, "Read this pcap file instead of capturing");
  CLI11_PARSE(app, argc, argv);

  if (!summarise.empty()) {
    const auto records = mm::capture::readPcap(summarise);
    if (!records) {
      std::cerr << records.error() << "\n";
      return 1;
    }
    std::cout << nlohmann::json(mm::capture::summarise(*records)).dump(2) << "\n";
    return 0;
  }
  if (adapter.empty() || out.empty()) {
    std::cerr << "--ifname and --out are needed to capture, or --summarise to read a file\n";
    return 2;
  }

  mm::capture::PcapWriter writer;
  if (auto r = writer.open(out); !r) {
    std::cerr << r.error() << "\n";
    return 1;
  }
  // Kept in memory for the summary as well as written, so the file is complete whatever happens
  // to the summary, and the summary needs no second read of it.
  std::vector<mm::capture::Record> records;
  std::string writeError;
  const auto sink = [&](int64_t timestampNs, std::span<const uint8_t> bytes) {
    if (auto r = writer.write(timestampNs, bytes); !r && writeError.empty()) {
      writeError = r.error();
    }
    records.push_back(mm::capture::Record{timestampNs, {bytes.begin(), bytes.end()}});
  };
  std::cerr << "capturing on " << adapter << " for " << seconds << " s into " << out << "\n";
  if (auto r = mm::capture::captureFrames(adapter, std::chrono::seconds{seconds}, sink); !r) {
    std::cerr << r.error() << "\n";
    return 1;
  }
  writer.close();
  if (!writeError.empty()) {
    std::cerr << writeError << "\n";
    return 1;
  }
  std::cout << nlohmann::json(mm::capture::summarise(records)).dump(2) << "\n";
  return 0;
}
