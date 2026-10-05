#include "explorer/input.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using explorer::Command;

struct Chord {
    UINT key;
    bool control;
    bool shift;
    bool alt;
};

unsigned assertions = 0;

void expect(const Chord& chord, bool editing, std::optional<Command> expected,
            const char* description) {
    ++assertions;
    const auto actual = explorer::shortcutCommand(chord.key, chord.control,
                                                  chord.shift, chord.alt, editing);
    if (actual != expected) {
        throw std::runtime_error(std::string(description) +
            " (key=" + std::to_string(chord.key) +
            ", Ctrl=" + std::to_string(chord.control) +
            ", Shift=" + std::to_string(chord.shift) +
            ", Alt=" + std::to_string(chord.alt) +
            ", editing=" + std::to_string(editing) + ")");
    }
}

void navigationAndWindowChords() {
    struct Mapping { Chord chord; Command command; };
    constexpr std::array mappings{
        Mapping{{VK_F4, false, false, false}, explorer::AddressList},
        Mapping{{'L', true, false, false}, explorer::Address},
        Mapping{{'D', false, false, true}, explorer::Address},
        Mapping{{VK_F3, false, false, false}, explorer::FocusSearch},
        Mapping{{'E', true, false, false}, explorer::FocusSearch},
        Mapping{{'F', true, false, false}, explorer::FocusSearch},
        Mapping{{'E', true, true, false}, explorer::ExpandAncestors},
        Mapping{{VK_LEFT, false, false, true}, explorer::Back},
        Mapping{{VK_RIGHT, false, false, true}, explorer::Forward},
        Mapping{{VK_UP, false, false, true}, explorer::Up},
        Mapping{{'N', true, false, false}, explorer::NewWindow},
        Mapping{{'W', true, false, false}, explorer::Close},
        Mapping{{VK_F1, true, false, false}, explorer::Collapse},
        Mapping{{VK_F5, false, false, false}, explorer::Refresh},
        Mapping{{'R', true, false, false}, explorer::Refresh},
        Mapping{{VK_F6, false, false, false}, explorer::FocusNext},
        Mapping{{VK_F6, false, true, false}, explorer::FocusPrevious},
        Mapping{{VK_F11, false, false, false}, explorer::Fullscreen},
        Mapping{{'P', false, false, true}, explorer::PreviewPane},
        Mapping{{'P', false, true, true}, explorer::DetailsPane}
    };
    for (const auto& mapping : mappings) {
        expect(mapping.chord, false, mapping.command, "Navigation/window chord must work in the view");
        expect(mapping.chord, true, mapping.command, "Explicit navigation/window chord must work while editing");
    }
}

void fileCommandsAndRenameSafety() {
    struct Mapping { Chord chord; Command command; };
    constexpr std::array mappings{
        Mapping{{VK_BACK, false, false, false}, explorer::Back},
        Mapping{{VK_F2, false, false, false}, explorer::Rename},
        Mapping{{'N', true, true, false}, explorer::NewFolder},
        Mapping{{'C', true, false, false}, explorer::Copy},
        Mapping{{'X', true, false, false}, explorer::Cut},
        Mapping{{'V', true, false, false}, explorer::Paste},
        Mapping{{'A', true, false, false}, explorer::SelectAll},
        Mapping{{'D', true, false, false}, explorer::Delete},
        Mapping{{VK_ADD, true, false, false}, explorer::SizeColumns},
        Mapping{{VK_DELETE, false, false, false}, explorer::Delete},
        Mapping{{VK_DELETE, false, true, false}, explorer::PermanentDelete},
        Mapping{{VK_RETURN, false, false, true}, explorer::Properties}
    };
    for (const auto& mapping : mappings) {
        expect(mapping.chord, false, mapping.command, "Expected file command is missing");
        expect(mapping.chord, true, std::nullopt, "Editing must not dispatch a file command");
    }

    // A native rename edit and both host edits share the editing contract.
    // Exhaust modifiers so Delete/Backspace or a text clipboard key cannot
    // accidentally become a destructive command through another branch.
    constexpr std::array<UINT, 8> editKeys{VK_BACK, VK_DELETE, VK_F2, VK_RETURN, 'C', 'X', 'V', 'A'};
    for (const UINT key : editKeys) {
        for (unsigned modifiers = 0; modifiers < 8; ++modifiers) {
            expect({key, (modifiers & 1) != 0, (modifiers & 2) != 0, (modifiers & 4) != 0},
                   true, std::nullopt, "Native text editing key must remain with its edit control");
        }
    }
}

