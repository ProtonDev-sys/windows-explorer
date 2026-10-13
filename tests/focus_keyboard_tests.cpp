#include "explorer/focus_keyboard.hpp"
#include "explorer/headless_visual.hpp"

#include <exception>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using explorer::FocusKeyboardReadback;
using explorer::FocusKeyboardState;
using explorer::FocusShiftTransitions;
constexpr BYTE Generic = 1, Left = 2, Right = 4;
constexpr UINT ShiftKeys[]{VK_SHIFT, VK_LSHIFT, VK_RSHIFT};
unsigned assertions = 0;

void require(bool condition, const char* description) {
    ++assertions;
    if (!condition) throw std::runtime_error(description);
}
void succeeded(HRESULT status, const char* description) {
    require(SUCCEEDED(status), description);
}
std::string tableDifferences(const FocusKeyboardState& expected, const FocusKeyboardState& actual) {
    std::ostringstream text;
    unsigned differences = 0;
    for (unsigned key = 0; key < expected.size(); ++key) if (expected[key] != actual[key]) {
        text << " key" << key << "=" << static_cast<unsigned>(expected[key]) << "/" << static_cast<unsigned>(actual[key]);
        ++differences;
    }
    return "changedBytes=" + std::to_string(differences) + text.str();
}
std::string stages(const FocusKeyboardReadback& report) {
    std::ostringstream text;
    text << std::hex << "original/hookInstall/neutralWrite/neutralRead/action/hookRemove/freshRead/merge/restoreWrite/restoreRead=0x"
        << static_cast<ULONG>(report.originalRead) << "/0x" << static_cast<ULONG>(report.hookInstall)
        << "/0x" << static_cast<ULONG>(report.neutralWrite) << "/0x" << static_cast<ULONG>(report.neutralRead)
        << "/0x" << static_cast<ULONG>(report.action) << "/0x" << static_cast<ULONG>(report.hookRemove)
        << "/0x" << static_cast<ULONG>(report.freshRead) << "/0x" << static_cast<ULONG>(report.merge)
        << "/0x" << static_cast<ULONG>(report.restoreWrite) << "/0x" << static_cast<ULONG>(report.restoreRead)
        << std::dec << "; removed/nonremoved=" << report.removedShiftEvents << "/" << report.nonremovedShiftNotifications
        << "; neutralized/ambiguous/detached/nonShiftPreserved/exact=" << report.neutralized << "/" << report.ambiguous
        << "/" << report.hookDetached << "/" << report.freshNonShiftPreserved << "/" << report.exactRestoredTable;
    return text.str();
}
FocusKeyboardState readState() {
    FocusKeyboardState state{};
    require(GetKeyboardState(state.data()) != FALSE, "Read exact private-thread keyboard table");
    return state;
}
void writeState(const FocusKeyboardState& state) {
    auto copy = state;
    SetLastError(ERROR_SUCCESS);
    const auto written = SetKeyboardState(copy.data());
    const auto error = GetLastError();
    if (!written) std::cerr << "focus-keyboard stage=private-state-write BOOL=" << written << "; error=" << error << '\n';
    require(written != FALSE, "Write private-thread logical keyboard table");
    const auto actual = readState();
    if (actual != state) std::cerr << "focus-keyboard stage=private-state-write-native-readback " << tableDifferences(state, actual) << '\n';
    require(actual == state, "Private-thread logical table exact native readback");
}
BYTE coherentMask(BYTE sides) {
    return static_cast<BYTE>(sides | (sides ? Generic : 0));
}
void setMask(FocusKeyboardState& state, BYTE mask) {
    for (unsigned index = 0; index < 3; ++index)
        state[ShiftKeys[index]] = static_cast<BYTE>((state[ShiftKeys[index]] & 0x7f) | ((mask & (1 << index)) ? 0x80 : 0));
}
FocusKeyboardState patterned(unsigned salt) {
    FocusKeyboardState state{};
    for (unsigned index = 0; index < state.size(); ++index) state[index] = static_cast<BYTE>((index * 37 + salt * 19) & 0xff);
    return state;
}

