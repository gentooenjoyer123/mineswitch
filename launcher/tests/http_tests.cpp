#include "mineswitch/http.hpp"
#include <mbedtls/md.h>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>

using namespace mineswitch;
namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("FAIL: " + label);
    ++checks;
}
Error rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) { ++checks; return error; }
    throw std::runtime_error("FAIL: expected an HTTP error");
}
std::string read(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read HTTP test output");
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
bool exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}
std::uint64_t size(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}
std::string repeated(const std::string& text, unsigned count) {
    std::string result;
    for (unsigned i = 0; i < count; ++i) result += text;
    return result;
}
std::string digest(const std::string& text, mbedtls_md_type_t type) {
    const auto* info = mbedtls_md_info_from_type(type);
    unsigned char bytes[MBEDTLS_MD_MAX_SIZE] {};
    if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes))
        throw std::runtime_error("Cannot hash local fixture");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < mbedtls_md_get_size(info); ++i) {
        result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15];
    }
    return result;
}
Download artifact(const std::string& url, const std::string& body) {
    Download result;
    result.file_name = "local-fixture.txt";
    result.urls = {url}; result.bytes = body.size(); result.max_bytes = body.size();
    result.hashes.sha1 = digest(body, MBEDTLS_MD_SHA1);
    result.hashes.sha256 = digest(body, MBEDTLS_MD_SHA256);
    result.hashes.sha512 = digest(body, MBEDTLS_MD_SHA512);
    result.hashes.md5 = digest(body, MBEDTLS_MD_MD5);
    return result;
}
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: http_tests HTTPS_BASE_URL CA_CERT OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const std::string base = argv[1], certificate = argv[2], output = argv[3];
        const std::string body = repeated("MineSwitch HTTPS fixture\n", 64);
        const std::string metadata = "{\"name\":\"local fixture\",\"version\":1}";
        CurlHttpClient http("MineSwitch HTTP fixture", certificate);
        HttpRequest get;
        get.url = base + "/metadata"; get.max_bytes = 256;
        get.headers["X-Fixture"] = "local";
        const auto response = http.request(get);
        check(response.status == 200 && response.body == metadata, "metadata response");
        check(response.headers.at("content-type") == "application/json", "response headers parsed");
        check(response.headers.at("x-fixture") == "local", "custom request header sent");

        HttpRequest post;
        post.method = "POST"; post.url = base + "/echo";
        post.headers["Content-Type"] = "application/json";
        post.body = "{\"ids\":[1,2,3]}";
        check(http.request(post).body == post.body, "POST metadata body");
        get.url = base + "/redirect-metadata";
        check(http.request(get).body == metadata, "ordinary HTTPS metadata redirect");
        get.url = base + "/not-found";
        check(http.request(get).status == 404, "metadata HTTP errors retain their status");

        auto download = artifact(base + "/artifact", body);
        const auto normal = output + "/normal.part";
        http.download(download, normal, {});
        check(read(normal) == body, "artifact downloaded with all four digest checks");
        const auto existing = rejects([&] { http.download(download, normal, {}); });
        check(existing.code == "download_file" && read(normal) == body, "existing output is preserved");

        download.urls = {base + "/not-found", base + "/artifact"};
        const auto alternate = output + "/alternate.part";
        http.download(download, alternate, {});
        check(read(alternate) == body, "alternate URL after ordinary missing artifact");
        download.urls = {base + "/redirect-artifact"};
        const auto redirected = output + "/redirect.part";
        http.download(download, redirected, {});
        check(read(redirected) == body, "redirect page is excluded from artifact data and hashes");

        download = artifact(base + "/stream", body);
        download.bytes = 0;
        const auto unknown = output + "/unknown.part";
        http.download(download, unknown, {});
        check(read(unknown) == body, "bounded transfer without declared size");
        download.max_bytes = 512;
        const auto bounded = output + "/bounded.part";
        bool exceeded = false;
        rejects([&] { http.download(download, bounded, [&](const Progress&) {
            exceeded |= size(bounded) > download.max_bytes;
        }); });
        check(!exceeded && !exists(bounded), "finite transfer budget and partial-file cleanup");

        const auto empty_path = output + "/empty.part";
        auto empty = artifact(base + "/empty", "");
        http.download(empty, empty_path, {});
        check(exists(empty_path) && size(empty_path) == 0, "zero-byte artifact with zero byte budget");

        const auto cancelled_path = output + "/cancelled.part";
        auto slow = artifact(base + "/slow", repeated(body, 128));
        std::atomic_bool cancelled {false};
        bool saw_partial = false;
        const auto cancelled_error = rejects([&] {
            http.download(slow, cancelled_path, [&](const Progress& event) {
                if (event.completed >= 2048) { saw_partial = true; cancelled = true; }
            }, &cancelled);
        });
        check(saw_partial && cancelled_error.code == "cancelled" && !exists(cancelled_path),
              "mid-transfer cancellation removes partial output");
        const auto pre_cancelled = rejects([&] { http.download(slow, cancelled_path, {}, &cancelled); });
        check(pre_cancelled.code == "cancelled" && !exists(cancelled_path), "pre-cancelled transfer does not create output");

        const auto callback_path = output + "/callback.part";
        const auto callback_error = rejects([&] {
            http.download(slow, callback_path, [](const Progress&) { throw std::runtime_error("UI consumer stopped"); });
        });
        check(callback_error.code == "download_write" && !exists(callback_path), "callback exception cleans partial output");

        // The local self-signed fixture is only trusted when explicitly supplied.
        CurlHttpClient ordinary_trust("MineSwitch HTTP fixture");
        get.url = base + "/metadata";
        const auto trust_error = rejects([&] { ordinary_trust.request(get); });
        check(trust_error.code == "http_tls", "certificate verification remains enabled");
        get.max_bytes = 4;
        rejects([&] { http.request(get); });
        check(http.request(post).body == post.body, "client remains usable after transfer errors");
        std::cout << "HTTP tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
