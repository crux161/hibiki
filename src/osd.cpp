// hibiki VCR-style on-screen display (see osd.h).

#include "osd.h"

#include <algorithm>
#include <cmath>

namespace osd {

namespace {
// Font pixel size as a fraction of the output height. A VCR menu fits roughly
// a dozen rows on screen; the HUD text is a little smaller, the channel
// number much bigger.
constexpr float kFontFrac[3] = {1.0f / 24.0f, 1.0f / 21.0f, 1.0f / 9.0f};
constexpr float kLinePitch = 1.18f;
constexpr uint64_t kEvictFrames = 240;

size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Keep the first `chars` code points (the OSD font is monospace).
std::string utf8_cut(const std::string& s, size_t chars) {
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (n == chars) break;
            ++n;
        }
    }
    return s.substr(0, i);
}
} // namespace

void Renderer::release() {
    flush();
    if (scan_tex_) { SDL_DestroyTexture(scan_tex_); scan_tex_ = nullptr; }
    for (auto*& f : fonts_) if (f) { TTF_CloseFont(f); f = nullptr; }
}

bool Renderer::init(SDL_Renderer* ren, const std::string& font_path) {
    ren_ = ren;
    for (int i = 0; i < 3; ++i) {
        fonts_[i] = TTF_OpenFont(font_path.c_str(), 24.0f);
        if (!fonts_[i]) return false;
    }
    // 1x2 scanline tile: transparent row, black row (strength via alpha mod).
    SDL_Surface* s = SDL_CreateSurface(1, 2, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        auto* px = static_cast<uint32_t*>(s->pixels);
        const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(s->format);
        px[0] = SDL_MapRGBA(d, nullptr, 0, 0, 0, 0);
        px[s->pitch / 4] = SDL_MapRGBA(d, nullptr, 0, 0, 0, 255);
        scan_tex_ = SDL_CreateTextureFromSurface(ren, s);
        SDL_DestroySurface(s);
        if (scan_tex_) {
            SDL_SetTextureScaleMode(scan_tex_, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(scan_tex_, SDL_BLENDMODE_BLEND);
        }
    }
    begin_frame();
    return true;
}

void Renderer::flush() {
    for (auto& kv : cache_) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
}

void Renderer::begin_frame() {
    ++frame_no_;
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(ren_, &w, &h);
    if (w == ow_ && h == oh_) return;
    ow_ = w; oh_ = h;
    flush();
    for (int i = 0; i < 3; ++i) {
        if (!fonts_[i]) continue;
        px_[i] = std::max(10.0f, std::round(h * kFontFrac[i]));
        TTF_SetFontSize(fonts_[i], px_[i]);
        int cw = 0, ch = 0;
        TTF_GetStringSize(fonts_[i], "M", 0, &cw, &ch);
        cellw_[i] = (float)cw;
    }
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
}

void Renderer::end_frame() {
    if (frame_no_ % 60) return;
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (frame_no_ - it->second.used > kEvictFrames) {
            SDL_DestroyTexture(it->second.tex);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

float Renderer::cell_w(Font f) const { return cellw_[(int)f]; }
float Renderer::line_h(Font f) const { return px_[(int)f] * kLinePitch; }

Renderer::Entry* Renderer::get(Font f, const std::string& s) {
    TTF_Font* font = fonts_[(int)f];
    if (!font || s.empty()) return nullptr;
    std::string key;
    key.reserve(s.size() + 1);
    key.push_back((char)('0' + (int)f));
    key += s;
    auto it = cache_.find(key);
    if (it != cache_.end()) { it->second.used = frame_no_; return &it->second; }
    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), 0, SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren_, surf);
    Entry e{tex, surf->w, surf->h, frame_no_};
    SDL_DestroySurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return &cache_.emplace(key, e).first->second;
}

float Renderer::text_w(Font f, const std::string& s) {
    Entry* e = get(f, s);
    return e ? (float)e->w : 0.0f;
}

float Renderer::text(Font f, const std::string& s, float x, float y, SDL_Color c,
                     Align a, Style st, uint8_t alpha) {
    Entry* e = get(f, s);
    if (!e) return 0.0f;
    float w = (float)e->w, h = (float)e->h;
    if (a == Align::Center) x -= w / 2;
    else if (a == Align::Right) x -= w;
    x = std::round(x); y = std::round(y);
    auto blit = [&](float dx, float dy, SDL_Color col, uint8_t al) {
        SDL_SetTextureColorMod(e->tex, col.r, col.g, col.b);
        SDL_SetTextureAlphaMod(e->tex, al);
        SDL_FRect d{x + dx, y + dy, w, h};
        SDL_RenderTexture(ren_, e->tex, nullptr, &d);
    };
    if (st == Style::Vhs) {
        // Black edge all round (like a VCR character generator) so the text
        // reads on bright pictures, then a slight red/blue chroma split.
        float o = std::max(1.0f, std::round(px_[(int)f] / 14.0f));
        const float ring[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (const auto& d : ring) blit(d[0] * o, d[1] * o, kBlack, (uint8_t)(alpha * 0.85f));
        blit(o * 1.5f, o * 1.5f, kBlack, (uint8_t)(alpha * 0.5f));
        blit(-o, 0, SDL_Color{255, 40, 60, 255}, (uint8_t)(alpha * 0.45f));
        blit(o, 0, SDL_Color{40, 90, 255, 255}, (uint8_t)(alpha * 0.45f));
    }
    blit(0, 0, c, alpha);
    return w;
}

void Renderer::rect(float x, float y, float w, float h, SDL_Color c, uint8_t alpha) {
    SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, alpha);
    SDL_FRect r{std::round(x), std::round(y), std::round(w), std::round(h)};
    SDL_RenderFillRect(ren_, &r);
}

void Renderer::frame(float x, float y, float w, float h, float t, SDL_Color c, uint8_t alpha) {
    rect(x, y, w, t, c, alpha);
    rect(x, y + h - t, w, t, c, alpha);
    rect(x, y + t, t, h - 2 * t, c, alpha);
    rect(x + w - t, y + t, t, h - 2 * t, c, alpha);
}

void Renderer::meter(float x, float y, float w, float h, int segments, float frac,
                     SDL_Color on, SDL_Color off) {
    if (segments <= 0) return;
    frac = std::clamp(frac, 0.0f, 1.0f);
    int lit = (int)std::lround(frac * segments);
    float pitch = w / segments;
    float seg = std::max(1.0f, pitch * 0.62f);
    for (int i = 0; i < segments; ++i) {
        // Unlit cells are drawn as a short stub, like "|||||||--" on tape.
        if (i < lit) rect(x + i * pitch, y, seg, h, on);
        else rect(x + i * pitch, y + h * 0.42f, seg, h * 0.16f, off);
    }
}

void Renderer::scanlines(uint8_t alpha) {
    if (!scan_tex_ || alpha == 0) return;
    SDL_SetTextureAlphaMod(scan_tex_, alpha);
    float scale = std::max(1.0f, std::round(oh_ / 480.0f));
    SDL_FRect dst{0, 0, (float)ow_, (float)oh_};
    SDL_RenderTextureTiled(ren_, scan_tex_, nullptr, scale, &dst);
}

// ---------------------------------------------------------------------------
void Menu::push(Page p) {
    stack_.push_back(std::move(p));
    refresh();
}

void Menu::pop() {
    if (!stack_.empty()) stack_.pop_back();
    if (!stack_.empty()) refresh();
}

void Menu::refresh() {
    Page* p = top();
    if (!p) return;
    if (p->build) {
        auto b = p->build;   // build may not outlive a reallocation of stack_
        b(*p);
    }
    p = top();
    if (!p) return;
    int n = (int)p->items.size();
    p->sel = n ? std::clamp(p->sel, 0, n - 1) : 0;
}

bool Menu::key(SDL_Keycode k) {
    Page* p = top();
    if (!p) return false;
    if (p->on_key) {
        auto fn = p->on_key;
        if (fn(k)) { refresh(); return true; }
        p = top();
        if (!p) return true;
    }
    int n = (int)p->items.size();
    auto run = [&](std::function<void()> fn) { if (fn) fn(); refresh(); };
    switch (k) {
    case SDLK_UP:       if (n) p->sel = (p->sel - 1 + n) % n; return true;
    case SDLK_DOWN:     if (n) p->sel = (p->sel + 1) % n; return true;
    case SDLK_PAGEUP:   if (n) p->sel = std::max(0, p->sel - 8); return true;
    case SDLK_PAGEDOWN: if (n) p->sel = std::min(n - 1, p->sel + 8); return true;
    case SDLK_HOME:     p->sel = 0; return true;
    case SDLK_END:      if (n) p->sel = n - 1; return true;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        pop();
        return true;
    case SDLK_LEFT:
    case SDLK_RIGHT:
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
        int dir = (k == SDLK_LEFT) ? -1 : 1;
        if (!n) { if (k == SDLK_LEFT) pop(); return true; }
        Item it = p->items[(size_t)p->sel];   // copy: actions may rebuild items
        if (!it.enabled) return true;
        if (it.adjust && k != SDLK_RETURN && k != SDLK_KP_ENTER) {
            auto adj = it.adjust;
            run([adj, dir] { adj(dir); });
        } else if (k == SDLK_LEFT) {
            pop();
        } else if (it.activate) {
            run(it.activate);
        } else if (it.adjust) {
            auto adj = it.adjust;
            run([adj] { adj(1); });
        }
        return true;
    }
    default:
        return false;
    }
}