void modifierConflictsAndSystemKeys() {
    // These previously matched broad checks for F4, Ctrl, or Delete.
    constexpr std::array conflicts{
        Chord{VK_F4, false, false, true},  // Alt+F4 belongs to Windows.
        Chord{VK_F4, true, false, false},
        Chord{VK_F4, false, true, false},
        Chord{VK_DELETE, true, false, true},
        Chord{VK_DELETE, true, true, true},
        Chord{VK_DELETE, true, false, false},
        Chord{VK_DELETE, false, false, true},
        Chord{VK_DELETE, false, true, true},
        Chord{'C', true, true, false},
        Chord{'X', true, true, false},
        Chord{'V', true, true, false},
        Chord{'A', true, true, false},
        Chord{'N', true, false, true},
        Chord{'N', true, true, true},
        Chord{'L', true, true, false},
        Chord{'D', false, true, true},
        Chord{'P', true, false, true},
        Chord{'P', true, true, true},
        Chord{VK_RETURN, false, true, true},
        Chord{VK_F5, true, false, false},
        Chord{VK_F3, false, true, false},
        Chord{VK_BACK, true, false, false},
        Chord{VK_F2, false, true, false}
    };
    for (const auto& chord : conflicts) {
        expect(chord, false, std::nullopt, "Extra modifiers must not change system/text chords into file commands");
        expect(chord, true, std::nullopt, "Extra modifiers must stay native while editing");
    }

    // AltGr is reported as Ctrl+Alt on common keyboard layouts. It must never
    // activate host commands while the user enters a character.
    constexpr std::array altGrKeys{'A', 'C', 'D', 'E', 'F', 'L', 'N', 'P', 'V', 'W', 'X'};
    for (const UINT key : altGrKeys) {
        for (const bool shift : {false, true}) {
            expect({key, true, shift, true}, false, std::nullopt, "AltGr must not dispatch host commands");
            expect({key, true, shift, true}, true, std::nullopt, "AltGr text input must remain native");
        }
    }
}

void viewLayoutChords() {
    constexpr std::array expected{
        explorer::ViewFirst,
        static_cast<Command>(explorer::ViewFirst + 1),
        static_cast<Command>(explorer::ViewFirst + 2),
        static_cast<Command>(explorer::ViewFirst + 3),
        static_cast<Command>(explorer::ViewFirst + 4),
        static_cast<Command>(explorer::ViewFirst + 5),
        static_cast<Command>(explorer::ViewFirst + 6),
        explorer::ViewLast
    };
    for (unsigned index = 0; index < expected.size(); ++index) {
        const UINT key = '1' + index;
        expect({key, true, true, false}, false, expected[index], "View-layout chord must preserve numeric order");
        expect({key, true, true, false}, true, std::nullopt, "View-layout chord must not interfere with text input");
        expect({key, true, true, true}, false, std::nullopt, "AltGr number must not change the view");
        expect({key, true, false, false}, false, std::nullopt, "Ctrl alone must not change the view");
    }
    constexpr std::array<UINT, 4> otherDigits{'0', '9', VK_NUMPAD1, VK_NUMPAD8};
    for (const UINT key : otherDigits)
        expect({key, true, true, false}, false, std::nullopt, "Only the documented top-row digits select layouts");
}

