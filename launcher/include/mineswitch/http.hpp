#pragma once
#include "model.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace mineswitch {
struct HttpRequest {
    std::string method = "GET", url, body;
    std::map<std::string, std::string> headers;
    std::uint64_t max_bytes = 8 * 1024 * 1024;
};
struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};
using ProgressCallback = std::function<void(const Progress&)>;
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse request(const HttpRequest&, const std::atomic_bool* cancelled = nullptr) = 0;
    virtual void download(const Download&, const std::string& temporary_path,
                          ProgressCallback, const std::atomic_bool* cancelled = nullptr) = 0;
};
class CurlHttpClient final : public HttpClient {
public:
    explicit CurlHttpClient(std::string user_agent = "MineSwitch/0.2.0 (Nintendo Switch homebrew)",
                            std::string ca_bundle = {});
    ~CurlHttpClient() override;
    HttpResponse request(const HttpRequest&, const std::atomic_bool* cancelled = nullptr) override;
    void download(const Download&, const std::string&, ProgressCallback,
                  const std::atomic_bool* cancelled = nullptr) override;
private:
    struct State;
    std::unique_ptr<State> state_;
    std::string user_agent_;
    std::string ca_bundle_;
};
}
