#include "mineswitch/images.hpp"
#include "mineswitch/http.hpp"
#include "mineswitch/storage.hpp"
#include <algorithm>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <webp/decode.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#include "../third_party/stb_image.h"
#pragma GCC diagnostic pop

namespace mineswitch {
namespace {
constexpr std::size_t max_entries = 160;
constexpr std::size_t max_image_file = 24 * 1024 * 1024;
constexpr std::uint64_t max_icon_download = 2 * 1024 * 1024;

std::string keyOf(const std::string &source, int w, int h) {
    return std::to_string(w) + "x" + std::to_string(h) + ":" + source;
}
std::string fnv(const std::string &text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));
    return out;
}
// Box-filter downscale of RGBA pixels; keeps icons and thumbnails sharp enough.
std::vector<unsigned char> shrink(const unsigned char *pixels, int w, int h, int nw, int nh) {
    std::vector<unsigned char> out(static_cast<std::size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) {
        int y0 = y * h / nh, y1 = std::max(y0 + 1, (y + 1) * h / nh);
        for (int x = 0; x < nw; ++x) {
            int x0 = x * w / nw, x1 = std::max(x0 + 1, (x + 1) * w / nw);
            unsigned sum[4] = {0, 0, 0, 0}, count = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const unsigned char *p = pixels + (static_cast<std::size_t>(sy) * w + sx) * 4;
                    for (int c = 0; c < 4; ++c)
                        sum[c] += p[c];
                    ++count;
                }
            unsigned char *q = &out[(static_cast<std::size_t>(y) * nw + x) * 4];
            for (int c = 0; c < 4; ++c)
                q[c] = static_cast<unsigned char>(sum[c] / count);
        }
    }
    return out;
}
} // namespace

struct ImageCache::Worker {
#ifdef __SWITCH__
    Thread thread{};
    bool started = false;
#else
    std::thread thread;
#endif
    CurlHttpClient http;
};

ImageCache::ImageCache(SDL_Renderer *renderer, std::string icons)
    : renderer_(renderer), icons_(std::move(icons)), worker_(new Worker) {
#ifdef __SWITCH__
    auto entry = [](void *self) { static_cast<ImageCache *>(self)->work(); };
    if (R_SUCCEEDED(threadCreate(&worker_->thread, entry, this, nullptr, 512 * 1024, 0x3B, -2))) {
        if (R_SUCCEEDED(threadStart(&worker_->thread)))
            worker_->started = true;
        else
            threadClose(&worker_->thread);
    }
#else
    worker_->thread = std::thread([this] { work(); });
#endif
}
ImageCache::~ImageCache() {
    stop_ = true;
#ifdef __SWITCH__
    if (worker_->started) {
        threadWaitForExit(&worker_->thread);
        threadClose(&worker_->thread);
    }
#else
    if (worker_->thread.joinable())
        worker_->thread.join();
#endif
    clear();
}

SDL_Texture *ImageCache::get(const std::string &source, int max_w, int max_h) {
    if (source.empty())
        return nullptr;
    auto key = keyOf(source, max_w, max_h);
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        found->second.used = frame_;
        return found->second.texture;
    }
    if (entries_.size() >= max_entries) {
        // Evict the least recently drawn texture that is not pending.
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (!it->second.pending && it->second.used < frame_ &&
                (victim == entries_.end() || it->second.used < victim->second.used))
                victim = it;
        if (victim == entries_.end())
            return nullptr;
        if (victim->second.texture)
            SDL_DestroyTexture(victim->second.texture);
        entries_.erase(victim);
    }
    Entry entry;
    entry.used = frame_;
    entries_.emplace(key, entry);
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({key, source, max_w, max_h});
    return nullptr;
}
bool ImageCache::failed(const std::string &source, int max_w, int max_h) {
    auto found = entries_.find(keyOf(source, max_w, max_h));
    return found != entries_.end() && found->second.failed;
}
void ImageCache::forget(const std::string &source) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto colon = it->first.find(':');
        if (it->first.compare(colon + 1, std::string::npos, source) == 0 && !it->second.pending) {
            if (it->second.texture)
                SDL_DestroyTexture(it->second.texture);
            it = entries_.erase(it);
        } else
            ++it;
    }
}
void ImageCache::pump() {
    ++frame_;
    std::deque<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Uploading is cheap, but bound the work per frame anyway.
        for (int i = 0; i < 6 && !done_.empty(); ++i) {
            ready.push_back(std::move(done_.front()));
            done_.pop_front();
        }
    }
    for (auto &image : ready) {
        auto found = entries_.find(image.key);
        if (found == entries_.end())
            continue;
        found->second.pending = false;
        if (image.pixels.empty()) {
            found->second.failed = true;
            continue;
        }
        auto *texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, image.w, image.h);
        if (!texture) {
            found->second.failed = true;
            continue;
        }
        SDL_UpdateTexture(texture, nullptr, image.pixels.data(), image.w * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
        found->second.texture = texture;
    }
}
void ImageCache::clear() {
    for (auto &entry : entries_)
        if (entry.second.texture)
            SDL_DestroyTexture(entry.second.texture);
    entries_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    done_.clear();
}

void ImageCache::work() {
    while (!stop_) {
        Request request;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) {
                // Newest requests first: they are what is on screen now.
                request = std::move(queue_.back());
                queue_.pop_back();
            }
        }
        if (request.key.empty()) {
            SDL_Delay(15);
            continue;
        }
        Decoded result;
        try {
            result = decode(request);
        } catch (...) {
            result = Decoded{};
        }
        result.key = request.key;
        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(result));
    }
}

std::string ImageCache::download(const std::string &url) {
    createDirectories(icons_);
    auto path = joinPath(icons_, fnv(url) + ".img");
    if (pathExists(path))
        return readTextFile(path, max_icon_download);
    HttpRequest request;
    request.url = url;
    request.max_bytes = max_icon_download;
    auto response = worker_->http.request(request, &stop_);
    if (response.status != 200 || response.body.empty())
        return {};
    try {
        atomicWriteText(path, response.body);
    } catch (...) {
        // An unwritable cache only costs another download.
    }
    return response.body;
}

ImageCache::Decoded ImageCache::decode(const Request &request) {
    Decoded result;
    std::string bytes = request.source.compare(0, 8, "https://") == 0 ? download(request.source)
                                                                      : readTextFile(request.source, max_image_file);
    if (bytes.empty())
        return result;
    int w = 0, h = 0, channels = 0;
    const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
    unsigned char *pixels = nullptr;
    void (*release)(void *) = stbi_image_free;
    // Modrinth serves most icons as WebP; PNG and JPEG go through stb_image.
    if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) {
        pixels = WebPDecodeRGBA(data, bytes.size(), &w, &h);
        release = [](void *p) { WebPFree(p); };
    } else
        pixels = stbi_load_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        if (pixels)
            release(pixels);
        return result;
    }
    std::unique_ptr<unsigned char, void (*)(void *)> owned(pixels, release);
    int nw = w, nh = h;
    if (nw > request.max_w || nh > request.max_h) {
        double scale = std::min(request.max_w / double(w), request.max_h / double(h));
        nw = std::max(1, static_cast<int>(w * scale));
        nh = std::max(1, static_cast<int>(h * scale));
        result.pixels = shrink(pixels, w, h, nw, nh);
    } else
        result.pixels.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
    result.w = nw;
    result.h = nh;
    return result;
}
} // namespace mineswitch
