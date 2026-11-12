#include "ui/choice.hpp"

#include "gpu/vulkan_renderer.hpp"
#include "ui/layer.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <utility>

namespace portablekit::ui {
namespace {

struct Choice {
    bool open{};
    bool focus_pending{};
    ChoiceRequest request;
    ChoiceDone on_done;
};

Choice &choice() {
    static Choice state;
    return state;
}

void finish(std::optional<std::size_t> chosen) {
    Choice &c = choice();
    if (!c.open) return;
    c.open = false;
    Layer &layer = Layer::get();
    layer.set_interactive(false);
    layer.renderer().set_game_input(true);
    layer.renderer().hold_frame(false);
    ChoiceDone done = std::move(c.on_done);
    c.on_done = nullptr;
    if (done) done(chosen);
}

} // namespace

bool open_game_choice(ChoiceRequest request, ChoiceDone on_done) {
    Layer &layer = Layer::get();
    if (!layer.attached()) return false;
    if (choice().open) finish(std::nullopt);
    layer.renderer().hold_frame(true);
    layer.renderer().set_game_input(false);
    layer.set_interactive(true);
    choice() = Choice{true, true, std::move(request), std::move(on_done)};
    return true;
}

bool choice_open() { return choice().open; }

void choice_frame() {
    Choice &c = choice();
    if (!c.open) return;
    Layer &layer = Layer::get();
    // The menu does not open over the list.
    (void)layer.take_menu_toggle();
    std::optional<std::size_t> chosen;
    begin_panel("##choice", c.request.title, c.request.subtitle, true);
    begin_content();
    for (std::size_t i = 0; i < c.request.items.size(); ++i) {
        const ChoiceItem &item = c.request.items[i];
        if (c.focus_pending && i == c.request.focus) focus_next_row();
        ImGui::PushID(static_cast<int>(i));
        if (list_row("##item", item.name, item.detail, ListIcon::File, !item.disabled) && !item.disabled) chosen = i;
        ImGui::PopID();
    }
    c.focus_pending = false;
    touch_scroll();
    begin_footer();
    hints({{Control::Confirm, "Choose"}, {Control::Back, "Cancel"}});
    end_panel();
    const ImGuiKey cancel = layer.confirm_south() ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    if (chosen) finish(chosen);
    else if (layer.take_back() || ImGui::IsKeyPressed(cancel, false)) finish(std::nullopt);
}

} // namespace portablekit::ui