void nativeKeysRemainNative() {
    // Enter/Tab/Escape and undo/redo remain available to native controls;
    // unsupported host Undo/Redo helpers must not intercept native shortcuts.
    constexpr std::array<UINT, 7> keys{VK_ESCAPE, VK_TAB, VK_SPACE, 'Z', 'Y', 'Q', VK_F12};
    for (const UINT key : keys) {
        for (unsigned modifiers = 0; modifiers < 8; ++modifiers) {
            const Chord chord{key, (modifiers & 1) != 0, (modifiers & 2) != 0, (modifiers & 4) != 0};
            expect(chord, false, std::nullopt, "Unhandled key must remain available to the Shell or Windows");
            expect(chord, true, std::nullopt, "Unhandled edit key must remain available to its control");
        }
    }
    expect({VK_RETURN, false, false, false}, false, std::nullopt, "The native view owns Enter to open its selection");
    expect({VK_RETURN, false, false, false}, true, std::nullopt, "The edit control owns Enter to commit its text");
    expect({0, false, false, false}, false, std::nullopt, "A missing key must not dispatch a command");
    expect({0xffffffffU, true, true, true}, true, std::nullopt, "An invalid key must not dispatch a command");
}

void focusAndFullscreenModifiers() {
    for (unsigned modifiers = 0; modifiers < 8; ++modifiers) {
        const Chord nextChord{VK_F6, (modifiers & 1) != 0,
                              (modifiers & 2) != 0, (modifiers & 4) != 0};
        const std::optional<Command> focus = modifiers == 0 ? explorer::FocusNext :
            modifiers == 2 ? std::optional<Command>(explorer::FocusPrevious) : std::nullopt;
        const Chord fullscreenChord{VK_F11, nextChord.control, nextChord.shift, nextChord.alt};
        const std::optional<Command> fullscreen = modifiers == 0 ?
            std::optional<Command>(explorer::Fullscreen) : std::nullopt;
        for (const bool editing : {false, true}) {
            expect(nextChord, editing, focus, "F6 must cycle focus only with its exact supported modifiers");
            expect(fullscreenChord, editing, fullscreen, "F11 must toggle fullscreen only without modifiers");
        }
    }
}

void expectFocus(std::optional<explorer::FocusRegion> current, bool backwards,
                 const explorer::FocusAvailability& available,
                 std::optional<explorer::FocusRegion> expected, const char* description) {
    ++assertions;
    if (explorer::cycleFocusRegion(current, backwards, available) != expected)
        throw std::runtime_error(description);
}

