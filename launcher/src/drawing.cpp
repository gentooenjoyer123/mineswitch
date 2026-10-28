#include "mineswitch/drawing.hpp"
#include "mineswitch/model.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mineswitch {
namespace {
std::vector<std::uint32_t> codepoints(const std::string &s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = s[i++];
        unsigned count = c < 128 ? 0 : (c & 224) == 192 ? 1 : (c & 240) == 224 ? 2 : (c & 248) == 240 ? 3 : 4;
        if (count == 4 || i + count > s.size()) {
            out.push_back(0xfffd);
            continue;
        }
        std::uint32_t value = count ? c & ((1u << (6 - count)) - 1) : c;
        bool valid = true;
        for (unsigned j = 0; j < count; ++j) {
            unsigned char next = s[i];
            if ((next & 192) != 128) {
                valid = false;
                break;
            }
            ++i;
            value = (value << 6) | (next & 63);
        }
        out.push_back(valid && value <= 0x10ffff ? value : 0xfffd);
    }
    return out;
}
} // namespace
Drawing::Drawing(Uint32 subsystems) : subsystems_(subsystems) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    const Uint32 previous = SDL_WasInit(0);
    if (SDL_InitSubSystem(subsystems_)) {
        std::string reason = SDL_GetError();
        SDL_QuitSubSystem(subsystems_ & ~previous);
        throw Error("ui_init", reason);
    }
    try {
        window_ = SDL_CreateWindow("MineSwitch", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (!window_)
            throw Error("ui_window", SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_)
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_)
            throw Error("ui_renderer", SDL_GetError());
        SDL_RenderSetLogicalSize(renderer_, 1280, 720);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        if (FT_Init_FreeType(&library_))
            throw Error("ui_font", "Cannot initialize fonts");
#ifdef __SWITCH__
        if (R_FAILED(plInitialize(PlServiceType_User)))
            throw Error("ui_font", "Cannot open system fonts");
        shared_font_ = true;
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, PlSharedFontType_Standard)) ||
            FT_New_Memory_Face(library_, static_cast<const FT_Byte *>(data.address), data.size, 0, &face_))
            throw Error("ui_font", "Cannot load the system font");
