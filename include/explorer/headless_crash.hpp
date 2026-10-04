#pragma once

#include <windows.h>
#include <filesystem>

namespace explorer {

// Opt-in, process-local diagnostics for an explicitly headless invocation.
// Installs before COM initialization. On an unhandled exception it creates
// only this exact file, exclusively, and preserves the original fault context.
// Successful runs do not create a dump. No Windows crash-reporting policy is
// changed, and the process-lifetime handler never loads a DLL during a fault.
HRESULT initializeHeadlessCrashDump(bool headless,
    const std::filesystem::path& filename) noexcept;

} // namespace explorer
