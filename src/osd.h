// hibiki VCR-style on-screen display.
//
// A small immediate-mode renderer for the "VCR OSD" look: chunky monospace
// text (VCR OSD Mono), the solid-blue set-up menu with a yellow selection bar,
// and the VHS playback HUD (white text with a slight chroma split and a dark
// edge so it reads over any picture).
//
// Text is rasterised once per (font, string) as a white texture and tinted at
// draw time with the texture colour mod, so redrawing a menu every frame costs
// no TTF work. Font sizes follow the window height, so the OSD keeps the same
// proportions at 720p, 1080p or fullscreen.

#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace osd {

// ---- palette ---------------------------------------------------------------
constexpr SDL_Color kVcrBlue   {0, 0, 168, 255};    // set-up menu background
constexpr SDL_Color kVcrYellow {250, 232, 60, 255}; // menu text / selection bar
constexpr SDL_Color kVcrDim    {120, 120, 190, 255};// disabled item on blue
constexpr SDL_Color kWhite     {238, 238, 255, 255};// HUD text
constexpr SDL_Color kRed       {255, 70, 70, 255};
constexpr SDL_Color kGreen     {90, 255, 120, 255};
constexpr SDL_Color kBlack     {0, 0, 0, 255};

enum class Font { Small, Menu, Big };
enum class Align { Left, Center, Right };

// Text styles: Flat = plain tinted text; Vhs = HUD text with an offset dark
// edge and a red/blue chroma split.
enum class Style { Flat, Vhs };

class Renderer {
public:
    ~Renderer() { release(); }
    bool init(SDL_Renderer* ren, const std::string& font_path);
    // Call once per frame before drawing; resizes fonts if the output changed.
    void begin_frame();
    void end_frame();   // evicts text textures not used recently
    void release();     // free fonts/textures (call before TTF_Quit)

    int width() const { return ow_; }
    int height() const { return oh_; }
    float cell_w(Font f) const;   // monospace advance
    float line_h(Font f) const;   // recommended line pitch
    float text_w(Font f, const std::string& s);

    // Draw s at (x, y) = top edge; returns the drawn width.
    float text(Font f, const std::string& s, float x, float y, SDL_Color c,
               Align a = Align::Left, Style st = Style::Flat, uint8_t alpha = 255);
    void rect(float x, float y, float w, float h, SDL_Color c, uint8_t alpha = 255);
    void frame(float x, float y, float w, float h, float t, SDL_Color c, uint8_t alpha = 255);
    // Horizontal segmented meter (VCR style: lit cells then dim cells).
    void meter(float x, float y, float w, float h, int segments, float frac,
               SDL_Color on, SDL_Color off);
    // Darken every other line pair across the whole output.
    void scanlines(uint8_t alpha);

    SDL_Renderer* sdl() const { return ren_; }
    bool ok() const { return fonts_[0] != nullptr; }

private:
    struct Entry { SDL_Texture* tex; int w, h; uint64_t used; };
    Entry* get(Font f, const std::string& s);
    void flush();

    SDL_Renderer* ren_ = nullptr;
    TTF_Font* fonts_[3] = {nullptr, nullptr, nullptr};
    float px_[3] = {0, 0, 0};
    float cellw_[3] = {0, 0, 0};
    int ow_ = 0, oh_ = 0;
    uint64_t frame_no_ = 0;
    std::unordered_map<std::string, Entry> cache_;
    SDL_Texture* scan_tex_ = nullptr;
};

// ---- menu model --------------------------------------------------------------
// A menu page is a titled list of items. An item can open a sub-page / run an
// action (Enter or Right), cycle a value (Left/Right), or be a disabled
// placeholder for a feature that is not implemented yet.
struct Item {
    std::string label;
    std::function<std::string()> value;      // shown after ':' (optional)
    std::function<void(int dir)> adjust;     // Left/Right cycles the value
    std::function<void()> activate;          // Enter / Right
    bool enabled = true;
    std::string note;                        // right-aligned tag, e.g. "SOON"
    bool marked = false;                     // leading marker (e.g. now playing)
};

struct Page {
    std::string title;
    std::vector<Item> items;
    std::vector<std::string> body;           // free text above the items
    std::vector<std::string> hints;          // footer; default SELECT/SET/END
    // Extra key handling for the page (e.g. Space = favourite). Return true
    // if the key was consumed.
    std::function<bool(SDL_Keycode)> on_key;
    // Rebuild items from live state (called when the page is shown and after
    // every action, so values stay current).
    std::function<void(Page&)> build;
    int sel = 0;
    int scroll = 0;
};

class Menu {
public:
    bool open() const { return !stack_.empty(); }
    void push(Page p);
    void pop();
    void close() { stack_.clear(); }
    Page* top() { return stack_.empty() ? nullptr : &stack_.back(); }
    // Returns true if the key was consumed by the menu.
    bool key(SDL_Keycode k);
    void draw(Renderer& r);
    void refresh();  // rebuild the top page
private:
    std::vector<Page> stack_;
};

} // namespace osd
