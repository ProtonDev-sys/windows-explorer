#pragma once

#include "explorer/commands.hpp"
#include <optional>

namespace explorer {

// Receives virtual-key codes and an explicit modifier snapshot. Editing covers
// both host textboxes and native Shell edit controls, including inline rename.
// An empty result leaves the key to the focused control or Windows.
std::optional<Command> shortcutCommand(UINT key, bool control, bool shift,
                                       bool alt, bool editing) noexcept;

enum class FocusRegion { Address, Search, FolderView, CommandBand, Navigation };

struct FocusAvailability {
    bool address = true;
    bool search = true;
    bool folderView = true;
    bool commandBand = true;
    bool navigation = true;
};

// Cycles address -> search -> folder view -> command band -> navigation and
// skips unavailable regions. An unknown current region starts at the first
// available region in the chosen direction. No available region returns empty.
std::optional<FocusRegion> cycleFocusRegion(std::optional<FocusRegion> current,
                                           bool backwards,
                                           const FocusAvailability& available) noexcept;

} // namespace explorer
