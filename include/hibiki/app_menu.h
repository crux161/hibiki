#pragma once
#include <cstdint>

namespace hibiki {
enum class AppAction : int { about = 1, shortcuts, firmware, rescan };
#ifdef __APPLE__
void install_app_menu(uint32_t event_type);
#else
inline void install_app_menu(uint32_t) {}
#endif
} // namespace hibiki
