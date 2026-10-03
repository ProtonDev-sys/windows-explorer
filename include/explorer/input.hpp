#pragma once

#include "explorer/commands.hpp"
#include <optional>

namespace explorer {

// Receives virtual-key codes and an explicit modifier snapshot. Editing covers
// both host textboxes and native Shell edit controls, including inline rename.
// An empty result leaves the key to the focused control or Windows.
std::optional<Command> shortcutCommand(UINT key, bool control, bool shift,
                                       bool alt, bool editing) noexcept;

} // namespace explorer