void reducerSequences() {
    for (unsigned initial = 0; initial < 4; ++initial) {
        auto original = patterned(initial);
        const BYTE originalSides = static_cast<BYTE>(((initial & 1) ? Left : 0) | ((initial & 2) ? Right : 0));
        setMask(original, coherentMask(originalSides));
        for (unsigned length = 0; length <= 5; ++length) {
            const auto combinations = 1u << (2 * length);
            for (unsigned sequence = 0; sequence < combinations; ++sequence) {
                FocusShiftTransitions transitions;
                BYTE logicalSides = originalSides, neutralSides = 0;
                for (unsigned index = 0; index < length; ++index) {
                    const auto event = (sequence >> (2 * index)) & 3;
                    const UINT side = (event & 2) ? VK_RSHIFT : VK_LSHIFT;
                    const BYTE bit = side == VK_LSHIFT ? Left : Right;
                    const bool down = (event & 1) != 0;
                    transitions.observe(side, down);
                    if (down) { logicalSides |= bit; neutralSides |= bit; }
                    else { logicalSides &= static_cast<BYTE>(~bit); neutralSides &= static_cast<BYTE>(~bit); }
                }
                auto fresh = patterned(sequence + length + 7);
                setMask(fresh, coherentMask(neutralSides));
                auto expected = fresh;
                setMask(expected, coherentMask(logicalSides));
                FocusKeyboardState actual{};
                require(explorer::mergeFocusKeyboardState(original, fresh, transitions, &actual) == S_OK,
                        "Coherent side transitions must merge");
                require(actual == expected, "Exact table must preserve fresh bytes and replay only valid Shift transitions");
                require(transitions.count == length && !transitions.ambiguous, "Bounded transition reducer count");
            }
        }
    }
    // SetKeyboardState accepts synthetic generic-only tables. Without native
    // input there is no reason to infer a left/right side or change that table.
    for (BYTE mask = 0; mask < 8; ++mask) {
        auto original = patterned(mask), fresh = patterned(mask + 13), expected = fresh;
        setMask(original, mask); setMask(fresh, 0); setMask(expected, mask);
        FocusKeyboardState actual{};
        require(explorer::mergeFocusKeyboardState(original, fresh, {}, &actual) == S_OK && actual == expected,
                "Stable synthetic masks restore exact Shift highbits without guessed sides");
    }
}

void reducerAmbiguities() {
    auto original = patterned(1), fresh = patterned(2);
    setMask(original, Generic | Left); setMask(fresh, 0);
    const auto rejectsFresh = [&](const FocusKeyboardState& prior, const FocusKeyboardState& current,
                                  const FocusShiftTransitions& transitions) {
        FocusKeyboardState restored{};
        require(explorer::mergeFocusKeyboardState(prior, current, transitions, &restored) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                "Ambiguous evidence must fail explicitly");
        require(restored == current, "Ambiguous evidence must preserve every fresh byte");
    };
    FocusShiftTransitions invalid; invalid.observe(VK_SHIFT, false);
    rejectsFresh(original, fresh, invalid);
    FocusShiftTransitions release; release.observe(VK_LSHIFT, false);
    auto inconsistent = original; setMask(inconsistent, Generic);
    rejectsFresh(inconsistent, fresh, release);
    auto unexpected = fresh; setMask(unexpected, Generic | Right);
    rejectsFresh(original, unexpected, {});
    rejectsFresh(original, unexpected, release);
    auto suppressed = release; suppressed.ambiguous = true;
    rejectsFresh(original, fresh, suppressed);
    FocusShiftTransitions overflow; overflow.count = std::numeric_limits<unsigned>::max(); overflow.observe(VK_LSHIFT, true);
    rejectsFresh(original, fresh, overflow);
    require(explorer::mergeFocusKeyboardState(original, fresh, {}, nullptr) == E_POINTER,
            "Null table output rejected");
    auto aliased = original;
    auto expected = fresh; setMask(expected, Generic | Left);
    require(explorer::mergeFocusKeyboardState(aliased, fresh, {}, &aliased) == S_OK && aliased == expected,
            "In-place original table output preserves initial Shift evidence");
    aliased = fresh;
    require(explorer::mergeFocusKeyboardState(original, aliased, {}, &aliased) == S_OK && aliased == expected,
            "In-place fresh table output preserves all fresh bytes");
}