#else
        const char *custom = std::getenv("MINESWITCH_FONT");
        for (const char *path :
             {custom, "/usr/share/fonts/roboto/Roboto-Regular.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            if (path && FT_New_Face(library_, path, 0, &face_) == 0)
                break;
        }
        if (!face_)
            throw Error("ui_font", "Set MINESWITCH_FONT to a TrueType font");
#endif
    } catch (...) {
        if (face_)
            FT_Done_Face(face_);
        if (library_)
            FT_Done_FreeType(library_);
#ifdef __SWITCH__
        if (shared_font_)
            plExit();
#endif
        if (renderer_)
            SDL_DestroyRenderer(renderer_);
        if (window_)
            SDL_DestroyWindow(window_);
        SDL_QuitSubSystem(subsystems_);
        throw;
    }
}
Drawing::~Drawing() {
    for (const auto &item : labels_)
        SDL_DestroyTexture(item.second.texture);
    FT_Done_Face(face_);
    FT_Done_FreeType(library_);
#ifdef __SWITCH__
    if (shared_font_)
        plExit();
#endif
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(subsystems_);
}
void Drawing::clear() {
    SDL_SetRenderDrawColor(renderer_, background.r, background.g, background.b, 255);
    SDL_RenderClear(renderer_);
}
void Drawing::present() { SDL_RenderPresent(renderer_); }
void Drawing::box(SDL_Rect r, Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(renderer_, &r);
}
void Drawing::outline(SDL_Rect r, Color c, int n) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    for (int i = 0; i < n; ++i) {
        SDL_RenderDrawRect(renderer_, &r);
        ++r.x;
        ++r.y;
        r.w -= 2;
        r.h -= 2;
    }
}
namespace {
// Corner arc points from angle a0 to a1 around (cx, cy).
void arc(std::vector<SDL_FPoint> &points, float cx, float cy, float radius, float a0, float a1) {
    const int steps = radius < 6 ? 3 : radius < 14 ? 5 : 8;
    for (int i = 0; i <= steps; ++i) {
        float a = a0 + (a1 - a0) * i / steps;
        points.push_back({cx + radius * SDL_cosf(a), cy + radius * SDL_sinf(a)});
    }
}
std::vector<SDL_FPoint> roundedPath(SDL_Rect r, float radius) {
    radius = std::min({radius, r.w / 2.0f, r.h / 2.0f});
    const float pi = 3.14159265f;
    std::vector<SDL_FPoint> points;
    arc(points, r.x + r.w - radius, r.y + radius, radius, -pi / 2, 0);
    arc(points, r.x + r.w - radius, r.y + r.h - radius, radius, 0, pi / 2);
    arc(points, r.x + radius, r.y + r.h - radius, radius, pi / 2, pi);
    arc(points, r.x + radius, r.y + radius, radius, pi, 1.5f * pi);
    return points;
}
SDL_Color sdl(Color c) { return {c.r, c.g, c.b, c.a}; }
} // namespace
void Drawing::rounded(SDL_Rect r, int radius, Color c) {
    if (radius <= 0 || r.w <= 2 || r.h <= 2) {
        box(r, c);
        return;
    }
    auto path = roundedPath(r, static_cast<float>(radius));
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(path.size() + 1);
    vertices.push_back({{r.x + r.w / 2.0f, r.y + r.h / 2.0f}, sdl(c), {0, 0}});
    for (auto p : path)
        vertices.push_back({p, sdl(c), {0, 0}});
    std::vector<int> indices;
    for (int i = 1; i < static_cast<int>(vertices.size()); ++i) {
        indices.push_back(0);
        indices.push_back(i);
        indices.push_back(i + 1 < static_cast<int>(vertices.size()) ? i + 1 : 1);
    }
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}
void Drawing::roundedOutline(SDL_Rect r, int radius, Color c, int thickness) {
    if (radius <= 0) {
        outline(r, c, thickness);
        return;
    }
    auto outer = roundedPath(r, static_cast<float>(radius));
    SDL_Rect in{r.x + thickness, r.y + thickness, r.w - 2 * thickness, r.h - 2 * thickness};
    auto inner = roundedPath(in, static_cast<float>(std::max(1, radius - thickness)));
    if (inner.size() != outer.size() || in.w <= 0 || in.h <= 0) {
        outline(r, c, thickness);
        return;
    }
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const int n = static_cast<int>(outer.size());
    for (int i = 0; i < n; ++i) {
        vertices.push_back({outer[i], sdl(c), {0, 0}});
        vertices.push_back({inner[i], sdl(c), {0, 0}});
    }
    for (int i = 0; i < n; ++i) {
        int a = 2 * i, b = 2 * ((i + 1) % n);
        for (int index : {a, b, a + 1, a + 1, b, b + 1})
            indices.push_back(index);
    }
    SDL_RenderGeometry(renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}
void Drawing::circle(int cx, int cy, int radius, Color c) { rounded({cx - radius, cy - radius, radius * 2, radius * 2}, radius, c); }
void Drawing::pushClip(SDL_Rect r) {
    if (!clips_.empty()) {
        SDL_Rect both;
        if (!SDL_IntersectRect(&clips_.back(), &r, &both))
            both = {r.x, r.y, 0, 0};
        r = both;
    }
    clips_.push_back(r);
    SDL_RenderSetClipRect(renderer_, &clips_.back());
}
void Drawing::popClip() {
    if (!clips_.empty())
        clips_.pop_back();
    SDL_RenderSetClipRect(renderer_, clips_.empty() ? nullptr : &clips_.back());
}
void Drawing::image(SDL_Texture *texture, SDL_Rect r, bool cover, Uint8 alpha) {
    if (!texture)
        return;
    int w = 0, h = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    SDL_Rect source{0, 0, w, h}, dest = r;
    if (cover) {
        // Crop the source to the destination aspect ratio.
        if (static_cast<long>(w) * r.h > static_cast<long>(h) * r.w) {
            source.w = static_cast<int>(static_cast<long>(h) * r.w / std::max(1, r.h));
            source.x = (w - source.w) / 2;
        } else {
            source.h = static_cast<int>(static_cast<long>(w) * r.h / std::max(1, r.w));
            source.y = (h - source.h) / 2;
        }
    } else {
        // Fit inside the destination, centred.
        if (static_cast<long>(w) * r.h > static_cast<long>(h) * r.w) {
            dest.h = static_cast<int>(static_cast<long>(h) * r.w / w);
            dest.y += (r.h - dest.h) / 2;
        } else {
            dest.w = static_cast<int>(static_cast<long>(w) * r.h / h);
            dest.x += (r.w - dest.w) / 2;
        }
    }
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_RenderCopy(renderer_, texture, &source, &dest);
}
const Drawing::Label &Drawing::label(const std::string &string, int size) {
    std::string key = std::to_string(size) + ":" + string;
    auto found = labels_.find(key);
    if (found != labels_.end())
        return found->second;
    if (labels_.size() >= 768) {
        for (const auto &entry : labels_)
            SDL_DestroyTexture(entry.second.texture);
        labels_.clear();
    }
    FT_Set_Pixel_Sizes(face_, 0, size);
    auto chars = codepoints(string);
    int w = 4, height = size * 2;
    for (auto ch : chars)
        if (!FT_Load_Char(face_, ch, FT_LOAD_DEFAULT))
            w += face_->glyph->advance.x >> 6;
    w = std::min(w, 16384);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * height * 4, 0);
    int pen = 1, baseline = static_cast<int>(face_->size->metrics.ascender >> 6);
    for (auto ch : chars) {
        if (FT_Load_Char(face_, ch, FT_LOAD_RENDER))
            continue;
        auto &glyph = *face_->glyph;
        for (unsigned row = 0; row < glyph.bitmap.rows; ++row)
            for (unsigned col = 0; col < glyph.bitmap.width; ++col) {
                int x = pen + glyph.bitmap_left + col, y = baseline - glyph.bitmap_top + row;
                if (x < 0 || y < 0 || x >= w || y >= height)
                    continue;
                auto offset = (static_cast<std::size_t>(y) * w + x) * 4;
                auto source = glyph.bitmap.pitch >= 0 ? row * glyph.bitmap.pitch
                                                      : (glyph.bitmap.rows - row - 1) * -glyph.bitmap.pitch;
                pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = 255;
                pixels[offset + 3] = glyph.bitmap.buffer[source + col];
            }
        pen += glyph.advance.x >> 6;
    }
    SDL_Texture *texture =
        SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, height);
    if (!texture)
        throw Error("ui_texture", SDL_GetError());
    SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return labels_.emplace(key, Label{texture, w, height}).first->second;
}
int Drawing::width(const std::string &s, int size) {
    auto cached = labels_.find(std::to_string(size) + ":" + s);
    if (cached != labels_.end())
        return cached->second.width;
    FT_Set_Pixel_Sizes(face_, 0, size);
    int width = 4;
    for (auto ch : codepoints(s))
        if (!FT_Load_Char(face_, ch, FT_LOAD_DEFAULT))
            width += face_->glyph->advance.x >> 6;
    return width;
}
void Drawing::text(const std::string &s, int x, int y, int size, Color c, int maximum_width) {
    std::string shown = s;
    if (maximum_width && width(shown, size) > maximum_width) {
        const int room = maximum_width - width("…", size);
        int used = 4;
        std::size_t end = 0;
        for (auto ch : codepoints(s)) {
            if (FT_Load_Char(face_, ch, FT_LOAD_DEFAULT))
                continue;
            int advance = face_->glyph->advance.x >> 6;
            if (used + advance > room)
                break;
            used += advance;
            if (end < s.size()) {
                ++end;
                while (end < s.size() && (static_cast<unsigned char>(s[end]) & 192) == 128)
                    ++end;
            }
        }
        shown = s.substr(0, end) + "…";
    }
    const auto &l = label(shown, size);
    SDL_SetTextureColorMod(l.texture, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(l.texture, c.a);
    SDL_Rect dest{x, y, l.width, l.height};
    SDL_RenderCopy(renderer_, l.texture, nullptr, &dest);
}
void Drawing::wrap(const std::string &s, int x, int y, int size, Color c, int maximum_width, int lines) {
    std::string line;
    std::size_t begin = 0;
    while (begin < s.size() && lines > 0) {
        auto end = s.find_first_of(" \n", begin);
        auto word = s.substr(begin, end == std::string::npos ? end : end - begin);
        auto next = line.empty() ? word : line + " " + word;
        if (!line.empty() && width(next, size) > maximum_width) {
            text(line, x, y, size, c, maximum_width);
            y += size + 9;
            --lines;
            line = word;
        } else
            line = next;
        if (end == std::string::npos)
            break;
        if (s[end] == '\n' && lines > 0) {
            text(line, x, y, size, c, maximum_width);
            line.clear();
            y += size + 9;
            --lines;
        }
        begin = end + 1;
    }
    if (lines > 0 && !line.empty())
        text(line, x, y, size, c, maximum_width);
}
void Drawing::screenshot(const std::string &path) {
    int w, h;
    SDL_GetRendererOutputSize(renderer_, &w, &h);
    SDL_Surface *image = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!image)
        throw Error("ui_capture", SDL_GetError());
    int result = SDL_RenderReadPixels(renderer_, nullptr, image->format->format, image->pixels, image->pitch);
    if (!result)
        result = SDL_SaveBMP(image, path.c_str());
    SDL_FreeSurface(image);
    if (result)
        throw Error("ui_capture", SDL_GetError());
}
} // namespace mineswitch
