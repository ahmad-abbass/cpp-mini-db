#include "minidb/command.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

class Socket {
public:
    explicit Socket(int value) : value_(value) {}
    ~Socket() { if (value_ >= 0) ::close(value_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return value_; }
    int release() { const int value = value_; value_ = -1; return value; }
private:
    int value_;
};

int number(const std::string& word, int minimum, int maximum) {
    int value = 0;
    auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != word.data() + word.size() || value < minimum || value > maximum) {
        throw std::invalid_argument("invalid numeric option: " + word);
    }
    return value;
}

void check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

bool send_all(int fd, const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void serve(int fd, minidb::Database& database) {
    minidb::CommandSession session(database);
    std::string line;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return;
        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                auto result = session.execute(line);
                line.clear();
                if (!send_all(fd, result.json + '\n') || result.quit) return;
            } else {
                if (line.size() == minidb::max_command_bytes) {
                    send_all(fd, "{\"ok\":false,\"code\":\"request_too_large\",\"error\":\"command exceeds 9 MiB\"}\n");
                    return;
                }
                line.push_back(buffer[i]);
            }
        }
    }
}

class WorkerPool {
public:
    WorkerPool(minidb::Database& database, std::size_t count)
        : database_(database), queue_limit_(count * 4), active_(count, -1) {
        try {
            for (std::size_t i = 0; i < count; ++i) workers_.emplace_back([this, i] { work(i); });
        } catch (...) { stop(); throw; }
    }
    ~WorkerPool() { stop(); }

    bool enqueue(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || pending_.size() >= queue_limit_) return false;
        pending_.push_back(fd);
        ready_.notify_one();
        return true;
    }

private:
    minidb::Database& database_;
    const std::size_t queue_limit_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<int> pending_;
    std::vector<int> active_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;

    void work(std::size_t worker) {
        for (;;) {
            int fd;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_) return;
                fd = pending_.front();
                pending_.pop_front();
                active_[worker] = fd;
            }
            try { serve(fd, database_); }
            catch (const std::exception& error) { std::cerr << "client error: " << error.what() << '\n'; }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_[worker] = -1;
                ::close(fd);
            }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (int fd : active_) if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
            for (int fd : pending_) ::close(fd);
            pending_.clear();
        }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: minidb_server DIRECTORY [--port 7070] [--workers 4] [--idle-timeout 30] [--buffered]\n";
        return 2;
    }
    try {
        int port = 7070;
        int workers = 4;
        int timeout = 30;
        bool durable = true;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--buffered") { durable = false; continue; }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[++i];
            if (option == "--port") port = number(value, 0, 65535);
            else if (option == "--workers") workers = number(value, 1, 32);
            else if (option == "--idle-timeout") timeout = number(value, 1, 300);
            else throw std::invalid_argument("unknown option: " + option);
        }
        std::signal(SIGTERM, request_stop);
        std::signal(SIGINT, request_stop);
        std::signal(SIGPIPE, SIG_IGN);
        minidb::Database database(argv[1], {durable});
        Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        check(listener.get() >= 0, "socket");
        const int reuse = 1;
        check(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0, "reuse address");
        check(::fcntl(listener.get(), F_SETFL, O_NONBLOCK) == 0, "nonblocking listener");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        check(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        check(::listen(listener.get(), 128) == 0, "listen");
        socklen_t size = sizeof(address);
        check(::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
        {
            WorkerPool pool(database, static_cast<std::size_t>(workers));
            std::cout << "{\"ready\":true,\"host\":\"127.0.0.1\",\"port\":" << ntohs(address.sin_port) << "}" << std::endl;
            while (!stop_requested) {
                pollfd event {listener.get(), POLLIN, 0};
                const int ready = ::poll(&event, 1, 100);
                if (ready < 0 && errno == EINTR) continue;
                check(ready >= 0, "poll");
                if (ready == 0) continue;
                const int fd = ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                check(fd >= 0, "accept");
                timeval idle {timeout, 0};
                if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle)) < 0 ||
                    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof(idle)) < 0) {
                    ::close(fd);
                    continue;
                }
                Socket accepted(fd);
                if (!pool.enqueue(fd)) {
                    send_all(fd, "{\"ok\":false,\"code\":\"busy\",\"error\":\"connection queue is full\"}\n");
                } else accepted.release();
            }
        }
        database.sync();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_server: " << error.what() << '\n';
        return 1;
    }
}