LPARAM keyBits(UINT side, bool released) {
    const auto scan = MapVirtualKeyExW(side, MAPVK_VK_TO_VSC_EX, GetKeyboardLayout(0));
    require(scan && (scan & 0xff00) == 0, "Native Shift scan mapping without guessed side or extended code");
    return static_cast<LPARAM>(1u | ((scan & 0xff) << 16) | (released ? 0xc0000000u : 0u));
}
void scanParsing() {
    const auto layout = GetKeyboardLayout(0);
    require(layout != nullptr, "Current native keyboard layout");
    for (const auto side : {VK_LSHIFT, VK_RSHIFT}) for (const bool released : {false, true}) {
        const auto bits = keyBits(side, released);
        require(explorer::focusShiftMessageSide(VK_SHIFT, bits, layout) == static_cast<UINT>(side), "Generic Shift scan decodes exact side");
        require(explorer::focusShiftMessageSide(side, bits, layout) == static_cast<UINT>(side), "Matching explicit Shift scan accepted");
        require(explorer::focusShiftMessageSide(side == VK_LSHIFT ? VK_RSHIFT : VK_LSHIFT, bits, layout) == 0,
                "Contradictory Shift key and scan rejected");
        require(explorer::focusShiftMessageSide('A', bits, layout) == 0, "Non-Shift key cannot become a Shift transition");
        require(explorer::focusShiftMessageSide(VK_SHIFT, bits, nullptr) == 0, "Missing layout rejected");
        // Exercise the exact production callback parser, including bit31,
        // auto-repeat bit30 and Alt context bit29. No native input is injected.
        for (const WPARAM key : {WPARAM{VK_SHIFT}, static_cast<WPARAM>(side)}) {
            for (unsigned flags = 0; flags < 4; ++flags) {
                FocusShiftTransitions transitions;
                const auto withFlags = static_cast<LPARAM>((static_cast<ULONG_PTR>(bits) & ~(ULONG_PTR{1} << 30)) |
                    ((flags & 1) ? (ULONG_PTR{1} << 30) : 0) | ((flags & 2) ? (ULONG_PTR{1} << 29) : 0));
                transitions.observeMessage(key, withFlags, layout);
                const BYTE sideBit = side == VK_LSHIFT ? Left : Right;
                require(transitions.count == 1 && !transitions.ambiguous && transitions.sidesSeen == sideBit &&
                        transitions.sidesDown == (released ? 0 : sideBit),
                        "Exact native callback method decodes press/release independently of repeat/Alt flags");
                auto original = patterned(flags), fresh = patterned(flags + 3), expected = fresh;
                setMask(original, Generic | Left | Right);
                setMask(fresh, released ? 0 : coherentMask(sideBit));
                expected = fresh;
                setMask(expected, released ? coherentMask(static_cast<BYTE>((Left | Right) & ~sideBit)) :
                    static_cast<BYTE>(Generic | Left | Right));
                FocusKeyboardState restored{};
                require(explorer::mergeFocusKeyboardState(original, fresh, transitions, &restored) == S_OK && restored == expected,
                        "Production-decoded event uses the exact reducer and preserves the other held Shift side");
            }
        }
    }
    require(explorer::focusShiftMessageSide(VK_SHIFT, 1, layout) == 0, "Missing scan rejected");
    const auto otherScan = MapVirtualKeyExW('A', MAPVK_VK_TO_VSC_EX, layout);
    require(otherScan != 0, "Native non-Shift scan");
    require(explorer::focusShiftMessageSide(VK_SHIFT, static_cast<LPARAM>(1u | ((otherScan & 0xff) << 16)), layout) == 0,
            "A non-Shift scan cannot be guessed into a side");
    for (const auto invalid : {LPARAM{1}, static_cast<LPARAM>(1u | ((otherScan & 0xff) << 16))}) {
        FocusShiftTransitions transitions;
        transitions.observeMessage(VK_SHIFT, invalid, layout);
        require(transitions.count == 1 && transitions.ambiguous && !transitions.sidesSeen && !transitions.sidesDown,
                "Production callback parser rejects missing/non-Shift scans without inventing a side");
    }
    FocusShiftTransitions sequence;
    sequence.observeMessage(VK_SHIFT, keyBits(VK_LSHIFT, false), layout);
    sequence.observeMessage(VK_SHIFT, keyBits(VK_RSHIFT, false), layout);
    sequence.observeMessage(VK_SHIFT, keyBits(VK_LSHIFT, true), layout);
    require(sequence.count == 3 && !sequence.ambiguous && sequence.sidesSeen == (Left | Right) && sequence.sidesDown == Right,
            "Production parser retains ordered opposite-side press and release sequence");
}

