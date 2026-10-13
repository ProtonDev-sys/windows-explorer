#pragma once

#include "explorer/commands.hpp"
#include <shellapi.h>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace explorer {

// Stable tokens are persisted instead of numeric enum values, which may shift
// as commands are added. The host owns native images, tooltips and enabled state.
struct QuickAccessCommand {
    Command command;
    std::wstring_view label;
    SHSTOCKICONID icon;
    std::string_view token;
};

std::span<const QuickAccessCommand> quickAccessCommands() noexcept;
const QuickAccessCommand* quickAccessCommand(Command command) noexcept;

class QuickAccessToolbar {
public:
    static constexpr std::size_t MaximumCommands = 20;
    static constexpr std::size_t Append = std::numeric_limits<std::size_t>::max();

    QuickAccessToolbar();
    const std::vector<Command>& commands() const noexcept;
    bool contains(Command command) const noexcept;
    // Index is the insertion position [0, size], or Append. Invalid operations
    // return false and leave the toolbar unchanged, including its placement.
    bool add(Command command, std::size_t index = Append);
    bool remove(Command command) noexcept;
    // Destination index is the final position [0, size - 1].
    bool move(Command command, std::size_t destinationIndex) noexcept;
    void clear() noexcept;
    void reset();
    bool belowRibbon() const noexcept;
    void setBelowRibbon(bool below) noexcept;

private:
    std::vector<Command> commands_;
    bool belowRibbon_ = false;
};

// App-owned settings only. Missing, oversized, invalid UTF-8 or unsupported
// version files return defaults. Valid lists discard unknown/duplicate tokens
// and keep their order up to MaximumCommands. An explicit empty list is valid;
// a nonempty list with no recognized commands falls back to default commands.
QuickAccessToolbar loadQuickAccessToolbar(const std::filesystem::path& path);
bool saveQuickAccessToolbar(const std::filesystem::path& path,
                           const QuickAccessToolbar& toolbar);

} // namespace explorer
