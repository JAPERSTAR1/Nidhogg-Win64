#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps4::net {
// WinHTTP is part of Windows 10. One receive and one send may run concurrently.
// No network calls on the simulation/render thread; bounded queues fail closed.
class WebSocketRelay {
public:
    ~WebSocketRelay();
    void Start(const std::string& url, const std::string& room, const std::string& token);
    void Stop();
    bool Send(const std::vector<uint8_t>& packet);
    bool Receive(std::vector<uint8_t>& packet);
    bool Connected() const { return connected_.load(); }
    std::string Error();
private:
    void Run(std::string url, std::string room, std::string token);
    std::atomic<bool> stop_{true}, connected_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> tx_, rx_;
    std::string error_;
    void* socket_ = nullptr;
    std::thread reader_;
};
}
