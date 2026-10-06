#include "explorer/app.hpp"
#include "explorer/commands.hpp"
#include "explorer/worker_sta.hpp"
#include <docobj.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <tuple>

namespace explorer {
namespace {
void requireBenchmark(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct BenchmarkFixture {
    std::filesystem::path root;
    BenchmarkFixture() {
        GUID guid{};
        requireBenchmark(SUCCEEDED(CoCreateGuid(&guid)), "Create benchmark identity");
        wchar_t name[40]{};
        requireBenchmark(StringFromGUID2(guid, name, ARRAYSIZE(name)) != 0, "Format benchmark identity");
        root = std::filesystem::temp_directory_path() / (std::wstring(L"WindowsExplorer-Benchmark-") + name);
        requireBenchmark(std::filesystem::create_directory(root), "Create owned benchmark directory");
    }
    ~BenchmarkFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path folder(unsigned count) {
        auto path = root / (L"Items-" + std::to_wstring(count));
        requireBenchmark(std::filesystem::create_directory(path), "Create benchmark folder");
        for (unsigned index = 0; index < count; ++index) {
            wchar_t name[32]{};
            swprintf_s(name, L"File-%05u.txt", index);
            std::ofstream output(path / name, std::ios::binary);
            output << "Owned benchmark fixture.\n";
            requireBenchmark(output.good(), "Write benchmark fixture");
        }
        return path;
    }
};
double nowMs() {
    LARGE_INTEGER frequency{}, counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}
struct NavigationSample { double navigation = 0, nativeCallback = 0, firstItem = 0, populated = 0; };
struct Case { unsigned count; std::vector<NavigationSample> samples; };
struct SelectionSample {
    const char* action;
    double duration, command, deferredWork, countReadback, commandStateReady;
    int selected;
    HeadlessCommandTimings hostWork;
    unsigned long long generationChanges, equivalentSelectionRefreshes, uncertainSelectionRefreshes;
    int immediateSelected;
    double immediateCountReadAt, immediateCountReadCost;
};
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(fraction * values.size())) - 1];
}
void summary(std::ostream& output, const std::vector<NavigationSample>& samples,
             double NavigationSample::*member) {
    std::vector<double> values;
    double total = 0;
    for (const auto& sample : samples) { values.push_back(sample.*member); total += sample.*member; }
    output << "{\"meanMs\":" << total / values.size() << ",\"medianMs\":" << percentile(values, .5)
           << ",\"p95Ms\":" << percentile(values, .95) << ",\"maxMs\":" << *std::max_element(values.begin(), values.end()) << '}';
}
}
int ExplorerApp::headlessBenchmark(const std::filesystem::path& report, const HeadlessStartupTimings& startup) {
    if (!headless_) return 2;
    try {
        const auto desktop = PrivateDesktop::current();
        requireBenchmark(desktop && desktop->ready() && SUCCEEDED(desktop->verifyIsolation()),
            "Benchmark requires an isolated private desktop");
        bool inputDesktopUnchanged = false, visibleInputWindows = false;
        unsigned desktopObservations = 0;
        double desktopObservationMs = 0, maximumDesktopObservationMs = 0;
        auto observeDesktop = [&] {
            const auto started = nowMs();
            bool unchanged = false, visible = false;
            requireBenchmark(SUCCEEDED(desktop->verifyIsolation(&unchanged)) && unchanged,
                "Benchmark changed the input desktop");
            requireBenchmark(SUCCEEDED(desktop->visibleWindowsOnInputDesktop(visible)),
                "Observe own windows on the input desktop");
            inputDesktopUnchanged = unchanged;
            visibleInputWindows = visibleInputWindows || visible;
            ++desktopObservations;
            requireBenchmark(!visible, "Benchmark displayed a window on the input desktop");
            const auto elapsed = nowMs() - started;
            desktopObservationMs += elapsed;
            maximumDesktopObservationMs = std::max(maximumDesktopObservationMs, elapsed);
        };
        observeDesktop();
        requireBenchmark(!IsWindowVisible(window_), "Benchmark host must stay hidden");
        struct PumpWake {
            HANDLE value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            ~PumpWake(){if(value)CloseHandle(value);}
        } pumpWake;
        requireBenchmark(pumpWake.value!=nullptr,"Create bounded COM-dispatch wait handle");
        const char* waitPhase = "navigation";
        auto pumpUntil = [this, &observeDesktop, &waitPhase, &pumpWake, &desktopObservationMs,
                          &maximumDesktopObservationMs, &desktopObservations](const std::function<bool()>& predicate, DWORD timeout) {
            const auto deadline = GetTickCount64() + timeout;
            const auto firstGeneration = namespaceGeneration_;
            const auto firstUpdates = commandTimings_.updateCount;
            const auto firstObservations = desktopObservations;
            const auto firstObservationMs = desktopObservationMs;
            const auto firstCurrentViewEvents = headlessCurrentViewStateEvents_;
            const auto firstStaleViewEvents = headlessStaleViewStateEvents_;
            while (!predicate()) {
                if (GetTickCount64() >= deadline) {
                    std::fprintf(stderr,"Benchmark wait timed out: phase=%s batch=%u tasks=%zu namespaceDirty=%u selectionDirty=%u navigating=%u clipboardChanged=%u\n",
                        waitPhase,selectionStateBatch_?1u:0u,commandStateTasks_.size(),namespaceDirty_?1u:0u,
                        selectionStateDirty_?1u:0u,navigating_?1u:0u,GetClipboardSequenceNumber()!=clipboardSequence_?1u:0u);
                    std::fprintf(stderr,"Benchmark wait diagnostics: generations=%llu updates=%llu observations=%u observation_ms=%.3f maximum_observation_ms=%.3f\n",
                        static_cast<unsigned long long>(namespaceGeneration_-firstGeneration),
                        commandTimings_.updateCount-firstUpdates,desktopObservations-firstObservations,
                        desktopObservationMs-firstObservationMs,maximumDesktopObservationMs);
                    for(size_t index=0;index<firstCurrentViewEvents.size();++index)
                        std::fprintf(stderr,"Benchmark view events: kind=%zu current=%llu stale=%llu\n",index,
                            headlessCurrentViewStateEvents_[index]-firstCurrentViewEvents[index],
                            headlessStaleViewStateEvents_[index]-firstStaleViewEvents[index]);
                    for(const auto& [id,capability]:commandCapabilities_)if(capability.status==E_PENDING)
                        std::fprintf(stderr,"Benchmark pending command: id=%u scope=%u selectionVerbs=%zu task=%u\n",
                            id,static_cast<unsigned>(capability.binding.scope),capability.selectionVerbs.size(),commandStateTasks_.contains(id)?1u:0u);
                    for(const auto& [id,task]:commandStateTasks_) {
                        NamespaceCommandStateTimings timings;
                        NamespaceCommandState state;
                        const auto status=task->poll(&state);
                        const auto measured=task->pollTimings(&timings);
                        std::fprintf(stderr,"Benchmark task: id=%u completed=%u status=0x%08lX timings=0x%08lX worker_us=%llu query_us=%llu\n",
                            id,task->completed()?1u:0u,static_cast<unsigned long>(status),static_cast<unsigned long>(measured),
                            timings.workerMicroseconds,timings.menuQueryMicroseconds);
                    }
                    StaWorkerDiagnostics workers;
                    const auto workerStatus=staWorkerDiagnostics(&workers);
                    std::fprintf(stderr,"Benchmark workers: status=0x%08lX pending=%u terminated=%u creatorWindows=%u otherWindows=%u\n",
                        static_cast<unsigned long>(workerStatus),workers.pending,workers.completedThreads,workers.creatorWindows,workers.otherWindows);
                    requireBenchmark(false,"Native benchmark timed out");
                }
                // A headless wait must dispatch incoming apartment calls as
                // well as window messages. A window-only loop can strand a
                // correctly marshaled native view during menu-state queries.
                DWORD signaled=0;
                const auto waited=CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS|COWAIT_DISPATCH_WINDOW_MESSAGES,
                    5,1,&pumpWake.value,&signaled);
                requireBenchmark(SUCCEEDED(waited)||waited==RPC_S_CALLPENDING,"Dispatch native benchmark apartment calls");
                MSG message{};
                unsigned dispatched = 0;
                while (dispatched < 16 && GetTickCount64() < deadline &&
                       PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    requireBenchmark(message.message != WM_QUIT, "Benchmark received quit");
                    if (!preprocess(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
                    ++dispatched;
                }
                requireBenchmark(!IsWindowVisible(window_), "Benchmark showed its host");
                observeDesktop();
            }
        };
        requireBenchmark(std::isfinite(startup.clockStartMs) && startup.clockStartMs > 0 &&
            std::isfinite(startup.desktopReadyMs) && startup.desktopReadyMs >= 0 &&
            std::isfinite(startup.platformReadyMs) && startup.platformReadyMs >= startup.desktopReadyMs &&
            std::isfinite(startup.createReturnedMs) && startup.createReturnedMs >= startup.platformReadyMs,
            "Startup phase observations must use the process entry clock");
        const auto requestedStartup = navigating_ ? pendingPidl_.get() : currentPidl_.get();
        Pidl startupTarget(requestedStartup ? ILCloneFull(requestedStartup) : nullptr);
        requireBenchmark(startupTarget != nullptr, "Retain actual initial native navigation target");
        bool startupTargetMatched = false;
        pumpUntil([this, &startupTarget, &startupTargetMatched] {
            if (closing_ || !folderView_ || navigating_ || !navigationCount_ || !currentPidl_) return false;
            const auto startupView = folderView_;
            const auto startupNavigation = navigationCount_;
            Pidl startupCurrent(ILCloneFull(currentPidl_.get()));
            if (!startupCurrent) return false;
            ComPtr<IShellFolder> nativeFolder;
            auto read = startupView->GetFolder(IID_PPV_ARGS(&nativeFolder));
            PIDLIST_ABSOLUTE raw = nullptr;
            if (SUCCEEDED(read)) read = SHGetIDListFromObject(nativeFolder.Get(), &raw);
            Pidl actual(raw);
            if (FAILED(read) || !actual) return false;
            ComPtr<IShellItem> requested, realized;
            read = SHCreateItemFromIDList(startupTarget.get(), IID_PPV_ARGS(&requested));
            if (SUCCEEDED(read)) read = SHCreateItemFromIDList(actual.get(), IID_PPV_ARGS(&realized));
            int order = 1;
            if (SUCCEEDED(read)) read = requested->Compare(realized.Get(), SICHINT_CANONICAL, &order);
            // Native folder/identity calls can dispatch a navigation callback.
            // A replaced view cannot establish this startup observation.
            startupTargetMatched = SUCCEEDED(read) && order == 0 && !closing_ && !navigating_ &&
                folderView_.Get() == startupView.Get() && navigationCount_ == startupNavigation && currentPidl_ &&
                ILIsEqual(startupCurrent.get(), currentPidl_.get());
            return startupTargetMatched;
        }, 15000);
        const auto startupViewReadyMs = nowMs() - startup.clockStartMs;
        requireBenchmark(std::isfinite(startupViewReadyMs) && startupViewReadyMs >= startup.createReturnedMs,
            "Initial native view readiness must follow creation return");
        BenchmarkFixture fixture;
        const auto empty = fixture.folder(0);
        std::vector<Case> cases;
        for (const unsigned count : {10u, 1000u, 10000u}) {
            const auto folder = fixture.folder(count);
            Case result{count, {}};
            for (unsigned iteration = 0; iteration < 5; ++iteration) {
                auto before = navigationCount_;
                requireBenchmark(SUCCEEDED(navigate(empty.wstring())), "Navigate empty benchmark folder");
                pumpUntil([this, before] { return navigationCount_ > before && !navigating_; }, 15000);
                before = navigationCount_;
                const auto started = nowMs();
                requireBenchmark(SUCCEEDED(navigate(folder.wstring())), "Navigate populated benchmark folder");
                NavigationSample sample;
                bool firstSeen = false, completed = false;
                pumpUntil([this, before, count, started, &sample, &firstSeen, &completed] {
                    if (!folderView_ || navigationCount_ <= before || navigating_) return false;
                    if (!completed) {
                        sample.navigation = nowMs() - started;
                        sample.nativeCallback = static_cast<double>(lastNavigationMs_);
                        completed = true;
                    }
                    int items = 0;
                    if (FAILED(folderView_->ItemCount(SVGIO_ALLVIEW, &items))) return false;
                    if (items > 0 && !firstSeen) { sample.firstItem = nowMs() - started; firstSeen = true; }
                    if (items != static_cast<int>(count)) return false;
                    sample.populated = nowMs() - started;
                    return true;
                }, 30000);
                result.samples.push_back(sample);
            }
            cases.push_back(std::move(result));
        }
        // These are real native-view changes. UI work is pumped between each
        // request; record latency without claiming a stock Explorer comparison.
        std::vector<double> viewChanges;
        for (const auto mode : {ViewMode::Details, ViewMode::List, ViewMode::SmallIcons,
                                ViewMode::LargeIcons, ViewMode::Tiles, ViewMode::Details}) {
            const auto started = nowMs();
            requireBenchmark(SUCCEEDED(setView(mode)), "Change native benchmark view");
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (!preprocess(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            viewChanges.push_back(nowMs() - started);
            observeDesktop();
        }
        // A hidden native view must still enter its ordinary active state for
        // documented OLE commands. This takes no keyboard focus and remains
        // on the private desktop throughout the benchmark.
        requireBenchmark(SUCCEEDED(view_->UIActivate(SVUIA_ACTIVATE_NOFOCUS)),
            "Activate owned native view without taking focus");
        {
            ComPtr<IOleCommandTarget> target;
            auto hr = view_.As(&target);
            OLECMD command{OLECMDID_SELECTALL, 0};
            if (SUCCEEDED(hr)) hr = target->QueryStatus(nullptr, 1, &command, nullptr);
            std::fprintf(stderr, "Owned view SelectAll status HRESULT=0x%08lX, flags=0x%08lX.\n",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(command.cmdf));
        }
        std::vector<SelectionSample> selectionChanges;
        for (const auto [command, action, expected] : {
            std::tuple<UINT, const char*, int>{SelectAll, "selectAll", 10000},
            {Invert, "invertAllToNone", 0}, {Invert, "invertNoneToAll", 10000},
            {SelectNone, "selectNone", 0}}) {
            resetHeadlessCommandTimings();
            const auto generationBefore=namespaceGeneration_;
            const auto equivalentBefore=headlessEquivalentSelectionRefreshes_;
            const auto uncertainBefore=headlessUncertainSelectionRefreshes_;
            const auto started = nowMs();
            const auto selectionResult = execute(command);
            const auto commandCompleted = nowMs();
            if (FAILED(selectionResult))
                std::fprintf(stderr, "Selection %s failed (HRESULT=0x%08lX).\n",
                    action, static_cast<unsigned long>(selectionResult));
            requireBenchmark(SUCCEEDED(selectionResult), "Change native benchmark selection");
            // Observe the native count before deferred creator callbacks run.
            // The historical duration below also includes draining that queue;
            // it is not the first observation of the completed selection.
            int immediateCount = -1;
            const auto immediateReadStarted = nowMs();
            const auto immediateReadResult = folderView_->ItemCount(SVGIO_SELECTION, &immediateCount);
            const auto immediateReadCompleted = nowMs();
            requireBenchmark(SUCCEEDED(immediateReadResult) && immediateCount >= 0 && immediateCount <= 10000,
                "Read native selection count immediately after command");
            // Include the actual deferred host/Ribbon work, not just the time
            // required to queue selection notifications.
            MSG message{};
            const auto deadline = GetTickCount64() + 45000;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                requireBenchmark(GetTickCount64() < deadline && message.message != WM_QUIT,
                    "Native selection message pump exceeded bound");
                if (!preprocess(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
                requireBenchmark(!IsWindowVisible(window_), "Selection benchmark showed its host");
                observeDesktop();
            }
            const auto deferredCompleted = nowMs();
            int count = -1;
            const auto counted = folderView_->ItemCount(SVGIO_SELECTION, &count);
            const auto readbackCompleted = nowMs();
            if (FAILED(counted) || count != expected)
                std::fprintf(stderr, "Selection %s: expected %d, observed %d, HRESULT=0x%08lX.\n",
                    action, expected, count, static_cast<unsigned long>(counted));
            requireBenchmark(SUCCEEDED(counted) && count == expected,
                "Native benchmark selection count differs from owned fixture");
            // Keep the count-visible boundary comparable with earlier runs.
            // Separately wait for actual asynchronous provider states; merely
            // emptying the message queue does not prove those states are ready.
            waitPhase = action;
            pumpUntil([this] { pollCommandStates(); return !commandStatesPending(); }, 45000);
            requireBenchmark(selectionKindsRequest_.status==S_OK&&selectionKinds_.count==static_cast<DWORD>(expected)&&
                !selectionKindsRequest_.pending,"Native complete-selection Kind result did not finish for the actual selection");
            for(const auto& [id,capability]:commandCapabilities_) {
                if(capability.status==E_PENDING) {
                    std::fprintf(stderr,"Benchmark provider returned unresolved state after worker completion: id=%u HRESULT=0x%08lX\n",
                        id,static_cast<unsigned long>(capability.status));
                    requireBenchmark(false,"Native provider state remained unresolved after worker completion");
                }
            }
            const auto stateReady = nowMs();
            selectionChanges.push_back({action, readbackCompleted - started,
                commandCompleted - started, deferredCompleted - commandCompleted,
                readbackCompleted - deferredCompleted, stateReady - readbackCompleted,
                count, headlessCommandTimings(),namespaceGeneration_-generationBefore,
                headlessEquivalentSelectionRefreshes_-equivalentBefore,
                headlessUncertainSelectionRefreshes_-uncertainBefore, immediateCount,
                immediateReadCompleted - started, immediateReadCompleted - immediateReadStarted});
        }
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        requireBenchmark(GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE, "Read own benchmark memory");
        requireBenchmark(!IsWindowVisible(window_), "Benchmark host visibility changed");
        std::vector<double> commandUpdates;
        for (unsigned iteration = 0; iteration < 20; ++iteration) {
            const auto started = nowMs();
            updateCommands();
            requireBenchmark(SUCCEEDED(ribbon_.flush()), "Flush actual native command state");
            commandUpdates.push_back(nowMs() - started);
        }
        // Leave the fixture before removing it; do not retain a native view of
        // the owned temporary files during cleanup.
        const auto before = navigationCount_;
        requireBenchmark(SUCCEEDED(navigate(empty.wstring())), "Leave populated benchmark view");
        pumpUntil([this, before] { return navigationCount_ > before && !navigating_; }, 15000);
        destroyBrowser();
        observeDesktop();
        std::ostringstream output;
        output << std::fixed << std::setprecision(3)
            << "{\n  \"schema\":1,\"headless\":true,\"privateDesktop\":true,"
            << "\"inputDesktopUnchanged\":" << (inputDesktopUnchanged ? "true" : "false")
            << ",\"visibleInputDesktopWindows\":" << (visibleInputWindows ? "true" : "false")
            << ",\"desktopVisibilityObservations\":" << desktopObservations
            << ",\"passed\":true,\"samplesPerFolder\":5,"
            << "\"measurement\":\"hidden native Shell view; cold-first plus warm samples; no stock Explorer baseline\",\n"
            << "  \"startup\":{\"measurement\":\"process entry to first native view; excludes image loading and visible paint\","
            << "\"privateDesktopReadyMs\":" << startup.desktopReadyMs
            << ",\"platformReadyMs\":" << startup.platformReadyMs
            << ",\"createReturnedMs\":" << startup.createReturnedMs
            << ",\"nativeViewReadyMs\":" << startupViewReadyMs
            << ",\"nativeTargetMatched\":" << (startupTargetMatched ? "true" : "false") << "},\n"
            << "  \"workingSetBytes\":" << memory.WorkingSetSize << ",\"privateBytes\":" << memory.PrivateUsage << ",\n"
            << "  \"ribbonLayout\":\"" << (ribbon_.layout() == RibbonLayout::InstalledWindows10 ? "InstalledWindows10" : "Authored")
            << "\",\"installedRibbonStatus\":" << static_cast<long>(ribbon_.installedLayoutStatus()) << ",\n"
            << "  \"cases\":[\n";
        for (size_t index = 0; index < cases.size(); ++index) {
            const auto& result = cases[index];
            if (index) output << ",\n";
            output << "    {\"items\":" << result.count << ",\"navigation\":";
            summary(output, result.samples, &NavigationSample::navigation);
            output << ",\"nativeNavigationCallback\":"; summary(output, result.samples, &NavigationSample::nativeCallback);
            output << ",\"firstItem\":"; summary(output, result.samples, &NavigationSample::firstItem);
            output << ",\"fullyPopulated\":"; summary(output, result.samples, &NavigationSample::populated);
            output << ",\"samples\":[";
            for (size_t sample = 0; sample < result.samples.size(); ++sample) {
                if (sample) output << ',';
                const auto& value = result.samples[sample];
                output << "{\"navigationMs\":" << value.navigation << ",\"nativeNavigationCallbackMs\":" << value.nativeCallback
                       << ",\"firstItemMs\":" << value.firstItem
                       << ",\"fullyPopulatedMs\":" << value.populated << '}';
            }
            output << "]}";
        }
        output << "\n  ],\"viewChangeMs\":[";
        for (size_t index = 0; index < viewChanges.size(); ++index) {
            if (index) output << ',';
            output << viewChanges[index];
        }
        output << "],\n  \"selectionChangeMs\":[";
        for (size_t index = 0; index < selectionChanges.size(); ++index) {
            if (index) output << ',';
            const auto& sample = selectionChanges[index];
            output << "{\"action\":\"" << sample.action << "\",\"durationMs\":" << sample.duration
                   << ",\"commandMs\":" << sample.command << ",\"deferredWorkMs\":" << sample.deferredWork
                   << ",\"countReadbackMs\":" << sample.countReadback
                   << ",\"commandStateReadyAfterReadbackMs\":" << sample.commandStateReady
                   << ",\"commandStateReadyTotalMs\":" << sample.duration + sample.commandStateReady
                   << ",\"selected\":" << sample.selected
                   << ",\"immediateSelectedCount\":" << sample.immediateSelected
                   << ",\"immediateCountReadAtMs\":" << sample.immediateCountReadAt
                   << ",\"immediateCountReadCostMs\":" << sample.immediateCountReadCost
                   << ",\"generationChanges\":" << sample.generationChanges
                   << ",\"equivalentSelectionRefreshes\":" << sample.equivalentSelectionRefreshes
                   << ",\"uncertainSelectionRefreshes\":" << sample.uncertainSelectionRefreshes;
            const auto& work = sample.hostWork;
            output << ",\"hostWork\":{\"status\":" << static_cast<long>(work.status)
                   << ",\"updateCount\":" << work.updateCount << ",\"totalUpdateMs\":" << work.totalMs
                   << ",\"viewReadbackMs\":" << work.viewReadbackMs
                   << ",\"selectionCountAttributesMs\":" << work.selectionCountAttributesMs
                   << ",\"selectionStatusMs\":" << work.selectionStatusMs
                   << ",\"selectionHostEligibilityMs\":" << work.selectionHostEligibilityMs
                   << ",\"selectionKindsMs\":" << work.selectionKindsMs
                   << ",\"selectionKindsSchedulingMs\":" << work.selectionKindsSchedulingMs
                   << ",\"selectionKindsPublicationMs\":" << work.selectionKindsPublicationMs
                   << ",\"selectionKindsReadyDelayMs\":" << work.selectionKindsReadyDelayMs
                   << ",\"selectionKindsStatus\":" << static_cast<long>(work.selectionKindsStatus)
                   << ",\"namespacePreparationMs\":" << work.namespacePreparationMs
                   << ",\"providerCatalogMs\":" << work.providerCatalogMs
                   << ",\"stateTaskSchedulingMs\":" << work.stateTaskSchedulingMs
                   << ",\"contextMs\":" << work.contextMs
                   << ",\"ribbonInvalidationMs\":" << work.ribbonInvalidationMs
                   << ",\"measurement\":\"creator STA phase totals; nested phases and direct context calls are not additive\""
                   << ",\"completedStateWorkers\":[";
            for(size_t workerIndex=0;workerIndex<work.completedStateWorkers.size();++workerIndex) {
                if(workerIndex)output << ',';
                const auto& worker=work.completedStateWorkers[workerIndex];
                output << "{\"command\":" << worker.command << ",\"selectionBatch\":" << (worker.selectionBatch?"true":"false")
                       << ",\"selectionKinds\":" << (worker.selectionKinds?"true":"false")
                       << ",\"status\":" << static_cast<long>(worker.status)
                       << ",\"timingStatus\":" << static_cast<long>(worker.timingStatus)
                       << ",\"workerMs\":" << worker.native.workerMicroseconds/1000.0
                       << ",\"kindReadMs\":" << worker.native.kindReadMicroseconds/1000.0
                       << ",\"dataObjectExportMs\":" << worker.native.dataObjectExportMicroseconds/1000.0
                       << ",\"identityConstructionMs\":" << worker.native.identityConstructionMicroseconds/1000.0
                       << ",\"contextBindMs\":" << worker.native.contextBindMicroseconds/1000.0
                       << ",\"menuQueryMs\":" << worker.native.menuQueryMicroseconds/1000.0
                       << ",\"menuEnumerationMs\":" << worker.native.menuEnumerationMicroseconds/1000.0
                       << ",\"stateReductionMs\":" << worker.native.stateReductionMicroseconds/1000.0 << '}';
            }
            output << "]}}";
        }
        output << "]\n}\n";
        auto text = output.str();
        const auto close = text.rfind("\n}");
        std::ostringstream commandOutput;
        commandOutput << std::fixed << std::setprecision(3) << ",\n  \"cachedCommandUpdateMs\":[";
        for (size_t index = 0; index < commandUpdates.size(); ++index) {
            if (index) commandOutput << ',';
            commandOutput << commandUpdates[index];
        }
        commandOutput << ']';
        text.insert(close, commandOutput.str());
        const auto file = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        requireBenchmark(file != INVALID_HANDLE_VALUE, "Create unique benchmark report");
        DWORD written = 0;
        const bool saved = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        CloseHandle(file);
        requireBenchmark(saved, "Write benchmark report");
        return 0;
    } catch (const std::exception& error) {
        OutputDebugStringA(error.what());
        std::fprintf(stderr, "FAIL: headless native benchmark: %s\n", error.what());
        return 1;
    }
}
}
