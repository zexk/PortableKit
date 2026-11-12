#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A list to choose one entry from, drawn over the running game: what stands in
// for a PSP system dialog that lists things, such as the save-data dialog's
// list of saves. The game keeps running behind it, as it does behind the
// PSP's own dialog, but reads a neutral pad, and the window keeps the frame
// that was on screen.
namespace portablekit::ui {

struct ChoiceItem {
    std::string name;
    std::string detail;  // on the right, dimmed
    bool disabled{};     // shown, but cannot be chosen
};

struct ChoiceRequest {
    std::string title;
    std::string subtitle;
    std::vector<ChoiceItem> items;
    std::size_t focus{};  // the entry focused when it opens
};

// Called once when the list closes: the entry chosen, or nullopt after Back.
using ChoiceDone = std::function<void(std::optional<std::size_t>)>;

// False when there is no interface to draw it in.
bool open_game_choice(ChoiceRequest request, ChoiceDone on_done);
[[nodiscard]] bool choice_open();
// Builds this frame of the list and handles its input. Call inside an
// interface frame.
void choice_frame();

} // namespace portablekit::ui
