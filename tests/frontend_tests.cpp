// Exercise the real application input dispatcher, menu navigation, PSIP model,
// and cancellable player probe without a USB device or desktop interaction.
#define main hibiki_frontend_main
#include "../src/sdl_frontend.cpp"
#undef main
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct AppRegression {
    static void run(SDL_Window* window, SDL_Renderer* renderer) {
        Options options;
        options.demo = true;
        App app(options);
        app.win_ = window;
        app.ren_ = renderer;
        app.app_event_ = SDL_RegisterEvents(1);
        auto font = std::filesystem::path(__FILE__).parent_path().parent_path() /
                    "res/font/VCR_OSD_MONO_1.001.ttf";
        require(app.osd_.init(renderer, font.string()), "Cannot initialize test OSD");
        app.scanned_ = {{11, hibiki::atsc_channel_freq_hz(11), 25.0f}};
        app.read_vct(11);
        require(app.tune_to(11, app.scanned_[0].freq), "Cannot tune demo fixture");
        app.poll_status();
        auto send = [&](SDL_Keycode key, SDL_Keymod mod = 0, bool repeat = false) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = key; event.key.mod = mod; event.key.repeat = repeat;
            app.handle_event(event);
        };
        auto page = [&] { return app.menu_.top() ? app.menu_.top()->title : std::string(); };
        require(scripted_key("?") == SDLK_F1, "Scripted question mark became an unshifted slash");
        auto volume = [&] { return std::lround(app.set_.volume * 100); };
        require(app.current_vc() && !app.current_vc()->name.empty(), "Station name missing");
        auto* show = app.psip_.at(app.current_vc()->source_id, app.psip_.now_utc());
        require(show && !show->title.empty() && !show->description.empty(), "Now-playing show missing");
        std::string title = vcr_text(show->title);
        int rf = app.channel_, program = app.current_program();
        send(SDLK_UP);
        require(volume() == 110, "Up did not raise playback volume");
        send(SDLK_DOWN);
        require(volume() == 100, "Down did not lower playback volume");
        require(app.channel_ == rf && app.current_program() == program, "Volume keys tuned a channel");

        send(SDLK_G);
        require(page() == "PROGRAM GUIDE", "G did not open the program guide");
        require(app.menu_.top()->items.size() == 3, "Guide lost virtual channels");
        require(app.menu_.top()->items[0].label.find(title.substr(0, 18)) != std::string::npos, "Guide lost show title");
        int selection = app.menu_.top()->sel;
        send(SDLK_DOWN);
        require(app.menu_.top()->sel == selection + 1 && volume() == 100, "Guide selection changed volume");
        send(SDLK_UP);
        send(SDLK_RETURN);
        require(page().find("11-1") == 0, "Guide did not open channel schedule");
        send(SDLK_RETURN);
        require(page() == "PROGRAM INFO", "Guide did not open program information");
        require(app.menu_.top()->body[0] == title, "Program info lost its show title");
        require(app.menu_.top()->body.size() >= 5, "Program info lost show description");
        send(SDLK_ESCAPE); send(SDLK_ESCAPE);
        require(page() == "PROGRAM GUIDE", "Guide back navigation failed");
        send(SDLK_PLUS);
        send(SDLK_PLUS, 0, true);
        require(volume() == 120 && app.current_program() == program, "Dedicated volume keys failed in guide");
        send(SDLK_MINUS); send(SDLK_MINUS);
        require(volume() == 100, "Guide volume-down failed");
        send(SDLK_SLASH, SDL_KMOD_SHIFT);
        require(page() == "KEYBOARD SHORTCUTS", "Shift-/ did not open shortcuts");
        send(SDLK_F1);
        require(page() == "PROGRAM GUIDE", "Shortcut help lost the underlying guide");
        uint64_t banner_until = app.banner_until_;
        send(SDLK_I);
        require(!app.menu_.open() && app.set_.info, "I did not reveal signal information from the guide");
        require(app.banner_until_ == banner_until, "I restarted the basic HUD banner");
        app.present();
        send(SDLK_I);
        require(!app.set_.info, "I did not hide signal information");

        send(SDLK_1); send(SDLK_1); send(SDLK_MINUS); send(SDLK_2); send(SDLK_RETURN);
        require(app.channel_ == 11 && app.current_program() == 2, "Virtual-channel entry 11-2 failed");
        require(volume() == 100, "Channel separator lowered volume");
        send(SDLK_PAGEUP);
        require(app.current_program() == 1, "PageUp did not select previous sub-channel");
        send(SDLK_PAGEDOWN);
        require(app.current_program() == 2, "PageDown did not select next sub-channel");
        send(SDLK_UP);
        require(app.current_program() == 2 && volume() == 110, "Up changed sub-channel instead of volume");
        send(SDLK_B);
        require(page() == "FIRMWARE SETUP", "B did not open firmware setup");
        send(SDLK_ESCAPE); send(SDLK_A);
        require(!app.menu_.open(), "A was stolen from audio-track selection");
        send(SDLK_F2);
        require(page() == "ABOUT HIBIKI", "F2 did not open About");
        send(SDLK_ESCAPE); send(SDLK_TAB);
        require(page() == "MENU", "Main menu was not restored");
        send(SDLK_RETURN);
        require(page() == "CHANNELS", "Main menu did not open channel list");
        send(SDLK_ESCAPE); send(SDLK_ESCAPE);

        // Check rendered regions as well as flags: a timed banner must not
        // keep the basic HUD visible after H turns it off.
        auto require_hud = [&](bool top_left, bool top_right, bool bottom_right) {
            app.osd_.begin_frame();
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            app.draw_hud();
            auto pixels = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>(
                SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
            require(pixels != nullptr, "Cannot read HUD pixels");
            auto visible = [&](int x0, int y0, int x1, int y1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        Uint8 r = 0, g = 0, b = 0, a = 0;
                        require(SDL_ReadSurfacePixel(pixels.get(), x, y, &r, &g, &b, &a), "Cannot inspect HUD pixel");
                        if (r || g || b) return true;
                    }
                }
                return false;
            };
            int w = pixels->w, h = pixels->h;
            require(visible(0, 0, w / 2, h / 2) == top_left, "Top-left channel HUD visibility is wrong");
            require(visible(w / 2, 0, w, h / 2) == top_right, "Top-right clock HUD visibility is wrong");
            require(visible(w / 2, h * 2 / 3, w, h) == bottom_right, "Bottom-right signal info visibility is wrong");
            app.osd_.end_frame();
        };
        app.banner_until_ = app.vol_until_ = 0;
        require_hud(false, false, false);
        send('H', SDL_KMOD_CAPS);
        require(app.set_.hud && !app.set_.info, "Caps Lock broke independent HUD toggle");
        require_hud(true, true, false);
        send(SDLK_H, SDL_KMOD_GUI);
        send(SDLK_H, 0, true);
        require(app.set_.hud, "Native shortcuts or repeats toggled HUD");
        send('I', SDL_KMOD_CAPS);
        require(app.set_.hud && app.set_.info, "I changed basic HUD state");
        require_hud(true, true, true);
        send(SDLK_I, SDL_KMOD_GUI);
        send(SDLK_I, 0, true);
        require(app.set_.info, "Native shortcuts or repeats toggled signal info");
        app.show_banner();
        send(SDLK_H);
        require(!app.set_.hud && app.banner_until_ == 0 && app.set_.info, "H did not immediately hide only the basic HUD");
        require_hud(false, false, true);
        send(SDLK_I);
        require_hud(false, false, false);
        send(SDLK_I);
        require_hud(false, false, true);
        send(SDLK_H);
        send(SDLK_I);
        require_hud(true, true, false);
        app.show_banner();
        send(SDLK_H);
        require_hud(false, false, false);
        app.show_banner();
        require_hud(true, true, false);
        send(SDLK_H);
        require_hud(false, false, false);

        app.open_page(app.feature_page());
        auto select_feature = [&](const std::string& label) {
            auto* feature = app.menu_.top();
            auto it = std::find_if(feature->items.begin(), feature->items.end(),
                                   [&](const osd::Item& item) { return item.label == label; });
            require(it != feature->items.end(), "Missing overlay preference");
            feature->sel = (int)(it - feature->items.begin());
        };
        select_feature("HUD");
        send(SDLK_RIGHT);
        require(app.set_.hud && !app.set_.info, "HUD menu setting enabled signal info");
        send(SDLK_LEFT);
        app.opt_.no_hud = true;
        app.st_ms_ = SDL_GetTicks();
        select_feature("SIGNAL INFO");
        send(SDLK_RIGHT);
        require(app.set_.info && !app.set_.hud && !app.opt_.no_hud && app.st_ms_ == 0,
                "Signal info menu setting did not independently enable status polling");
        send(SDLK_I);
        require(!app.menu_.open() && !app.set_.info, "I did not turn off the menu-enabled signal panel");
        app.opt_.no_hud = true;
        send(SDLK_I);
        require(!app.opt_.no_hud && app.st_ms_ == 0, "I did not restore polling after --no-hud");
        app.st_ok_ = false;
        require_hud(false, false, true);
        app.opt_.ts_file = "capture.ts";
        require_hud(false, false, true);
        send(SDLK_I);
        require_hud(false, false, false);
        app.opt_.ts_file.clear();

        SDL_Event native{};
        native.type = app.app_event_;
        native.user.code = static_cast<int>(hibiki::AppAction::shortcuts);
        app.handle_event(native);
        require(page() == "KEYBOARD SHORTCUTS", "Native help action did not reach OSD");
        app.present();
    }
};
}

int main() {
    try {
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) && TTF_Init(), "SDL test initialization failed");
        SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
        require(SDL_CreateWindowAndRenderer("Hibiki regression", 1280, 720, 0, &window, &renderer), "Cannot create test renderer");
        hibiki::g_running = true;
        AppRegression::run(window, renderer);
        player::Player player;
        int pumps = 0;
        auto started = std::chrono::steady_clock::now();
        bool opened = player.open([](uint8_t*, int) { SDL_Delay(5); return 0; }, renderer, -1,
                                  [&] { return ++pumps < 5; });
        require(!opened && pumps >= 5, "Empty stream probe did not cancel through UI pump");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "Probe cancellation stalled UI");
        player.close(); player.close();
        SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
        TTF_Quit(); SDL_Quit();
        std::cout << "Playback volume, guide navigation, HUD/info toggles, shortcuts, and probe cancellation passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
