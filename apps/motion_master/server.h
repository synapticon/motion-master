#pragma once

#include <uwebsockets/App.h>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

class Server {
 public:
  struct Config {
    uint16_t port = 8443;
    std::string cert_file;
    std::string key_file;
    std::string version;
  };

  explicit Server(Config config);
  ~Server();

  void start();
  void stop();
  void broadcast(std::string json);

 private:
  struct WsData {};

  void run();

  Config config_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::atomic<uWS::Loop*> loop_{nullptr};
  std::atomic<us_listen_socket_t*> listen_token_{nullptr};
  std::unordered_set<uWS::WebSocket<true, true, WsData>*> connections_;
};
