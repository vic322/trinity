#include "focused_tick.h"
#include "player.h"
#include "offsets.h"
#include "../mem/hooks.h"
#include <atomic>

namespace trinity::game {
namespace {
using TickFn = uint64_t(__fastcall*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
TickFn original = nullptr;
void* target = nullptr;
std::atomic_flag busy = ATOMIC_FLAG_INIT;
uint64_t __fastcall Tick(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g) {
    const auto result = original(a,b,c,d,e,f,g);
    if (!busy.test_and_set(std::memory_order_acquire)) {
        __try { Player::Tick(); }
        __except(EXCEPTION_EXECUTE_HANDLER) { }
        busy.clear(std::memory_order_release);
    }
    return result;
}
}
bool InstallFocusedTick() {
    return mem::InstallHook("focused player tick",kSig_MoveUpdate,"player controls unavailable",&Tick,&original,&target);
}
void RemoveFocusedTick() { mem::RemoveHook(&target); }
}
