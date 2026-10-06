#pragma once

#include <windows.h>
#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace explorer {

enum class StartupDesktopArm : DWORD { InputThenPrivate = 1, InitialPrivate = 2 };

struct StartupDesktopChildReadback {
    HRESULT packet = E_PENDING, apartment = E_PENDING, parent = E_PENDING;
    HRESULT identity = E_PENDING, startup = E_PENDING, attachment = E_PENDING;
    HRESULT input = E_PENDING, executable = E_PENDING;
    DWORD parentPid = 0, parentTid = 0, childPid = 0, childTid = 0, session = 0;
    int apartmentType = -1, apartmentQualifier = -1;
    bool initialHandleBorrowed = false, inputUnchanged = false;
};

// Claim before theme, COM, common controls or HWND creation. Only a successful
// claim takes ownership of the two designated inherited kernel handles. The
// section is copied once into a bounded snapshot; a read-only handle is not an
// authentication boundary against another process with section write access.
// Keep this context alive through native teardown and PrivateDesktop finish.
class StartupDesktopChild final {
public:
    StartupDesktopChild() noexcept;
    ~StartupDesktopChild();
    StartupDesktopChild(const StartupDesktopChild&) = delete;
    StartupDesktopChild& operator=(const StartupDesktopChild&) = delete;
    HRESULT claim(HANDLE mapping, HANDLE parent, StartupDesktopArm expected,
                  StartupDesktopChildReadback& readback) noexcept;
    HRESULT verifyInitial() const noexcept;
    HRESULT verifyParentAndInput() const noexcept;
    bool ready() const noexcept;
    StartupDesktopArm arm() const noexcept;
    HDESK initialDesktop() const noexcept; // borrowed; never close or switch it
    const std::wstring& desktopName() const noexcept;
    const std::wstring& inputName() const noexcept;
    const std::wstring& reportPath() const noexcept;
    bool installedRibbon() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct StartupDesktopArmReadback {
    StartupDesktopArm arm = StartupDesktopArm::InputThenPrivate;
    HRESULT create = E_PENDING, packet = E_PENDING, resume = E_PENDING;
    HRESULT wait = E_PENDING, exitRead = E_PENDING, preservation = E_PENDING;
    HRESULT termination = E_PENDING;
    DWORD childPid = 0, childTid = 0, exitCode = STILL_ACTIVE;
    ULONGLONG resumeTick = 0, kernelExitTick = 0;
    bool kernelExited = false, timedOut = false;
    std::filesystem::path report, stdoutLog, stderrLog;
};

struct StartupDesktopComparisonReadback {
    HRESULT guard = E_PENDING, directory = E_PENDING, desktop = E_PENDING;
    HRESULT executable = E_PENDING, finish = E_PENDING;
    bool noninheritableDesktop = false, inputUnchanged = false;
    bool sameExecutable = false;
    std::array<StartupDesktopArmReadback, 2> arms{};
};

// Explicit diagnostic launcher, before COM/HWND initialization. outputDirectory
// must be an absolute new directory (CREATE_NEW semantics); it retains the two
// child reports. No shell parses the constructed command line. Each exact owned
// child has a 180-second bound; normal CTest and per-native-call budgets do not
// change. Only the packet, minimal parent identity, two owned log files and a
// read-only NUL input handle are inherited, never an HDESK. The one created
// GUID desktop stays owned until both child processes actually exit.
// S_OK reports transport/lifetime/preservation, not Smoke or Preview success:
// the unchanged child's strict result is retained in arms[].exitCode/report.
HRESULT runStartupDesktopComparison(const std::filesystem::path& outputDirectory,
    bool installedRibbon, StartupDesktopComparisonReadback& readback) noexcept;

} // namespace explorer
