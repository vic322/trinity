#include "mod.h"
#include <MinHook.h>
#include "logger.h"
#include "state.h"
#include "localization.h"
#include "version_detect.h"
#include "readiness.h"
#include "../hooks/dx12_hook.h"
#include "../game/player.h"
#include "../game/focused_tick.h"
#include "../game/offsets.h"
#include "../mem/scanner.h"
namespace trinity {
void Mod::Initialize(HMODULE module) {
    if (m_initialized) return;
    m_module=module;
    LOG("Focused Combat Menu based on Trinity - local build for PE revision 2850.");
    if (core::GetGameVersion().revision != 2850) {
        LOG_ERR("Unsupported game revision: no hooks installed.");
        return;
    }
    loc::Init();
    State::Get() = State{};
    State::Get().autoSave = false;
    State::Get().showConsole = false;
    if (MH_Initialize()!=MH_OK) return;
    if (!hooks::InstallDX12Hooks()) { MH_Uninitialize(); return; }
    const bool ready=core::WaitForReadiness([] {
        return mem::FindPattern(game::kSig_DamageApply) != 0 &&
               mem::FindPattern(game::kSig_MoveUpdate) != 0;
    }, [] {return GetTickCount64();}, [](uint32_t ms){Sleep(ms);},180000,2000);
    if (ready && game::Player::Install()) {
        if (!game::InstallFocusedTick()) game::Player::Remove();
    } else {
        LOG_ERR("Combat hooks unavailable. Controls remain inactive.");
    }
    m_initialized=true;
    LOG_OK("Ready. TILDE/BACKTICK (no Shift) or LB + D-pad Down opens the combat menu. All cheats start OFF.");
}
void Mod::Shutdown() {
    if (!m_initialized) return;
    game::RemoveFocusedTick();
    game::Player::Remove();
    hooks::RemoveDX12Hooks();
    MH_Uninitialize();
    Logger::Shutdown();
    m_initialized=false;
}
}
