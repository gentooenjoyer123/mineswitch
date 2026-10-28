#include "mineswitch/http.hpp"
#include <curl/curl.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace mineswitch {
namespace {
std::string lower(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
void validate_url(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || url.size() > 8192)
        throw Error("http_url", "Downloads require a valid HTTPS URL");
    auto end = url.find_first_of("/?#", 8);
    auto authority = url.substr(8, end == std::string::npos ? end : end - 8);
    if (authority.empty() || authority.find('@') != std::string::npos || url.find('#') != std::string::npos)
        throw Error("http_url", "Credentials and fragments are not allowed in download URLs");
    for (unsigned char ch : url) if (ch <= 32 || ch == 127)
        throw Error("http_url", "Invalid character in download URL");
}
struct HashState {
    mbedtls_md_context_t context;
    std::string expected;
    explicit HashState(mbedtls_md_type_t type, const std::string& hash) : expected(lower(hash)) {
        mbedtls_md_init(&context);
        auto* info = mbedtls_md_info_from_type(type);
        if (!info || expected.size() != 2 * mbedtls_md_get_size(info)
                || expected.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw Error("download_hash", "Invalid published artifact hash");
        if (mbedtls_md_setup(&context, info, 0) || mbedtls_md_starts(&context)) {
            mbedtls_md_free(&context);
            throw Error("download_hash", "Cannot initialize artifact hash verification");
        }
    }
    ~HashState() { mbedtls_md_free(&context); }
    HashState(const HashState&) = delete;
    bool update(const void* bytes, std::size_t size) {
        return mbedtls_md_update(&context, static_cast<const unsigned char*>(bytes), size) == 0;
    }
    bool matches() {
        std::array<unsigned char, MBEDTLS_MD_MAX_SIZE> value{};
        if (mbedtls_md_finish(&context, value.data())) return false;
        static const char hex[] = "0123456789abcdef";
        std::string actual;
        for (std::size_t i = 0; i < expected.size() / 2; ++i) {
            actual += hex[value[i] >> 4]; actual += hex[value[i] & 15];
        }
        return actual == expected;
    }
};
struct Transfer {
    HttpResponse response;
    const std::atomic_bool* cancel = nullptr;
    ProgressCallback progress;
    std::string detail;
    std::uint64_t limit = 0, received = 0;
    std::uint64_t discarded = 0;
    int fd = -1;
    bool failed = false;
    std::string failure;
    std::vector<std::unique_ptr<HashState>> hashes;
    std::chrono::steady_clock::time_point last_progress{};
};
size_t write_body(char* data, size_t width, size_t count, void* opaque) noexcept {
    auto& transfer = *static_cast<Transfer*>(opaque);
    try {
        if (width && count > SIZE_MAX / width) return 0;
        std::size_t bytes = width * count;
        if (transfer.fd >= 0 && transfer.response.status != 200) {
            if (bytes > 1024 * 1024 - transfer.discarded) return 0;
            transfer.discarded += bytes;
            return bytes;
        }
        if ((transfer.cancel && transfer.cancel->load()) || bytes > transfer.limit - transfer.received) {
            transfer.failed = true; transfer.failure = "Download cancelled or exceeded its size limit"; return 0;
        }
        if (transfer.fd < 0) transfer.response.body.append(data, bytes);
        else {
            std::size_t offset = 0;
            while (offset < bytes) {
                ssize_t written = ::write(transfer.fd, data + offset, bytes - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) {
                    transfer.failed = true; transfer.failure = "Cannot write download to SD storage"; return 0;
                }
                offset += static_cast<std::size_t>(written);
            }
            for (const auto& hash : transfer.hashes) if (!hash->update(data, bytes)) {
                transfer.failed = true; transfer.failure = "Artifact hashing failed"; return 0;
            }
        }
        transfer.received += bytes;
        return bytes;
    } catch (...) { transfer.failed = true; return 0; }
}
size_t read_header(char* data, size_t width, size_t count, void* opaque) noexcept {
    try {
        if (width && count > SIZE_MAX / width) return 0;
        std::size_t bytes = width * count;
        if (bytes > 16384) return 0;
        auto& response = static_cast<Transfer*>(opaque)->response;
        std::string line(data, bytes);
        if (line.rfind("HTTP/", 0) == 0) {
            response.headers.clear(); response.body.clear();
            auto space = line.find(' ');
            response.status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
        }
        auto colon = line.find(':');
        if (colon != std::string::npos && response.headers.size() < 128) {
            auto name = lower(line.substr(0, colon));
            auto first = line.find_first_not_of(" \t", colon + 1);
            auto last = line.find_last_not_of(" \t\r\n");
            response.headers[name] = first == std::string::npos || last < first ? "" : line.substr(first, last - first + 1);
        }
        return bytes;
    } catch (...) { return 0; }
}
int progress(void* opaque, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t) noexcept {
    auto& transfer = *static_cast<Transfer*>(opaque);
    if (transfer.cancel && transfer.cancel->load()) return 1;
    try {
        auto now = std::chrono::steady_clock::now();
        if (transfer.progress && now - transfer.last_progress >= std::chrono::milliseconds(100)) {
            transfer.last_progress = now;
            Progress event;
            event.stage = "download"; event.detail = transfer.detail;
            event.completed = done > 0 ? static_cast<std::uint64_t>(done) : 0;
            event.total = total > 0 ? static_cast<std::uint64_t>(total) : transfer.limit;
            transfer.progress(event);
        }
    } catch (...) { transfer.failed = true; return 1; }
    return 0;
}
void check_result(CURLcode code, const Transfer& transfer) {
    if (transfer.cancel && transfer.cancel->load()) throw Error("cancelled", "Operation cancelled");
    if (transfer.failed) throw Error("download_write", transfer.failure.empty() ? "Download processing failed" : transfer.failure);
    if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR)
        throw Error("http_tls", "Secure connection failed. Check the console date, network and certificate trust.");
    if (code != CURLE_OK) throw Error("http_transfer", curl_easy_strerror(code));
}
void setup(CURL* handle, Transfer& transfer, const std::string& url, const std::string& user_agent, bool redirect) {
    validate_url(url);
    curl_easy_reset(handle); // Retains the easy handle's connection/DNS cache.
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, redirect ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
}
}

