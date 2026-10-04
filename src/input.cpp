#include "explorer/input.hpp"
#include <array>

namespace explorer {

std::optional<Command> shortcutCommand(UINT key, bool control, bool shift,
                                       bool alt, bool editing) noexcept {
    constexpr unsigned Control = 1;
    constexpr unsigned Shift = 2;
    constexpr unsigned Alt = 4;
    const unsigned modifiers = (control ? Control : 0) |
                               (shift ? Shift : 0) |
                               (alt ? Alt : 0);

    // Exact modifier matches protect system chords such as Ctrl+Alt+Delete
    // and Alt+F4, and keep text editing shortcuts out of file-command routing.
    switch (modifiers) {
    case 0:
        if (key == VK_F4) return Address;
        if (key == VK_F3) return FocusSearch;
        if (key == VK_F5) return Refresh;
        if (key == VK_F6) return FocusNext;
        if (key == VK_F11) return Fullscreen;
        if (editing) return std::nullopt;
        if (key == VK_BACK) return Back;
        if (key == VK_F2) return Rename;
        if (key == VK_DELETE) return Delete;
        break;
    case Control:
        if (key == 'L') return Address;
        if (key == 'E' || key == 'F') return FocusSearch;
        if (key == 'N') return NewWindow;
        if (key == 'W') return Close;
        if (key == VK_F1) return Collapse;
        if (editing) return std::nullopt;
        if (key == 'C') return Copy;
        if (key == 'X') return Cut;
        if (key == 'V') return Paste;
        if (key == 'A') return SelectAll;
        break;
    case Shift:
        if (key == VK_F6) return FocusPrevious;
        if (!editing && key == VK_DELETE) return PermanentDelete;
        break;
    case Control | Shift:
        if (editing) return std::nullopt;
        if (key == 'N') return NewFolder;
        if (key >= '1' && key <= '8')
            return static_cast<Command>(ViewFirst + key - '1');
        break;
    case Alt:
        if (key == 'D') return Address;
        if (key == VK_LEFT) return Back;
        if (key == VK_RIGHT) return Forward;
        if (key == VK_UP) return Up;
        if (key == 'P') return PreviewPane;
        if (!editing && key == VK_RETURN) return Properties;
        break;
    case Alt | Shift:
        if (key == 'P') return DetailsPane;
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<FocusRegion> cycleFocusRegion(std::optional<FocusRegion> current,
                                           bool backwards,
                                           const FocusAvailability& available) noexcept {
    constexpr int regionCount = 5;
    const std::array<bool, regionCount> enabled{
        available.address, available.search, available.folderView,
        available.commandBand, available.navigation};
    int index = backwards ? 0 : regionCount - 1;
    if (current) {
        const int candidate = static_cast<int>(*current);
        if (candidate >= 0 && candidate < regionCount) index = candidate;
    }

    for (int attempted = 0; attempted < regionCount; ++attempted) {
        index = (index + (backwards ? regionCount - 1 : 1)) % regionCount;
        if (enabled[static_cast<std::size_t>(index)])
            return static_cast<FocusRegion>(index);
    }
    return std::nullopt;
}

} // namespace explorer