struct RestoreKeyboard {
    FocusKeyboardState original = readState();
    bool restored = false;
    void restore() { writeState(original); restored = true; }
    ~RestoreKeyboard() { if (!restored) SetKeyboardState(original.data()); }
};
struct Action {
    FocusKeyboardState expectedNeutral{}, fresh{};
    unsigned calls = 0;
    HRESULT result = S_OK;
    bool throwException = false, changeState = false, nested = false;
    static HRESULT invoke(void* raw) {
        auto& self = *static_cast<Action*>(raw);
        ++self.calls;
        const auto admitted = readState();
        if (admitted != self.expectedNeutral) std::cerr << "focus-keyboard stage=action-admission " << tableDifferences(self.expectedNeutral, admitted) << '\n';
        require(admitted == self.expectedNeutral, "Action sees exact expected neutral table");
        if (self.nested) {
            Action inner; inner.expectedNeutral = self.expectedNeutral;
            FocusKeyboardReadback report;
            const auto nestedResult = explorer::focusWithNeutralShift(invoke, &inner, &report);
            if (nestedResult != HRESULT_FROM_WIN32(ERROR_BUSY) || inner.calls)
                std::cerr << "focus-keyboard stage=nested-admission result=" << static_cast<ULONG>(nestedResult) << "; calls=" << inner.calls << "; " << stages(report) << '\n';
            require(nestedResult == HRESULT_FROM_WIN32(ERROR_BUSY) && inner.calls == 0,
                    "Nested neutralizer cannot replace live hook/TLS context");
        }
        if (self.changeState) writeState(self.fresh);
        if (self.throwException) throw std::runtime_error("Owned simulated provider exception");
        return self.result;
    }
};