struct CurlHttpClient::State {
    CURL* handle = nullptr;
    std::mutex mutex;
    State() {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw Error("http_init", "Cannot initialize HTTPS");
        });
        handle = curl_easy_init();
        if (!handle) throw Error("http_init", "Cannot allocate HTTPS client");
    }
    ~State() { if (handle) curl_easy_cleanup(handle); }
};
CurlHttpClient::CurlHttpClient(std::string user_agent, std::string ca_bundle)
    : state_(new State), user_agent_(std::move(user_agent)), ca_bundle_(std::move(ca_bundle)) {}
CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::request(const HttpRequest& request, const std::atomic_bool* cancelled) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    Transfer transfer;
    transfer.cancel = cancelled; transfer.limit = request.max_bytes;
    if (request.max_bytes == 0 || request.max_bytes > 64 * 1024 * 1024)
        throw Error("http_limit", "Invalid metadata response limit");
    bool sensitive = false;
    curl_slist* headers = nullptr;
    for (const auto& header : request.headers) {
        if (header.first.find_first_of("\r\n:") != std::string::npos || header.second.find_first_of("\r\n") != std::string::npos) {
            curl_slist_free_all(headers); throw Error("http_header", "Invalid request header");
        }
        auto key = lower(header.first);
        sensitive |= key == "authorization" || key == "x-api-key" || key == "cookie";
        auto* next = curl_slist_append(headers, (header.first + ": " + header.second).c_str());
        if (!next) { curl_slist_free_all(headers); throw Error("http_memory", "Cannot allocate HTTP headers"); }
        headers = next;
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> release(headers, curl_slist_free_all);
    // libcurl forwards custom headers across redirects. Credential-bearing API
    // requests deliberately stay at their exact configured endpoint.
    setup(state_->handle, transfer, request.url, user_agent_, !sensitive);
    if (!ca_bundle_.empty()) curl_easy_setopt(state_->handle, CURLOPT_CAINFO, ca_bundle_.c_str());
    curl_easy_setopt(state_->handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(state_->handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(state_->handle, CURLOPT_TIMEOUT, 60L);
    if (request.method == "POST") {
        curl_easy_setopt(state_->handle, CURLOPT_POST, 1L);
        curl_easy_setopt(state_->handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(state_->handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    } else if (request.method != "GET") throw Error("http_method", "Unsupported API request method");
    auto result = curl_easy_perform(state_->handle);
    check_result(result, transfer);
    long status = 0; curl_easy_getinfo(state_->handle, CURLINFO_RESPONSE_CODE, &status);
    transfer.response.status = static_cast<int>(status);
    return std::move(transfer.response);
}

void CurlHttpClient::download(const Download& artifact, const std::string& temporary_path,
                            ProgressCallback on_progress, const std::atomic_bool* cancelled) {
    if (artifact.manual || artifact.urls.empty()) throw Error("download_manual", "This file needs an authorized manual download");
    if (artifact.hashes.empty()) throw Error("download_hash", "Artifact has no published integrity hash");
    if (artifact.max_bytes > (UINT64_C(16) << 30) || artifact.bytes > artifact.max_bytes)
        throw Error("download_size", "Artifact exceeds the available download budget");
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::string last_failure;
    for (const auto& url : artifact.urls) {
        if (cancelled && cancelled->load()) throw Error("cancelled", "Operation cancelled");
        Transfer transfer;
        transfer.cancel = cancelled;
        transfer.limit = artifact.bytes ? artifact.bytes : artifact.max_bytes;
        transfer.progress = on_progress; transfer.detail = artifact.file_name;
        for (auto entry : {std::make_pair(MBEDTLS_MD_SHA1, artifact.hashes.sha1),
                           std::make_pair(MBEDTLS_MD_SHA256, artifact.hashes.sha256),
                           std::make_pair(MBEDTLS_MD_SHA512, artifact.hashes.sha512),
                           std::make_pair(MBEDTLS_MD_MD5, artifact.hashes.md5)})
            if (!entry.second.empty()) transfer.hashes.emplace_back(new HashState(entry.first, entry.second));
        setup(state_->handle, transfer, url, user_agent_, true);
        if (!ca_bundle_.empty()) curl_easy_setopt(state_->handle, CURLOPT_CAINFO, ca_bundle_.c_str());
        curl_easy_setopt(state_->handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(transfer.limit));
        transfer.fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (transfer.fd < 0) throw Error("download_file", "Cannot create a new temporary download file");
        try {
            auto result = curl_easy_perform(state_->handle);
            check_result(result, transfer);
            long status = 0; curl_easy_getinfo(state_->handle, CURLINFO_RESPONSE_CODE, &status);
            if (status != 200) throw Error("download_status", "Download server returned HTTP " + std::to_string(status));
            if (artifact.bytes && transfer.received != artifact.bytes)
                throw Error("download_size", "Downloaded size does not match metadata");
            for (const auto& hash : transfer.hashes)
                if (!hash->matches()) throw Error("download_hash", "Downloaded artifact failed integrity verification");
            if (::fsync(transfer.fd)) throw Error("download_flush", "Cannot flush verified file to SD storage");
            if (::close(transfer.fd)) { transfer.fd = -1; throw Error("download_close", "Cannot close verified download"); }
            transfer.fd = -1;
            return;
        } catch (const Error& error) {
            if (transfer.fd >= 0) ::close(transfer.fd);
            ::unlink(temporary_path.c_str());
            if (error.code == "cancelled" || error.code == "download_write" || error.code == "download_flush") throw;
            last_failure = error.what();
        } catch (...) {
            if (transfer.fd >= 0) ::close(transfer.fd);
            ::unlink(temporary_path.c_str());
            throw;
        }
    }
    throw Error("download_failed", last_failure.empty() ? "All artifact download sources failed" : last_failure);
}
}
