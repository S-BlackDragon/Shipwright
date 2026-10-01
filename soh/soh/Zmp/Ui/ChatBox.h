#pragma once

// The message box of ZMP (phase 5b, docs/PETICIONES_ALEX.md "que se sienta nativo"): everything ZMP has to say goes
// to a small box at the bottom left of the picture instead of over the middle of the game. Presentation only: local
// settings (CVars gZmp.Chat.*), nothing of it reaches the simulation.

#include <string>
#include <vector>

namespace Zmp::Chat {

enum class Category { Players = 0, Items = 1, System = 2, Debug = 3, Count = 4 };

// A new message. `key`: messages with the same non-empty key are not repeated while the last one is younger than
// `quietSeconds` (a state that is drawn every frame posts once). `hold` multiplies how long the message stays.
void Post(Category category, const std::string& text, const std::string& key = "", double quietSeconds = 5.0,
          float hold = 1.0f);

// From another player (prepared for a chat between players; nothing sends these yet).
void PostFrom(const std::string& player, const std::string& text);

// Draws the box (foreground of the game picture). Call once per frame.
void Draw();

// The settings, inside the ZMP window.
void DrawSettings();

// What the box shows right now, oldest first (tests read it through the harness), and every message posted since
// the game started with its category (also the ones the filters hide).
std::vector<std::string> VisibleLines();
std::vector<std::pair<int, std::string>> History();

} // namespace Zmp::Chat
