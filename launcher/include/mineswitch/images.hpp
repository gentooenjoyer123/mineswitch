#pragma once
#include <SDL.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mineswitch {
// Decodes PNG/JPEG files and downloads catalog icons on a background thread.
// Textures are created on the render thread in pump(); get() never blocks.
class ImageCache {
  public:
    ImageCache(SDL_Renderer *, std::string icon_cache_directory);
    ~ImageCache();
    ImageCache(const ImageCache &) = delete;
    // source: absolute local path or https:// URL. Images are scaled down to
    // fit max_w × max_h. Returns nullptr while pending or after a failure.
    SDL_Texture *get(const std::string &source, int max_w, int max_h);
    bool failed(const std::string &source, int max_w, int max_h);
    void forget(const std::string &source); // After the file was replaced or deleted.
    void pump();                            // Call once per frame on the render thread.
    void clear();

  private:
    struct Request {
        std::string key, source;
        int max_w, max_h;
    };
    struct Decoded {
        std::string key;
        int w = 0, h = 0;
        std::vector<unsigned char> pixels; // Empty on failure.
    };
    struct Entry {
        SDL_Texture *texture = nullptr;
        bool pending = true, failed = false;
        std::uint64_t used = 0;
    };
    void work();
    Decoded decode(const Request &);
    std::string download(const std::string &url);

    SDL_Renderer *renderer_;
    std::string icons_;
    std::map<std::string, Entry> entries_;
    std::uint64_t frame_ = 0;
    std::mutex mutex_;
    std::deque<Request> queue_;
    std::deque<Decoded> done_;
    std::atomic_bool stop_{false};
    struct Worker;
    std::unique_ptr<Worker> worker_;
};
} // namespace mineswitch