void stableNativeAndProviderUpdates() {
    RestoreKeyboard restoration;
    unsigned stableCases = 0;
    for (const BYTE mask : {BYTE{0}, Generic, BYTE{Generic | Left}, BYTE{Generic | Right}, BYTE{Generic | Left | Right}}) {
        for (unsigned lowBits = 0; lowBits < 8; ++lowBits) {
            auto original = restoration.original;
            for (unsigned index = 0; index < 3; ++index)
                original[ShiftKeys[index]] = static_cast<BYTE>((original[ShiftKeys[index]] & 0xfe) | ((lowBits >> index) & 1));
            setMask(original, mask); writeState(original);
            const std::array<USHORT, 3> nativeKeys{
                static_cast<USHORT>(GetKeyState(VK_SHIFT)), static_cast<USHORT>(GetKeyState(VK_LSHIFT)),
                static_cast<USHORT>(GetKeyState(VK_RSHIFT))};
            Action action; action.expectedNeutral = original; setMask(action.expectedNeutral, 0);
            FocusKeyboardReadback report;
            const auto result = explorer::focusWithNeutralShift(Action::invoke, &action, &report);
            const auto actual = readState();
            if (result != S_OK || action.calls != 1 || actual != original)
                std::cerr << "focus-keyboard stage=stable-matrix heldMask=" << static_cast<unsigned>(mask) << "; lowBits=" << lowBits
                    << "; originalGetKeyState(generic/left/right)=" << nativeKeys[0] << "/" << nativeKeys[1] << "/" << nativeKeys[2]
                    << "; result=" << static_cast<ULONG>(result) << "; calls=" << action.calls << "; " << stages(report)
                    << "; " << tableDifferences(original, actual) << '\n';
            require(result == S_OK && action.calls == 1, "Stable native action runs once");
            require(actual == original, "Every Shift lowbit/held-mask combination restores exact original256");
            if (mask) require(report.hookInstall == S_OK && report.hookRemove == S_OK && report.hookDetached &&
                report.exactRestoredTable && report.freshNonShiftPreserved && report.removedShiftEvents == 0,
                "Owned native hook installation/removal and exact stable restoration evidence");
            else require(!report.neutralized && report.hookInstall == E_PENDING,
                         "Shift-up route does not install a hook or rewrite the table");
            ++stableCases;
        }
    }
    require(stableCases == 40, "All eight Shift lowbit combinations and five held masks exercised");
    auto original = restoration.original; setMask(original, Generic | Left); writeState(original);
    Action action; action.expectedNeutral = original; setMask(action.expectedNeutral, 0);
    action.fresh = action.expectedNeutral;
    // v72's native fixture observed SetKeyboardState TRUE but immediate
    // GetKeyboardState A/B highbits requested128/read0. The strict writer
    // rejected that unsupported fixture before its intended action result;
    // all helper cleanup stages/table-preservation checks succeeded. Use
    // representable modifier/toggle updates here, retaining exact roundtrip
    // assertions. The pure reducer still checks arbitrary non-Shift bytes.
    action.fresh[VK_MENU] ^= 0x80; action.fresh[VK_CAPITAL] ^= 1; action.fresh[VK_CONTROL] ^= 0x80;
    action.fresh[VK_SHIFT] ^= 1; action.changeState = true; action.nested = true;
    auto expected = action.fresh; setMask(expected, Generic | Left);
    FocusKeyboardReadback report;
    const auto result = explorer::focusWithNeutralShift(Action::invoke, &action, &report);
    const auto actual = readState();
    if (result != S_OK || actual != expected) std::cerr << "focus-keyboard stage=reentrant-provider result=" << static_cast<ULONG>(result)
        << "; actionCalls=" << action.calls << "; " << stages(report) << "; expected/actual " << tableDifferences(expected, actual)
        << "; ownedFresh/actual " << tableDifferences(action.fresh, actual) << '\n';
    require(result == S_OK && actual == expected,
            "Reentrant provider non-Shift changes and fresh Shift lowbits survive restoration");
    require(report.freshNonShiftPreserved && report.exactRestoredTable && report.hookDetached,
            "Native fresh-table merge readback");
    restoration.restore();
}

