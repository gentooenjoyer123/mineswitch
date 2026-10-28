#pragma once
#include <SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <map>
#include <vector>
#include <string>

namespace mineswitch {
struct Color {
    unsigned char r, g, b, a = 255;
};
inline constexpr Color background{13, 20, 30}, panel{23, 34, 47}, raised{31, 45, 61};
inline constexpr Color foreground{234, 241, 248}, muted{147, 168, 187}, accent{152, 232, 180};
inline constexpr Color border{49, 68, 87}, warning{243, 195, 118}, danger{240, 128, 128};
inline constexpr Color sidebar{17, 27, 39}, info{124, 180, 226};
class Drawing {
  public:
    explicit Drawing(Uint32 subsystems = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    ~Drawing();
    Drawing(const Drawing &) = delete;
    void clear();
    void present();
    void box(SDL_Rect, Color);
    void outline(SDL_Rect, Color, int thickness = 2);
    // Anti-aliasing is not available; small radii keep the edges clean.
    void rounded(SDL_Rect, int radius, Color);
    void roundedOutline(SDL_Rect, int radius, Color, int thickness = 2);
    void circle(int cx, int cy, int radius, Color);
    // Nested clip regions intersect; pop restores the previous one.
    void pushClip(SDL_Rect);
    void popClip();
    void image(SDL_Texture *, SDL_Rect, bool cover = false, Uint8 alpha = 255);
    void text(const std::string &, int x, int y, int size = 22, Color color = foreground,
              int maximum_width = 0);
    int width(const std::string &, int size = 22);
    void wrap(const std::string &, int x, int y, int size, Color, int width, int lines = 3);
    void screenshot(const std::string &path);
    SDL_Window *window() const { return window_; }
    SDL_Renderer *renderer() const { return renderer_; }

  private:
    Uint32 subsystems_ = 0;
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    bool shared_font_ = false;
    struct Label {
        SDL_Texture *texture;
        int width, height;
    };
    std::map<std::string, Label> labels_;
    std::vector<SDL_Rect> clips_;
    const Label &label(const std::string &, int size);
};
} // namespace mineswitch