void focusCycleOrderAndAvailability() {
    using explorer::FocusRegion;
    const explorer::FocusAvailability all;
    constexpr std::array forward{
        FocusRegion::FolderView, FocusRegion::Sorting, FocusRegion::Status,
        FocusRegion::Toolbar, FocusRegion::Navigation, FocusRegion::FolderView};
    std::optional<FocusRegion> current = FocusRegion::Navigation;
    for (const auto expected : forward) {
        expectFocus(current, false, all, expected, "Windows 10 forward focus order must wrap around");
        current = expected;
    }
    constexpr std::array backward{
        FocusRegion::Navigation, FocusRegion::Toolbar, FocusRegion::Status,
        FocusRegion::Sorting, FocusRegion::FolderView, FocusRegion::Navigation};
    current = FocusRegion::FolderView;
    for (const auto expected : backward) {
        expectFocus(current, true, all, expected, "Windows 10 backward focus order must reverse the same regions");
        current = expected;
    }
    expectFocus(std::nullopt, false, all, FocusRegion::FolderView, "Unknown forward focus starts at content");
    expectFocus(std::nullopt, true, all, FocusRegion::Navigation, "Unknown backward focus starts at navigation");
    expectFocus(static_cast<FocusRegion>(-1), false, all, FocusRegion::FolderView, "Invalid negative focus is treated as unknown");
    expectFocus(static_cast<FocusRegion>(999), true, all, FocusRegion::Navigation, "Invalid focus is treated as unknown");

    const explorer::FocusAvailability iconsWithoutNavigation{true, false, true, true, false};
    expectFocus(FocusRegion::FolderView, false, iconsWithoutNavigation, FocusRegion::Status,
                "Icon views skip the unavailable Details sorting header");
    expectFocus(FocusRegion::Status, true, iconsWithoutNavigation, FocusRegion::FolderView,
                "Backward focus skips the unavailable sorting header");
    expectFocus(FocusRegion::Toolbar, false, iconsWithoutNavigation, FocusRegion::FolderView,
                "A hidden navigation pane is skipped on wraparound");
    expectFocus(FocusRegion::Navigation, true, iconsWithoutNavigation, FocusRegion::Toolbar,
                "A newly hidden current region advances from its original position");

    const explorer::FocusAvailability noNativeFrame{false, false, false, true, false};
    expectFocus(std::nullopt, false, noNativeFrame, FocusRegion::Toolbar,
                "A toolbar remains usable when the native view/frame is unavailable");
    expectFocus(FocusRegion::Toolbar, true, noNativeFrame, FocusRegion::Toolbar,
                "The sole available region is its own backward cycle");

    const explorer::FocusAvailability none{false, false, false, false, false};
    for (const auto region : forward) {
        expectFocus(region, false, none, std::nullopt, "Empty focus set has no forward target");
        expectFocus(region, true, none, std::nullopt, "Empty focus set has no backward target");
    }
    expectFocus(std::nullopt, false, none, std::nullopt, "Unknown focus with an empty set stays empty");
    expectFocus(std::nullopt, true, none, std::nullopt, "Unknown backward focus with an empty set stays empty");
}
void focusCycleSubsetInvariants() {
    using explorer::FocusRegion;
    constexpr std::array regions{
        FocusRegion::FolderView, FocusRegion::Sorting, FocusRegion::Status,
        FocusRegion::Toolbar, FocusRegion::Navigation};
    auto require = [](bool passed, const char* description) {
        ++assertions;
        if (!passed) throw std::runtime_error(description);
    };

    // Every availability subset must form a reversible closed cycle that
    // visits each available region once and never targets a hidden region.
    for (unsigned subset = 1; subset < 32; ++subset) {
        const explorer::FocusAvailability available{
            (subset & 1) != 0, (subset & 2) != 0, (subset & 4) != 0,
            (subset & 8) != 0, (subset & 16) != 0};
        unsigned expectedVisits = 0;
        for (unsigned index = 0; index < regions.size(); ++index)
            if ((subset & (1U << index)) != 0) ++expectedVisits;

        for (unsigned start = 0; start < regions.size(); ++start) {
            if ((subset & (1U << start)) == 0) continue;
            for (const bool backwards : {false, true}) {
                std::optional<FocusRegion> current = regions[start];
                unsigned visited = 0;
                for (unsigned step = 0; step < expectedVisits; ++step) {
                    const auto next = explorer::cycleFocusRegion(current, backwards, available);
                    require(next.has_value(), "A nonempty focus set must always yield a target");
                    const unsigned index = static_cast<unsigned>(*next);
                    require(index < regions.size(), "The focus target must be a defined region");
                    const unsigned bit = 1U << index;
                    require((subset & bit) != 0, "The focus target must be available");
                    require((visited & bit) == 0, "Focus must visit each available region once per cycle");
                    require(explorer::cycleFocusRegion(next, !backwards, available) == current,
                            "Reversing a focus step must return to the preceding available region");
                    visited |= bit;
                    current = next;
                }
                require(visited == subset, "One focus cycle must visit the entire available set");
                require(current == regions[start], "One complete focus cycle must return to its start");
            }
        }
    }
}
} // namespace

int runInputTests() {
    struct Test { const char* name; void (*run)(); };
    constexpr std::array tests{
        Test{"navigation and window chords", navigationAndWindowChords},
        Test{"file commands and native rename safety", fileCommandsAndRenameSafety},
        Test{"modifier conflicts, system keys, and AltGr", modifierConflictsAndSystemKeys},
        Test{"eight view-layout shortcuts", viewLayoutChords},
        Test{"unhandled native editing and Shell keys", nativeKeysRemainNative},
        Test{"F6 and fullscreen modifier precedence", focusAndFullscreenModifiers},
        Test{"focus order, wraparound, and unavailable regions", focusCycleOrderAndAvailability},
        Test{"focus cycle invariants for all availability subsets", focusCycleSubsetInvariants}
    };
    assertions = 0;
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: Keyboard " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: Keyboard " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL: Keyboard " << test.name << ": unknown exception\n";
        }
    }
    std::cout << "Keyboard: " << tests.size() - static_cast<unsigned>(failures)
              << '/' << tests.size() << " headless groups; " << assertions << " assertions\n";
    return failures;
}
