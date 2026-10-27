// LipSync Together 2: moves Skyrim Together players' mouths while they talk, and optionally carries their voices, as a
// standalone SKSE plugin on Skyrim Together's Plugin API. Nothing is patched into Skyrim Together.
//
// One DLL with three parts: LipSync Together 0.8's microphone and voice code (plugin/, unchanged apart from no longer
// exporting its descriptor), the Skyrim Together side the 0.8 add-on kept in the exe (Host.cpp), and its settings
// window on SKSE Menu Framework (Window.cpp). It replaces both halves of 0.8, the DLL and the add-on. Builds that
// still carry the 0.8 add-on leave it off, as it looks for the 0.8 descriptor export.

#include "Common/Safety.h"

#include "Plugin.h"
#include "Window.h"

#include <spdlog/sinks/base_sink.h>

namespace Host
{
bool Start();
void Run();
} // namespace Host

namespace
{
constexpr const char* kPluginName = "LipSyncTogether";

// LipSync Together keeps its own log; what goes through spdlog (the crash and freeze reports) ends up there too.
class ForwardSink final : public spdlog::sinks::base_sink<std::mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& acMessage) override
    {
        LipSync::Log("%.*s", static_cast<int>(acMessage.payload.size()), acMessage.payload.data());
    }
    void flush_() override {}
};

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    switch (apMessage->type)
    {
    case SKSE::MessagingInterface::kPostLoad:
        // Every SKSE plugin is loaded by now, SKSE Menu Framework included.
        if (!Window::Register(LipSync::GetDescriptor()))
            LipSync::Log("SKSE Menu Framework is not installed: no settings window; LipSyncTogether.ini still works");
        break;
    case SKSE::MessagingInterface::kDataLoaded:
        Host::Run();
        if (auto* pApi = Together::FindApi())
            Together::Safety::StartFreezeWatchdog(pApi);
        break;
    default: break;
    }
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* apSkse)
{
    SKSE::Init(apSkse);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("global", std::make_shared<ForwardSink>()));
    Together::Safety::InstallTerminateHandler(kPluginName);

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OnSkseMessage), &module);
    const auto cVersion = REL::Module::get().version();
    LipSync::Start(module, (static_cast<std::uint32_t>(cVersion.major()) << 24) | (static_cast<std::uint32_t>(cVersion.minor()) << 16) |
                               (static_cast<std::uint32_t>(cVersion.patch()) << 4));

    if (!Host::Start())
    {
        LipSync::Log("Skyrim Together's Plugin API was not found: LipSync Together stays off (the microphone stays closed)");
        return true;
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    return true;
}