void actionFailureAndUnexpectedShift() {
    RestoreKeyboard restoration;
    for (const bool exception : {false, true}) {
        auto original = restoration.original; setMask(original, Generic); writeState(original);
        Action action; action.expectedNeutral = original; setMask(action.expectedNeutral, 0);
        action.result = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); action.throwException = exception;
        action.changeState = true; action.fresh = action.expectedNeutral; action.fresh[VK_CONTROL] ^= 0x80;
        auto expected = action.fresh; setMask(expected, Generic);
        FocusKeyboardReadback report;
        const auto result = explorer::focusWithNeutralShift(Action::invoke, &action, &report);
        const auto actual = readState();
        const auto expectedResult = exception ? E_FAIL : action.result;
        if (result != expectedResult || actual != expected) std::cerr << "focus-keyboard stage=failed-action exception=" << exception
            << "; result/expected=" << static_cast<ULONG>(result) << "/" << static_cast<ULONG>(expectedResult) << "; calls=" << action.calls
            << "; " << stages(report) << "; expected/actual " << tableDifferences(expected, actual)
            << "; ownedFresh/actual " << tableDifferences(action.fresh, actual) << '\n';
        require(result == expectedResult,
                "Native failure/exception propagated after cleanup");
        require(actual == expected && report.hookDetached && report.exactRestoredTable,
                "Failed/throwing action still unhooks and merges exact fresh keys");
    }
    auto original = restoration.original; setMask(original, Generic | Left); writeState(original);
    Action action; action.expectedNeutral = original; setMask(action.expectedNeutral, 0);
    action.fresh = action.expectedNeutral; setMask(action.fresh, Generic | Right); action.changeState = true;
    action.fresh[VK_CONTROL] ^= 0x80;
    FocusKeyboardReadback report;
    require(explorer::focusWithNeutralShift(Action::invoke, &action, &report) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "Unobserved provider Shift change cannot be replayed as native input");
    require(readState() == action.fresh && report.ambiguous && report.hookDetached,
            "Unexpected Shift change preserves entire fresh table and fails");
    require(explorer::focusWithNeutralShift(nullptr, nullptr) == E_POINTER, "Null focus callback rejected");
    restoration.restore();
}