void Menu::draw(Renderer& r) {
    Page* p = top();
    if (!p) return;
    const float W = (float)r.width(), H = (float)r.height();
    const Font F = Font::Menu;
    const float cw = r.cell_w(F), lh = r.line_h(F);
    if (cw <= 0) return;

    r.rect(0, 0, W, H, kVcrBlue);

    // Content block: up to 36 cells wide, centred, like a VCR character grid.
    int cols = std::min(36, (int)(W * 0.9f / cw));
    float x0 = std::round((W - cols * cw) / 2);
    float y = std::round(H * 0.08f);

    // ---------- TITLE ----------
    std::string t = " " + p->title + " ";
    int dash = std::max(2, (cols - (int)t.size()) / 2);
    std::string title = std::string((size_t)dash, '-') + t + std::string((size_t)dash, '-');
    r.text(F, title, W / 2, y, kVcrYellow, Align::Center);
    y += lh * 1.5f;

    for (const auto& line : p->body) {
        r.text(F, line, x0 + cw, y, kVcrYellow);
        y += lh;
    }
    if (!p->body.empty()) y += lh * 0.5f;

    // Footer hints.
    std::vector<std::string> hints = p->hints;
    if (hints.empty())
        hints = {"SELECT:\xE2\x96\xB2\xE2\x96\xBC KEY", "SET   :\xE2\x96\xBA KEY",
                 "END   :ESC KEY"};
    float footer_y = H - H * 0.06f - lh * hints.size();

    // Items (scrolling window).
    int n = (int)p->items.size();
    int rows = std::max(1, (int)((footer_y - lh * 0.5f - y) / lh));
    if (p->sel < p->scroll) p->scroll = p->sel;
    if (p->sel >= p->scroll + rows) p->scroll = p->sel - rows + 1;
    p->scroll = std::clamp(p->scroll, 0, std::max(0, n - rows));

    size_t label_w = 0;
    for (const auto& it : p->items)
        if (it.value) label_w = std::max(label_w, it.label.size());

    const float ix = x0 + cw * 2;
    const float right = x0 + cols * cw;
    const float list_top = y;
    for (int i = p->scroll; i < std::min(n, p->scroll + rows); ++i) {
        const Item& it = p->items[(size_t)i];
        bool sel = (i == p->sel);
        std::string s = it.label;
        if (it.value) {
            s.resize(std::max(label_w, s.size()), ' ');
            std::string v = it.value();
            if (sel && it.adjust && it.enabled)
                s += ":\xE2\x97\x84" + v + "\xE2\x96\xBA";
            else
                s += ":" + v;
        }
        // Clip so a long label never runs into the right-hand note column.
        size_t room = (size_t)std::max(1.0f, (right - ix) / cw) -
                      (it.note.empty() ? 0 : utf8_len(it.note) + 1);
        if (utf8_len(s) > room) s = utf8_cut(s, room);
        SDL_Color fg = it.enabled ? kVcrYellow : kVcrDim;
        if (sel) {
            float tw = r.text_w(F, s);
            r.rect(ix - cw * 0.25f, y, tw + cw * 0.5f, lh * 0.98f,
                   it.enabled ? kVcrYellow : kVcrDim);
            fg = kVcrBlue;
        }
        if (it.marked) r.text(F, "\xE2\x96\xBA", x0 + cw * 0.5f, y + lh * 0.06f, kVcrYellow);
        r.text(F, s, ix, y + lh * 0.06f, fg);
        if (!it.note.empty())
            r.text(F, it.note, right, y + lh * 0.06f, it.enabled ? kVcrYellow : kVcrDim,
                   Align::Right);
        y += lh;
    }
    if (n == 0 && p->body.empty()) {
        r.text(F, "(EMPTY)", ix, y, kVcrDim);
    }
    // Scroll arrows at the right edge of the list window.
    if (p->scroll > 0)
        r.text(F, "\xE2\x96\xB2", right + cw * 0.5f, list_top, kVcrYellow);
    if (p->scroll + rows < n)
        r.text(F, "\xE2\x96\xBC", right + cw * 0.5f, list_top + lh * (rows - 1), kVcrYellow);

    float fy = footer_y;
    for (const auto& h : hints) {
        r.text(F, h, x0, fy, kVcrYellow);
        fy += lh;
    }
    r.scanlines(60);
}

} // namespace osd
