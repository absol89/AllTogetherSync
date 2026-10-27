#pragma once

// Failing loudly instead of silently. A plugin that hits something it cannot handle logs it and crashes on purpose,
// so CrashLogger records the stack; a game that stops running its main thread gets a dump of every thread written
// next to the logs, so a freeze leaves the same evidence a crash does.

#include "Common/Together.h"

#include <DbgHelp.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <thread>

namespace Together::Safety
{
// "TOG": the exception code of a deliberate crash, so a crash log tells it apart from an access violation.
inline constexpr DWORD kFailException = 0xE0544F47;
// At most this many dumps per game start: a stall that keeps coming back must not fill the disk.
inline constexpr int kMaxDumps = 3;
// The main thread counts as frozen after this long without running an SKSE task (a loading screen still runs them).
inline constexpr auto kFreezeThreshold = std::chrono::seconds(20);

inline const char* g_pPluginName = "plugin";

// Logs, then crashes so CrashLogger writes a log with this plugin's stack. For states the plugin cannot recover from.
[[noreturn]] inline void Fail(std::string_view aWhat)
{
    SKSE::log::critical("{}: {}; crashing on purpose so the crash log shows where", g_pPluginName, aWhat);
    spdlog::default_logger()->flush();
    RaiseException(kFailException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

// A C++ exception that escapes this plugin would otherwise end the game through abort(), which leaves no crash log.
inline void InstallTerminateHandler(const char* acpPluginName)
{
    g_pPluginName = acpPluginName;
    std::set_terminate(
        []
        {
            std::string what = "unhandled C++ exception";
            if (const auto cException = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(cException);
                }
                catch (const std::exception& acException)
                {
                    what = std::format("unhandled C++ exception: {}", acException.what());
                }
                catch (...)
                {
                }
            }
            Fail(what);
        });
}

// Writes a dump of every thread (stacks, modules, handles; not the whole heap) to the SKSE log folder. False once the
// cap is reached or when it could not be written.
inline bool WriteDump(const char* acpReason)
{
    static std::atomic<int> s_written{0};
    if (s_written.fetch_add(1) >= kMaxDumps)
        return false;

    auto path = SKSE::log::log_directory();
    if (!path)
        return false;

    SYSTEMTIME time;
    GetLocalTime(&time);
    *path /= std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", g_pPluginName, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);

    const HANDLE cFile = CreateFileW(path->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (cFile == INVALID_HANDLE_VALUE)
        return false;

    const auto cType = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    const bool cWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), cFile, cType, nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(cFile);

    if (cWritten)
        SKSE::log::error("{}: wrote {} ({})", g_pPluginName, path->string(), acpReason);
    else
        SKSE::log::error("{}: could not write a dump ({}), error {}", g_pPluginName, acpReason, GetLastError());
    return cWritten;
}

// Watches the game's main thread while connected: it posts an SKSE task every second and writes one dump per stall
// when none has run for kFreezeThreshold. One watchdog per process, whichever plugin starts it first.
inline void StartFreezeWatchdog(STAPI::IVSkyrimTogether2* apApi)
{
    const auto cName = std::format(L"Local\\SkyrimTogetherPluginWatchdog_{}", GetCurrentProcessId());
    const HANDLE cMutex = CreateMutexW(nullptr, FALSE, cName.c_str());
    if (!cMutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return; // kept open for the life of the process, so the other plugin sees it

    using Clock = std::chrono::steady_clock;
    static std::atomic<Clock::rep> s_lastBeat{Clock::now().time_since_epoch().count()};

    std::thread(
        [apApi]
        {
            bool dumpedThisStall = false;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                SKSE::GetTaskInterface()->AddTask([] { s_lastBeat = Clock::now().time_since_epoch().count(); });

                const auto cSilence = Clock::now() - Clock::time_point(Clock::duration(s_lastBeat.load()));
                if (cSilence < kFreezeThreshold)
                {
                    if (dumpedThisStall)
                        SKSE::log::info("{}: the main thread runs again", g_pPluginName);
                    dumpedThisStall = false;
                    continue;
                }

                if (!dumpedThisStall && apApi->GetLocalPlayerId() != 0)
                {
                    dumpedThisStall = true;
                    const auto cSeconds = std::chrono::duration_cast<std::chrono::seconds>(cSilence).count();
                    SKSE::log::error("{}: the game's main thread has not run for {} s while connected", g_pPluginName, cSeconds);
                    WriteDump("main thread frozen");
                }
            }
        })
        .detach();
}
} // namespace Together::Safety
