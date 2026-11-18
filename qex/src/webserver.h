// webserver.h - dependency-free HTTP server hosting the query dashboard.
#pragma once

#include "engine.h"
#include <atomic>
#include <thread>

namespace qex {

class WebServer {
public:
    WebServer(Engine &engine, int port);
    ~WebServer();
    void start();
    void stop();
    int port() const { return port_; }

private:
    Engine &engine_;
    int port_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    void acceptLoop();
    void handle(int fd);
};

} // namespace qex