struct PostedReadback { unsigned removed = 0, nonremoved = 0; };
thread_local PostedReadback* postedReadback = nullptr;
LRESULT CALLBACK postedHook(int code, WPARAM removal, LPARAM raw) noexcept {
    if (code == HC_ACTION && postedReadback && raw) {
        const auto& message = *reinterpret_cast<const MSG*>(raw);
        if ((message.message == WM_KEYDOWN || message.message == WM_KEYUP) && message.wParam == VK_SHIFT) {
            if (removal == PM_REMOVE) ++postedReadback->removed;
            else if (removal == PM_NOREMOVE) ++postedReadback->nonremoved;
        }
    }
    return CallNextHookEx(nullptr, code, removal, raw);
}
struct PostedHook {
    HHOOK hook = nullptr;
    explicit PostedHook(PostedReadback& report) {
        postedReadback = &report;
        hook = SetWindowsHookExW(WH_GETMESSAGE, postedHook, nullptr, GetCurrentThreadId());
        if (!hook) postedReadback = nullptr;
        require(hook != nullptr, "Install separate owned GETMESSAGE observation hook");
    }
    void finish() {
        postedReadback = nullptr;
        const auto removed = UnhookWindowsHookEx(hook); hook = nullptr;
        require(removed != FALSE, "Remove owned GETMESSAGE observation hook");
    }
    ~PostedHook() { postedReadback = nullptr; if (hook) UnhookWindowsHookEx(hook); }
};
struct PostedAction {
    FocusKeyboardState neutral{};
    static HRESULT invoke(void* raw) {
        const auto& self = *static_cast<PostedAction*>(raw);
        for (const auto side : {VK_LSHIFT, VK_RSHIFT}) for (const bool released : {false, true}) {
            const UINT type = released ? WM_KEYUP : WM_KEYDOWN;
            const auto bits = keyBits(side, released);
            require(PostThreadMessageW(GetCurrentThreadId(), type, VK_SHIFT, bits) != FALSE, "Post owned thread Shift fixture message");
            MSG peek{}, removed{};
            require(PeekMessageW(&peek, reinterpret_cast<HWND>(-1), type, type, PM_NOREMOVE) &&
                    peek.hwnd == nullptr && peek.message == type && peek.wParam == VK_SHIFT && peek.lParam == bits,
                    "Exact owned posted Shift message observed without removal");
            require(readState() == self.neutral, "Posted key peek cannot change logical keyboard state");
            require(PeekMessageW(&removed, reinterpret_cast<HWND>(-1), type, type, PM_REMOVE) &&
                    removed.hwnd == nullptr && removed.message == type && removed.wParam == VK_SHIFT && removed.lParam == bits,
                    "Exact owned posted Shift message removed once");
            require(readState() == self.neutral, "Posted key removal cannot change logical keyboard state");
        }
        return S_OK;
    }
};
void postedMessagesCannotInventInput() {
    RestoreKeyboard restoration;
    auto original = restoration.original; setMask(original, Generic | Left | Right); writeState(original);
    PostedReadback posted; PostedHook observer(posted);
    PostedAction action; action.neutral = original; setMask(action.neutral, 0);
    FocusKeyboardReadback report;
    require(explorer::focusWithNeutralShift(PostedAction::invoke, &action, &report) == S_OK,
            "Posted Shift fixture must leave native input observer empty");
    observer.finish();
    require(posted.removed == 4 && posted.nonremoved == 4, "Separate native GETMESSAGE hook proves exact PM_REMOVE/PM_NOREMOVE fixture delivery");
    require(report.removedShiftEvents == 0 && report.nonremovedShiftNotifications == 0,
            "Posted messages cannot masquerade as WH_KEYBOARD input-channel events");
    require(report.hookDetached && readState() == original, "Posted fixture restores both-held Shift table exactly");
    restoration.restore();
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::promise<int> promise;
    auto completed = promise.get_future();
    std::thread worker([output = std::move(promise)]() mutable {
        explorer::PrivateDesktop desktop;
        int failures = 0;
        try {
            succeeded(desktop.initialize(), "Attach fresh owned non-input desktop before queue creation");
            succeeded(desktop.verifyIsolation(), "Verify private-thread test desktop isolation");
            MSG queue{}; PeekMessageW(&queue, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            const auto test = [&](const char* name, auto body) {
                try { body(); std::cout << "PASS: focus keyboard " << name << '\n'; }
                catch (const std::exception& error) { ++failures; std::cerr << "FAIL: focus keyboard " << name << ": " << error.what() << '\n'; }
                catch (...) { ++failures; std::cerr << "FAIL: focus keyboard " << name << ": unknown exception\n"; }
            };
            test("coherent exact transition reducer sequences", reducerSequences);
            test("ambiguous transitions preserve the entire fresh table", reducerAmbiguities);
            test("documented scan and callback parameter decoding", scanParsing);
            test("native stable masks, nested denial and fresh non-Shift provider updates", stableNativeAndProviderUpdates);
            test("failed actions, exceptions and unobserved Shift mutation", actionFailureAndUnexpectedShift);
            test("owned posted Shift messages never invent native input", postedMessagesCannotInventInput);
            succeeded(desktop.verifyIsolation(), "Private focus-keyboard fixture desktop remains isolated");
        } catch (const std::exception& error) { ++failures; std::cerr << "FAIL: focus keyboard fixture: " << error.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: focus keyboard fixture unknown exception\n"; }
        output.set_value(failures);
    });
    // No input injection or visible HWND exists. Wait for the actual kernel
    // thread exit so stack/TLS/hook/desktop lifetimes cannot outlive this test.
    if (WaitForSingleObject(static_cast<HANDLE>(worker.native_handle()), 10000) != WAIT_OBJECT_0) {
        std::cerr << "FAIL: focus keyboard private-thread deadline10000ms\n";
        if (!TerminateProcess(GetCurrentProcess(), 9)) std::_Exit(9);
        std::_Exit(9);
    }
    worker.join();
    const auto failures = completed.get();
    std::cout << "Focus keyboard assertions=" << assertions << "; actual input-positive hardware transition is not exercised\n";
    return failures ? 1 : 0;
}
