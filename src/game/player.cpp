#include "player.h"
#include "teleport.h"
#include "inventory.h"

#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <cstdint>
#include <iterator>

#include <MinHook.h>

#pragma comment(lib, "xinput9_1_0.lib")

#include "offsets.h"
#include "../mem/scanner.h"
#include "../mem/safe_memory.h"
#include "../mem/hooks.h"
#include "../core/logger.h"
#include "../core/state.h"
#include "../core/version_mapping.h"
#include "../core/version_detect.h"
#include "player_logic.h"

namespace trinity::game
{
    using mem::ReadPtr;
    using mem::Read64;
    using mem::Read32;
    using mem::Read8;
    using mem::Write64;

    namespace
    {
        // --- Fresh player-set resolution (character-manager global) --------
        // Crimson Desert is a three-protagonist game: Kliff plus two companions,
        // all of them controllable, summonable, and able to coexist in the world
        // at once (swapped between, or the other two summoned as companions). The
        // stat features must cover EVERY active protagonist, not just the single
        // locally-possessed body the engine's own accessor (sub_2393AA0) returns.
        //
        // The character you actively control is NOT reliably tagged SelfPlayer
        // (playing a secondary protagonist re-tags it Mercenary), so we identify
        // protagonists by their shared character-class VTABLE + a live vital
        // chain instead of by tag - see TickResolveSelf. We resolve the whole
        // SET fresh every game tick from the gameplay-character manager, never
        // cached: a body transition (mount / transform / character swap)
        // reallocates a character, but the next resolve simply rebuilds the set,
        // so there is no stale-pointer churn to track. See offsets.h
        // (kCharMgrAnchors) and the trinity-engine-architecture notes.
        //
        // Resolved address of the qword_6181090 slot; the manager is
        // *(*g_charMgrGlobal). Zero if no anchor resolved, in which case every
        // stat feature below is inert (RefreshSelf no-ops).
        uintptr_t g_charMgrGlobal = 0;
        std::atomic<bool> g_currentFallbackLogged{false};

        // Resolve the char-manager global by consensus across kCharMgrAnchors.
        // Each anchor is an independent call site that RIP-resolves the same
        // global, so they act as each other's check: we take the value the most
        // anchors agree on, and log loudly on any disagreement or on anchors
        // that stopped matching. A single update is very unlikely to break all
        // of them, and the vote means a lone stale survivor pointing at a
        // sibling realm's manager (see offsets.h) cannot silently win.
        uintptr_t ResolveCharMgrGlobal()
        {
            constexpr int kN = static_cast<int>(std::size(kCharMgrAnchors));
            uintptr_t vals[kN] = {};
            int votes[kN] = {};
            int distinct = 0, matched = 0;

            for (const CharMgrAnchor& a : kCharMgrAnchors)
            {
                const uintptr_t m = mem::FindPattern(a.sig);
                if (!m) continue;
                const uintptr_t g = mem::ResolveRipAt(m + a.movOff, 7);
                if (!g) continue;

                ++matched;
                int i = 0;
                for (; i < distinct; ++i)
                    if (vals[i] == g) { ++votes[i]; break; }
                if (i == distinct) { vals[distinct] = g; votes[distinct] = 1; ++distinct; }
            }

            if (!distinct) return 0;

            int best = 0;
            for (int i = 1; i < distinct; ++i)
                if (votes[i] > votes[best]) best = i;

            // Anchors that disagree mean at least one is matching the wrong site
            // (a sibling realm's manager resolves fine and then fails silently),
            // so surface it rather than trusting the winner blindly.
            if (distinct > 1)
                LOG_WARN("player: char-manager anchors DISAGREE (%d distinct values); "
                         "using %p with %d/%d votes - re-derive the anchors.",
                         distinct, reinterpret_cast<void*>(vals[best]), votes[best], matched);
            else if (matched < kN)
                LOG("player: char-manager successfully resolved (%d anchors verified).", matched);

            return vals[best];
        }

        // --- Structural fallback for the char-manager global -----------------
        //
        // Byte anchors key on the instructions *around* the global, so a Title
        // Update that recompiles those call sites takes them with it. Install()
        // gives up entirely when that happens, which turns one stale pattern
        // into "no gameplay hooks at all" - and on this build only one of the
        // five anchors still matches, so there is not much margin left.
        //
        // The manager's shape outlives the code that reaches it:
        //
        //     global -> P -> mgr,  mgr+ListData = character*[],  mgr+ListCount
        //
        // so walk the module's committed pages, treat every aligned qword as a
        // candidate global, and keep whichever one's double dereference lands on
        // that shape. Reads are SEH-guarded, so following a garbage chain costs
        // a failed read rather than the process.

        // Is `ch` the body actually being driven right now? The controller points
        // back at exactly the pawn it possesses, so this round-trip closes for the
        // live body and for nothing else.
        bool PossessorRoundTrips(uintptr_t ch)
        {
            uintptr_t poss = 0, pawn = 0;
            if (!ReadPtr(ch + kOff_Owner_Possessor, &poss) || poss < kMinPointer) return false;
            if (!ReadPtr(poss + kOff_Possessor_Pawn, &pawn)) return false;
            return pawn == ch;
        }

        // Does `mgr` look like the character manager, and does its list hold the
        // player? Returns a score, or -1 when the object has the wrong shape.
        int ScoreCharManager(uintptr_t mgr, int* outTagged, int* outDriven, uint32_t* outCount)
        {
            uintptr_t data  = 0;
            uint32_t  count = 0;
            if (!ReadPtr(mgr + kOff_CharMgr_ListData, &data) || data < kMinPointer) return -1;
            if (!Read32(mgr + kOff_CharMgr_ListCount, &count))                      return -1;
            if (count < 16 || count > kCharList_MaxCount)                            return -1;

            const mem::ModuleRegion& mod = mem::GameModule();
            const uintptr_t modBase = mod.base, modEnd = mod.base + mod.size;

            const uint32_t limit = (count < 2048u) ? count : 2048u;
            int valid = 0, tagged = 0, driven = 0;
            for (uint32_t i = 0; i < limit; ++i)
            {
                uintptr_t ch = 0;
                if (!ReadPtr(data + 8ull * i, &ch)) return -1; // array not readable -> not it
                if (ch < kMinPointer || (ch & 7) != 0) continue;

                // A real engine object starts with a vtable pointer into the
                // module. Without this, tables of item definitions sail through:
                // their embedded text satisfies every other test, and the "type
                // tag" is just a byte out of a string.
                uintptr_t vt = 0;
                if (!ReadPtr(ch, &vt)) continue;
                if (vt < modBase || vt >= modEnd || (vt & 7) != 0) continue;
                ++valid;

                // Test the round-trip on EVERY entry, not only tag-matching ones:
                // the controlled body is re-tagged while playing a secondary
                // protagonist, so gating this behind the tag hides the one
                // character that proves the list. Stop at the first hit - the
                // check chases two usually-garbage pointers, and running it over
                // every entry of every candidate costs tens of thousands of SEH
                // faults per pass.
                if (driven == 0 && PossessorRoundTrips(ch)) ++driven;

                uintptr_t td  = 0;
                uint8_t   tag = 0;
                if (!ReadPtr(ch + kOff_Owner_TypeDesc, &td) || td < kMinPointer) continue;
                if (!Read8(td + 1, &tag) || ((tag - 1) & 0xF7) != 0) continue;
                ++tagged;
            }
            // The real list is densely populated with live objects; a struct that
            // merely holds a pointer and a plausible integer is not.
            if (valid < 16 || valid * 4 < static_cast<int>(limit)) return -1;

            if (outTagged) *outTagged = tagged;
            if (outDriven) *outDriven = driven;
            if (outCount)  *outCount  = count;

            // Holding the possessed body is the entry requirement; among lists
            // that do, the widest one wins. Ranking by tagged count instead picks
            // the engine's pool of player-class slots, which resolves the
            // protagonists and then starves mount and vehicle discovery.
            if (driven < 1) return 0;
            return static_cast<int>(count);
        }

        uintptr_t ProbeCharMgrGlobal()
        {
            const mem::ModuleRegion& mod = mem::GameModule();
            if (!mod) return 0;

            const ULONGLONG t0 = GetTickCount64();
            const uintptr_t imageEnd = mod.base + mod.size;

            uintptr_t best = 0;
            int bestScore = 0, candidates = 0, bestTagged = 0, bestDriven = 0;
            uint32_t bestCount = 0;

            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t addr = mod.base; addr < imageEnd; )
            {
                if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
                const uintptr_t regBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const uintptr_t regEnd  = regBase + mbi.RegionSize;

                const bool readable =
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 &&
                    (mbi.Protect & PAGE_GUARD) == 0;

                if (readable)
                {
                    const uintptr_t stop = (regEnd < imageEnd) ? regEnd : imageEnd;
                    for (uintptr_t g = (regBase + 7) & ~uintptr_t(7); g + 8 <= stop; g += 8)
                    {
                        uintptr_t p = 0, mgr = 0;
                        if (!ReadPtr(g, &p) || p < kMinPointer || (p & 7) != 0)       continue;
                        if (!ReadPtr(p, &mgr) || mgr < kMinPointer || (mgr & 7) != 0) continue;

                        int tagged = 0, driven = 0;
                        uint32_t count = 0;
                        const int score = ScoreCharManager(mgr, &tagged, &driven, &count);
                        if (score <= 0) continue;

                        ++candidates;
                        LOG("player:   probe candidate base+0x%llX -> count=%u tagged=%d driven=%d",
                            static_cast<unsigned long long>(g - mod.base), count, tagged, driven);
                        if (score > bestScore)
                        {
                            bestScore = score; best = g;
                            bestTagged = tagged; bestDriven = driven; bestCount = count;
                        }
                    }
                }
                addr = (regEnd > addr) ? regEnd : addr + 0x1000;
            }

            const ULONGLONG ms = GetTickCount64() - t0;
            if (bestDriven >= 1)
            {
                LOG("player: char-manager recovered structurally at base+0x%llX (count=%u, "
                    "%d tagged, %d possessed, %d candidate(s), %llu ms). Byte anchors are stale "
                    "for this build - re-derive kCharMgrAnchors from this address.",
                    static_cast<unsigned long long>(best - mod.base), bestCount, bestTagged,
                    bestDriven, candidates, ms);
                return best;
            }
            LOG("player: probe pass saw %d candidate(s) but none with a possessed body (%llu ms).",
                candidates, ms);
            return 0;
        }

        // The probe has to run *after* the world exists: nothing is possessed on
        // the title screen, and the manager is not populated enough to even be a
        // candidate. A pass costs seconds, so it gets a thread of its own rather
        // than the game thread, and stops for good once a pass succeeds.
        DWORD WINAPI ProbeThread(LPVOID)
        {
            for (int pass = 1; pass <= 40; ++pass)
            {
                Sleep(5000);
                if (g_charMgrGlobal) return 0;
                const uintptr_t g = ProbeCharMgrGlobal();
                if (g) { g_charMgrGlobal = g; return 0; }
            }
            LOG_ERR("player: structural probe gave up after 40 passes - stat features stay "
                    "disabled.");
            return 0;
        }

        // The player set's stat entries + battle-damage identities, recomputed
        // each tick by RefreshSelf(). The stat hooks match against these live
        // sets instead of a historical cache: because they are always the
        // current bodies', membership (not a ring) is correct and self-healing.
        // Public sets have margin for scanning, but a VALID gameplay party can
        // never exceed the game's three protagonists. A fourth same-vtable body
        // is the character/equipment-menu preview actor and must never receive
        // stat writes.
        constexpr int kMaxPlayers      = 8;
        constexpr int kMaxPartyPlayers = 3;
        constexpr int kMaxGaugePerType = 16;                   // stamina/spirit gauges per body
        constexpr int kMaxStatEntries  = kMaxPlayers * kMaxGaugePerType;

        std::atomic<uintptr_t> g_hpEntries[kMaxPlayers]{};
        std::atomic<uintptr_t> g_stamEntries[kMaxStatEntries]{};
        std::atomic<uintptr_t> g_mountStamEntries[kMaxStatEntries]{};
        std::atomic<uintptr_t> g_spiritEntries[kMaxStatEntries]{};
        // Battle-damage identities, one per tracked player. A player's actor is
        // the attacker side of an outgoing hit; its vital/target owner (the
        // "root" object) is the victim side of an incoming one. A hit against any
        // protagonist is scaled by the incoming multiplier; a hit dealt by any of
        // them is scaled by the outgoing one. See the damage-apply hook below.
        std::atomic<uintptr_t> g_actors[kMaxPlayers]{};
        std::atomic<uintptr_t> g_owners[kMaxPlayers]{};
        std::atomic<uintptr_t> g_targetOwners[kMaxPlayers]{};

        constexpr int kMaxMounts = 4;
        std::atomic<uintptr_t> g_mountActors[kMaxMounts]{};
        std::atomic<uintptr_t> g_mountTargetOwners[kMaxMounts]{};
        std::atomic<uintptr_t> g_mountOwners[kMaxMounts]{};
        std::atomic<int>       g_mountCount{0};
        std::atomic<bool>      g_isRidingMount{false};
        std::atomic<uintptr_t> g_playerPossessor{0};
        std::atomic<uint64_t>  g_characterTrackingRequestedUntilMs{0};
        constexpr uint64_t     kCharacterTrackingDemandMs = 2000;

        // Stat commit (pa_StatCommit / IDB sub_BED7820) - the single funnel every
        // HP/Stamina/Spirit write passes through. God Mode, Infinite Stamina
        // and Infinite Spirit all guard it; see the hook below.
        using StatCommit_t = int64_t(__fastcall*)(void* entry, int64_t time, int64_t target, uint16_t flag);
        StatCommit_t oStatCommit = nullptr;
        void* g_commitTarget = nullptr;

        // Damage-apply dispatcher (pa_StatApplyDelta / IDB sub_145B2A0) - one
        // level above the commit funnel, the only site where a battle hit
        // still carries BOTH its victim (targetOwner) and its attacker
        // (sourceCtx). The damage multipliers scale the signed delta here.
        using DamageApply_t = int64_t(__fastcall*)(void* targetOwner, uint16_t statusId,
                                                   int64_t time, int64_t delta, uintptr_t sourceCtx,
                                                   char a6, char a7, char a8, char a9, char a10,
                                                   void* out);
        DamageApply_t oDamageApply = nullptr;
        void* g_damageHookTarget = nullptr;

        // --- Stat-entry typing --------------------------------------------
        bool StatEntryType(uintptr_t entry, int32_t* type)
        {
            uint32_t t = 0;
            if (!Read32(entry + kOff_StatEntry_Type, &t)) return false;
            *type = static_cast<int32_t>(t);
            return true;
        }

        constexpr int kMaxMountStamEntries = kMaxStatEntries;
        inline bool PlausibleStatType(int32_t t) { return t >= 0 && t < 256; }

        bool IsHealthType(int32_t t)  { return t == StatType_Health; }

        // Match authoritative stamina gauges in Crimson Desert (Sprint 20, Pool 22, Mount Gallop/Flight 19).
        // Strictly purged 17 & 18 (internal Heat/Combustion gauges) and 48 (Fire Breath) to completely prevent character auto-ignition.
        bool IsStaminaType(int32_t t) {
            return t == 22; // PE2850 verified stamina; exclude environmental meters.
        }

        // Both spirit-typed HUD gauges: 21 (SpiritPool) and 23 (SpiritPool117)
        bool IsSpiritType(int32_t t)  {
            return t == StatType_SpiritPool || t == StatType_SpiritPool117;
        }

        // True if `e` is a member of one of the resolved player sets (a tiny
        // linear scan; a fresh resolve keeps each set to just the live bodies').
        bool InSet(const std::atomic<uintptr_t>* set, int n, uintptr_t e)
        {
            if (e < kMinPointer) return false;
            for (int i = 0; i < n; ++i)
                if (set[i].load(std::memory_order_relaxed) == e) return true;
            return false;
        }

        // Force an entry's current value back to full (whichever of base/cap
        // holds the max), writing both representations the game reads.
        void PinEntry(uintptr_t e)
        {
            if (e < kMinPointer) return;
            uint64_t base = 0, cap = 0, cur = 0;
            Read64(e + kOff_StatEntry_Base, &base);
            Read64(e + kOff_StatEntry_Cap, &cap);
            Read64(e + kOff_StatEntry_Current, &cur);
            if (base > 1000000000ULL || cap > 1000000000ULL) return;
            uint64_t full = (cap > base) ? cap : base;
            if (!full && cur > 0 && cur < 1000000000ULL) full = cur;
            if (!full) return;
            Write64(e + kOff_StatEntry_Current, full);
            Write64(e + kOff_StatEntry_Norm,    full - base);
        }

        // The engine accessor's class gate: the type-descriptor tag byte at
        // *(owner+0x88)+1 is 1 (SelfPlayer) or 9 (OtherPlayer) for player-class
        // characters - ((tag - 1) & 0xF7) == 0 is the exact test sub_2393AA0
        // (and sub_30DF50) compiles to. This is the ONLY reliable type read:
        // the +0x48 objType word on the live player flickers (seen 0/3/8), so it
        // is not used. This gate identifies the protagonist character CLASS - we
        // use it only to derive that class's vtable (any pool slot will do); the
        // actual active-body selection is vtable-based (see TickResolveSelf),
        // because the body you control is re-tagged (Mercenary=4) when playing a
        // secondary protagonist and would slip past a tag-only test.
        bool IsPlayerClass(uintptr_t owner)
        {
            uint64_t td = 0;
            uint8_t tag = 0;
            return Read64(owner + kOff_Owner_TypeDesc, &td) && td >= kMinPointer &&
                   Read8(static_cast<uintptr_t>(td) + 1, &tag) && ((tag - 1) & 0xF7) == 0;
        }

        // The player's resolved identity/stat chain for one tick.
        struct SelfChain { uintptr_t actor, targetOwner, statArray; };

        // Walk owner -> actor(+0x68) -> marker(+0x20) -> root(+0x18) ->
        // statArray(+0x58). The array base is the Health entry (index 0);
        // return false unless it type-checks as Health, which validates the
        // whole chain before we trust it. `root` doubles as the vital/target
        // owner battle damage is addressed to.
        bool WalkSelfChain(uintptr_t owner, SelfChain* out)
        {
            uint64_t actor = 0, marker = 0, root = 0, arr = 0;
            if (!Read64(owner + kOff_Owner_Actor, &actor) || actor < kMinPointer) return false;
            if (!Read64(static_cast<uintptr_t>(actor) + kOff_Actor_StatusMarker, &marker) ||
                marker < kMinPointer) return false;
            if (!Read64(static_cast<uintptr_t>(marker) + kOff_Marker_TargetOwner, &root) ||
                root < kMinPointer) return false;
            if (!Read64(static_cast<uintptr_t>(root) + kOff_Root_StatArray, &arr) ||
                arr < kMinPointer) return false;
            int32_t t = 0;
            if (!StatEntryType(static_cast<uintptr_t>(arr), &t) || !IsHealthType(t)) return false;

            out->actor       = static_cast<uintptr_t>(actor);
            out->targetOwner = static_cast<uintptr_t>(root);
            out->statArray   = static_cast<uintptr_t>(arr);
            return true;
        }

        // Dedicated mount vital chain walker: does not require humanoid Health entry at index 0
        bool WalkMountVitalChain(uintptr_t owner, uintptr_t* outStatArray, uintptr_t* outTargetOwner = nullptr, uintptr_t* outActor = nullptr)
        {
            uint64_t actor = 0, marker = 0, root = 0, arr = 0;
            if (!Read64(owner + kOff_Owner_Actor, &actor) || actor < kMinPointer)
                actor = owner;
            if (!Read64(static_cast<uintptr_t>(actor) + kOff_Actor_StatusMarker, &marker) ||
                marker < kMinPointer) return false;
            if (!Read64(static_cast<uintptr_t>(marker) + kOff_Marker_TargetOwner, &root) ||
                root < kMinPointer) return false;
            if (!Read64(static_cast<uintptr_t>(root) + kOff_Root_StatArray, &arr) ||
                arr < kMinPointer) return false;
            if (outStatArray) *outStatArray = static_cast<uintptr_t>(arr);
            if (outTargetOwner) *outTargetOwner = static_cast<uintptr_t>(root);
            if (outActor) *outActor = static_cast<uintptr_t>(actor);
            return true;
        }

        // Recompute the player set's identities and stat entries from a fresh
        // resolve. Nothing is cached: the manager and its vector are re-read
        // fresh, so a body transition / character swap is picked up next tick.
        //
        // The set we want is every ACTIVE protagonist - Kliff, the character you
        // are currently playing, and any summoned companion. The class TAG alone
        // does not identify them: the game keeps a large pool of player-class
        // (SelfPlayer/OtherPlayer) character slots, but the body you actively
        // control is re-tagged when you play a secondary protagonist (observed
        // live: the played character carries tag 4 / Mercenary, not SelfPlayer,
        // while Kliff-as-companion keeps SelfPlayer). What they DO share is the
        // protagonist character-class vtable. So we (A) derive that vtable from
        // any player-class character, then (B) track every character of that
        // exact class whose vital chain resolves - which is precisely the active
        // protagonists (the pool's inactive slots have no stat array and every
        // NPC/enemy is a different class), tag-agnostic.

        // Zero every resolved set. Used both when no protagonist is resolvable
        // this tick and when the resolve is skipped entirely (see RefreshSelf) -
        // so a stale entry can never match after a transition, swap, or a
        // feature being toggled back on.
        void ClearPlayerSets()
        {
            for (int i = 0; i < kMaxPlayers; ++i)
            {
                g_hpEntries[i].store(0, std::memory_order_release);
                g_actors[i].store(0, std::memory_order_release);
                g_owners[i].store(0, std::memory_order_release);
                g_targetOwners[i].store(0, std::memory_order_release);
            }
            for (int i = 0; i < kMaxMounts; ++i)
            {
                g_mountActors[i].store(0, std::memory_order_release);
                g_mountTargetOwners[i].store(0, std::memory_order_release);
                g_mountOwners[i].store(0, std::memory_order_release);
            }
            g_mountCount.store(0, std::memory_order_release);
            for (int i = 0; i < kMaxStatEntries; ++i)
            {
                g_stamEntries[i].store(0, std::memory_order_release);
                g_mountStamEntries[i].store(0, std::memory_order_release);
                g_spiritEntries[i].store(0, std::memory_order_release);
            }
        }

        // The resolve exists solely to feed the stat pins (God Mode / Infinite
        // Stamina / Infinite Spirit) and the damage multipliers. When none of
        // those consume the sets, the whole-character-list walk it does every
        bool AnyStatFeatureActive(const State& st)
        {
            if (Teleport::IsProtected() || Teleport::GetFlightEngaged()) return true;
            return st.godMode || st.infStamina || st.infMountStamina || st.infSpirit ||
                   st.oneHitKill || st.noFallDamage ||
                   st.dmgInMult != 1.0f || st.dmgOutMult != 1.0f;
        }

        static ULONGLONG s_lastResolveMs = 0;

        // TU 2.01.00 no longer exposes the gameplay-character manager through
        // any of the pre-2.01 call-site anchors.  The inventory subsystem does,
        // however, resolve the currently controlled live character through a
        // separately validated client-realm root.  Use that owner as a narrow
        // fallback so the active player's stat features remain available while
        // deliberately leaving party-wide and mount discovery disabled.
        void TickResolveCurrentPlayerFallback()
        {
            ClearPlayerSets();
            g_playerPossessor.store(0, std::memory_order_release);

            const uintptr_t owner = Inventory::ClientCharacterAddr();
            SelfChain c{};
            if (owner < kMinPointer || !WalkSelfChain(owner, &c)) return;

            if (!g_currentFallbackLogged.exchange(true, std::memory_order_acq_rel))
                LOG_OK("player: current-character fallback resolved @ %p (active player only).",
                       reinterpret_cast<void*>(owner));

            g_hpEntries[0].store(c.statArray, std::memory_order_release);
            g_actors[0].store(c.actor, std::memory_order_release);
            g_targetOwners[0].store(c.targetOwner, std::memory_order_release);
            g_owners[0].store(owner, std::memory_order_release);

            uint64_t possessor = 0;
            if (Read64(owner + kOff_Owner_Possessor, &possessor) && possessor >= kMinPointer)
                g_playerPossessor.store(static_cast<uintptr_t>(possessor), std::memory_order_release);

            int nStam = 0, nSpir = 0;
            for (int k = 1; k < 20; ++k)
            {
                const uintptr_t e = c.statArray + k * kSizeof_StatEntry;
                int32_t statType = 0;
                if (!StatEntryType(e, &statType) || !PlausibleStatType(statType)) break;
                if (IsStaminaType(statType) && nStam < kMaxStatEntries)
                    g_stamEntries[nStam++].store(e, std::memory_order_release);
                else if (IsSpiritType(statType) && nSpir < kMaxStatEntries)
                    g_spiritEntries[nSpir++].store(e, std::memory_order_release);
            }

            const State& st = State::Get();
            if (st.godMode) PinEntry(c.statArray);
            if (st.infStamina)
                for (int i = 0; i < nStam; ++i)
                    PinEntry(g_stamEntries[i].load(std::memory_order_relaxed));
            if (st.infSpirit)
                for (int i = 0; i < nSpir; ++i)
                    PinEntry(g_spiritEntries[i].load(std::memory_order_relaxed));
        }

        // Collect one body's stamina / spirit gauges. Index 0 is Health and is
        // tracked separately, so the scan starts at 1 and stops at the first
        // entry that does not read as a plausible stat - the array is not
        // terminated, so the type check is what bounds it.
        void ScanGauges(uintptr_t statArray, int& nStam, int& nSpir)
        {
            for (int k = 1; k < 20; ++k)
            {
                const uintptr_t e = statArray + k * kSizeof_StatEntry;
                int32_t stt = 0;
                if (!StatEntryType(e, &stt)) break;
                if (!PlausibleStatType(stt)) break;
                if (IsStaminaType(stt))
                {
                    if (nStam < kMaxStatEntries)
                        g_stamEntries[nStam++].store(e, std::memory_order_release);
                }
                else if (IsSpiritType(stt))
                {
                    if (nSpir < kMaxStatEntries)
                        g_spiritEntries[nSpir++].store(e, std::memory_order_release);
                }
            }
        }

        void TickResolveSelf()
        {
            uint64_t p = 0, mgr = 0, data = 0;
            uint32_t count = 0;
            if (!g_charMgrGlobal || !Read64(g_charMgrGlobal,&p) || p < kMinPointer ||
                !Read64(static_cast<uintptr_t>(p),&mgr) || mgr < kMinPointer ||
                !Read64(static_cast<uintptr_t>(mgr)+kOff_CharMgr_ListData,&data) || data < kMinPointer ||
                !Read32(static_cast<uintptr_t>(mgr)+kOff_CharMgr_ListCount,&count) ||
                count == 0 || count > kCharList_MaxCount) {
                ClearPlayerSets();
                g_playerPossessor.store(0,std::memory_order_release);
                return;
            }

            // (A) The protagonist class vtable = the vtable of any player-class
            // character (the SelfPlayer/OtherPlayer pool all share it).
            uint64_t anchorVt = 0;
            uint64_t playerPoss = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint64_t ch = 0;
                if (!Read64(static_cast<uintptr_t>(data) + 8ull * i, &ch) || ch < kMinPointer) continue;
                if (!IsPlayerClass(static_cast<uintptr_t>(ch))) continue;
                if (Read64(static_cast<uintptr_t>(ch), &anchorVt) && anchorVt >= kMinPointer)
                {
                    Read64(static_cast<uintptr_t>(ch) + kOff_Owner_Possessor, &playerPoss);
                    break;
                }
                anchorVt = 0;
            }
            if (!anchorVt)
            {
                ClearPlayerSets();
                return;
            }
            g_playerPossessor.store(static_cast<uintptr_t>(playerPoss), std::memory_order_release);

            int nPlayers = 0, nStam = 0, nMountStam = 0, nSpir = 0, nMounts = 0;

            // (A2) Seed the set with the body actually being driven.
            //
            // The vtable gate in (B) anchors on the first player-class character
            // in the list, which is Kliff whenever he is present - he keeps the
            // SelfPlayer tag even while a secondary protagonist is the one under
            // your control. A protagonist whose class vtable differs from his is
            // then driven but never enumerated, and every consumer of these sets
            // - God Mode, the damage multipliers, the stat pins - goes quietly
            // inert for that character while transform-only features keep working.
            //
            // The possessor's active pawn is authoritative about who is being
            // driven, so take it first and let (B) fill the remaining slots. It
            // has to be seeded rather than appended because the loop stops at
            // kMaxPartyPlayers: appending would let two companions fill the set
            // and leave out the character you are actually playing.
            //
            // Riding needs no special case - while mounted the active pawn is the
            // mount, and WalkSelfChain's "index 0 must be a Health entry" check
            // rejects it, which is why mounts have their own vital walker.
            uintptr_t drivenOwner = 0;
            if (playerPoss >= kMinPointer)
            {
                uint64_t pawn = 0;
                if (Read64(static_cast<uintptr_t>(playerPoss) + kOff_Possessor_Pawn, &pawn) &&
                    pawn >= kMinPointer)
                {
                    SelfChain dc;
                    if (WalkSelfChain(static_cast<uintptr_t>(pawn), &dc))
                    {
                        drivenOwner = static_cast<uintptr_t>(pawn);
                        g_hpEntries[0].store(dc.statArray, std::memory_order_release);
                        g_actors[0].store(dc.actor, std::memory_order_release);
                        g_targetOwners[0].store(dc.targetOwner, std::memory_order_release);
                        g_owners[0].store(drivenOwner, std::memory_order_release);
                        nPlayers = 1;
                        ScanGauges(dc.statArray, nStam, nSpir);
                    }
                }
            }

            // (B) Track active protagonist bodies with matching vtable
            for (uint32_t i = 0; i < count && nPlayers < kMaxPartyPlayers; ++i)
            {
                uint64_t ch = 0;
                if (!Read64(static_cast<uintptr_t>(data) + 8ull * i, &ch) || ch < kMinPointer) continue;
                const uintptr_t owner = static_cast<uintptr_t>(ch);
                if (owner == drivenOwner) continue; // already seeded above
                uint64_t vt = 0;
                if (!Read64(owner, &vt) || vt != anchorVt) continue;

                uint64_t typeDesc = 0;
                uint8_t typeTag = 0;
                if (!Read64(owner + kOff_Owner_TypeDesc, &typeDesc) || typeDesc < kMinPointer ||
                    !Read8(static_cast<uintptr_t>(typeDesc) + 1, &typeTag) ||
                    !IsTrackedProtagonistTypeTag(typeTag))
                    continue;

                SelfChain c;
                if (!WalkSelfChain(owner, &c)) continue;

                g_hpEntries[nPlayers].store(c.statArray, std::memory_order_release);
                g_actors[nPlayers].store(c.actor, std::memory_order_release);
                g_targetOwners[nPlayers].store(c.targetOwner, std::memory_order_release);
                g_owners[nPlayers].store(owner, std::memory_order_release);
                ++nPlayers;

                // Scan stat array for stamina (17, 19, 20, 22) and spirit (18, 21, 23)
                ScanGauges(c.statArray, nStam, nSpir);
            }

            // Mount stamina discovery. TU 2.01 folds state bits into owner+0x48
            // (live values are 0x10001/0x10002/...); the stable engine type is
            // the descriptor tag at *(owner+0x88)+1. Prefer Vehicle (5) over
            // Pet (6), so "Active Mount" resolves the ridden horse before a
            // summoned pet that happens to appear earlier in the manager.
            for (uint8_t wantedTag : { static_cast<uint8_t>(Obj_Vehicle),
                                       static_cast<uint8_t>(Obj_Pet) })
            {
                for (uint32_t i = 0; i < count && nMountStam < kMaxMountStamEntries; ++i)
                {
                    uint64_t ch = 0;
                    if (!Read64(static_cast<uintptr_t>(data) + 8ull * i, &ch) || ch < kMinPointer) continue;
                    const uintptr_t owner = static_cast<uintptr_t>(ch);

                    uint64_t typeDesc = 0;
                    uint8_t typeTag = 0;
                    if (!Read64(owner + kOff_Owner_TypeDesc, &typeDesc) || typeDesc < kMinPointer ||
                        !Read8(static_cast<uintptr_t>(typeDesc) + 1, &typeTag) ||
                        typeTag != wantedTag || !IsMountTypeTag(typeTag))
                        continue;

                    uintptr_t mountStatArray = 0, mountTarget = 0, mountAct = 0;
                    if (WalkMountVitalChain(owner, &mountStatArray, &mountTarget, &mountAct) && mountStatArray >= kMinPointer)
                    {
                        if (nMounts < kMaxMounts)
                        {
                            g_mountActors[nMounts].store(mountAct ? mountAct : owner, std::memory_order_release);
                            g_mountTargetOwners[nMounts].store(mountTarget, std::memory_order_release);
                            g_mountOwners[nMounts].store(owner, std::memory_order_release);
                            ++nMounts;
                        }
                        for (int k = 0; k < 20; ++k)
                        {
                            const uintptr_t e = mountStatArray + k * kSizeof_StatEntry;
                            int32_t stt = 0;
                            if (!StatEntryType(e, &stt)) break;
                            if (!PlausibleStatType(stt)) break;
                            if (IsStaminaType(stt) && nMountStam < kMaxMountStamEntries)
                                g_mountStamEntries[nMountStam++].store(e, std::memory_order_release);
                        }
                    }
                }
            }
            g_mountCount.store(nMounts, std::memory_order_release);

            // Clear trailing slots
            for (int i = nPlayers; i < kMaxPlayers; ++i)
            {
                g_hpEntries[i].store(0, std::memory_order_release);
                g_actors[i].store(0, std::memory_order_release);
                g_owners[i].store(0, std::memory_order_release);
                g_targetOwners[i].store(0, std::memory_order_release);
            }
            for (int i = nStam; i < kMaxStatEntries; ++i)
                g_stamEntries[i].store(0, std::memory_order_release);
            for (int i = nMountStam; i < kMaxMountStamEntries; ++i)
                g_mountStamEntries[i].store(0, std::memory_order_release);
            for (int i = nSpir; i < kMaxStatEntries; ++i)
                g_spiritEntries[i].store(0, std::memory_order_release);
            for (int i = nMounts; i < kMaxMounts; ++i)
            {
                g_mountActors[i].store(0, std::memory_order_release);
                g_mountTargetOwners[i].store(0, std::memory_order_release);
                g_mountOwners[i].store(0, std::memory_order_release);
            }

            // Immediately pin stats each frame when active
            const State& st = State::Get();
            if (st.godMode)
            {
                for (int i = 0; i < nPlayers; ++i)
                {
                    const uintptr_t e = g_hpEntries[i].load(std::memory_order_relaxed);
                    if (e >= kMinPointer) PinEntry(e);
                }
            }
            if (st.infStamina)
            {
                for (int i = 0; i < nStam; ++i)
                {
                    const uintptr_t e = g_stamEntries[i].load(std::memory_order_relaxed);
                    if (e >= kMinPointer) PinEntry(e);
                }
            }
            if (st.infMountStamina)
            {
                for (int i = 0; i < nMountStam; ++i)
                {
                    const uintptr_t e = g_mountStamEntries[i].load(std::memory_order_relaxed);
                    if (e >= kMinPointer) PinEntry(e);
                }
            }
            if (st.infSpirit)
            {
                for (int i = 0; i < nSpir; ++i)
                {
                    const uintptr_t e = g_spiritEntries[i].load(std::memory_order_relaxed);
                    if (e >= kMinPointer) PinEntry(e);
                }
            }
        }

        // --- God Mode / Infinite Stamina / Infinite Mount Stamina / Infinite Spirit: guard the stat-
        // commit write ------------------------------------------------------
        int64_t __fastcall hkStatCommit(void* entry, int64_t time, int64_t target, uint16_t flag)
        {
            const uintptr_t e = reinterpret_cast<uintptr_t>(entry);
            const State& st = State::Get();

            const bool isPlayerHp   = InSet(g_hpEntries, kMaxPlayers, e);
            const bool isStam       = InSet(g_stamEntries, kMaxStatEntries, e);
            const bool isMountStam  = InSet(g_mountStamEntries, kMaxMountStamEntries, e);
            const bool isSpirit     = InSet(g_spiritEntries, kMaxStatEntries, e);

            const bool shouldLock = (st.godMode && isPlayerHp) ||
                                    ((st.infStamina && isStam) || (st.infMountStamina && isMountStam)) ||
                                    (st.infSpirit && isSpirit);

            if (shouldLock)
            {
                uint64_t base = 0, cap = 0, cur = 0;
                Read64(e + kOff_StatEntry_Base, &base);
                Read64(e + kOff_StatEntry_Cap, &cap);
                Read64(e + kOff_StatEntry_Current, &cur);
                uint64_t full = (cap > base) ? cap : base;
                if (!full && cur > 0 && cur < 1000000000ULL) full = cur;
                if (full > 0)
                {
                    PinEntry(e);
                    target = static_cast<int64_t>(full);
                }
            }

            const int64_t result = oStatCommit(entry, time, target, flag);

            if (shouldLock)
                PinEntry(e);

            return result; // PE2850 returns flags, not the requested stat value.
        }

        bool IsPlayerEntity(uintptr_t target)
        {
            if (target < kMinPointer) return false;
            if (InSet(g_targetOwners, kMaxPlayers, target)) return true;
            if (InSet(g_actors, kMaxPlayers, target)) return true;
            if (InSet(g_hpEntries, kMaxPlayers, target)) return true;
            if (InSet(g_owners, kMaxPlayers, target)) return true;
            for (int i = 0; i < kMaxPartyPlayers; ++i)
            {
                const uintptr_t act = g_actors[i].load(std::memory_order_relaxed);
                if (act && act == target) return true;
                const uintptr_t own = g_owners[i].load(std::memory_order_relaxed);
                if (own && own == target) return true;
                const uintptr_t tgt = g_targetOwners[i].load(std::memory_order_relaxed);
                if (tgt && tgt == target) return true;
            }
            return false;
        }

        // targetOwner is the victim's strict battle-vital identity. Do not
        // classify it through broad owner/actor aliases: those aliases are
        // needed for attacker discovery and can overlap during companion
        // swaps, which made enemies intermittently look like players.
        bool IsStrictPlayerTarget(uintptr_t target)
        {
            if (target < kMinPointer) return false;
            return InSet(g_targetOwners, kMaxPlayers, target);
        }

        bool IsMountEntity(uintptr_t target)
        {
            if (target < kMinPointer) return false;
            if (InSet(g_mountTargetOwners, kMaxMounts, target)) return true;
            if (InSet(g_mountActors, kMaxMounts, target)) return true;
            if (InSet(g_mountOwners, kMaxMounts, target)) return true;
            if (InSet(g_mountStamEntries, kMaxStatEntries, target)) return true;
            return false;
        }

        // --- Damage multipliers: scale the hit at the apply dispatcher -----
        int64_t ScaleDamage(uintptr_t targetOwner, uintptr_t sourceCtx, int64_t delta)
        {
            const State& st = State::Get();

            // Victim is Player (Incoming Hit)
            if (IsStrictPlayerTarget(targetOwner))
            {
                if (st.godMode) return 0;
                if (st.dmgInMult != 1.0f)
                {
                    const double scaled = static_cast<double>(delta) * static_cast<double>(st.dmgInMult);
                    if (scaled <= static_cast<double>(INT64_MIN)) return INT64_MIN;
                    if (scaled >= 0.0) return 0;
                    return static_cast<int64_t>(scaled);
                }
                return delta;
            }

            // Victim is Non-Player / Enemy (Outgoing Hit dealt by Player)
            bool isPlayerAttacker = false;
            if (sourceCtx >= kMinPointer)
            {
                if (IsPlayerEntity(sourceCtx) || IsMountEntity(sourceCtx))
                {
                    isPlayerAttacker = true;
                }
                else
                {
                    uint64_t actor = 0;
                    if (Read64(sourceCtx + kOff_Owner_Actor, &actor) && actor >= kMinPointer)
                    {
                        if (IsPlayerEntity(static_cast<uintptr_t>(actor)) || IsMountEntity(static_cast<uintptr_t>(actor)))
                            isPlayerAttacker = true;
                    }
                    
                    if (!isPlayerAttacker)
                    {
                        uint64_t marker = 0;
                        if (Read64(sourceCtx + kOff_Actor_StatusMarker, &marker) && marker >= kMinPointer)
                        {
                            if (IsPlayerEntity(static_cast<uintptr_t>(marker)))
                                isPlayerAttacker = true;
                        }
                    }

                    if (!isPlayerAttacker)
                    {
                        uint64_t poss = 0;
                        if (Read64(sourceCtx + kOff_Owner_Possessor, &poss) && poss >= kMinPointer)
                        {
                            const uintptr_t pposs = g_playerPossessor.load(std::memory_order_relaxed);
                            if (pposs && poss == pposs)
                                isPlayerAttacker = true;
                        }
                    }
                }
            }

            if (isPlayerAttacker)
            {
                float mult = st.oneHitKill ? 10000.0f : st.dmgOutMult;
                if (mult != 1.0f)
                {
                    const double scaled = static_cast<double>(delta) * static_cast<double>(mult);
                    if (scaled <= static_cast<double>(INT64_MIN)) return INT64_MIN;
                    if (scaled >= 0.0) return 0;
                    return static_cast<int64_t>(scaled);
                }
            }

            return delta;
        }

        int64_t __fastcall hkDamageApply(void* targetOwner, uint16_t statusId,
                                         int64_t time, int64_t delta, uintptr_t sourceCtx,
                                         char a6, char a7, char a8, char a9, char a10,
                                         void* out)
        {
            const State& st = State::Get();
            const uintptr_t owner = reinterpret_cast<uintptr_t>(targetOwner);
            const bool isPlayerTarget = IsStrictPlayerTarget(owner);
            const bool isMountTarget  = IsMountEntity(owner);

            // Determine if damage source is an active hostile enemy vs environmental/fall impact
            bool isEnemyAttacker = false;
            if (sourceCtx >= kMinPointer && !IsPlayerEntity(sourceCtx))
            {
                uint64_t sourceActor = 0;
                if (Read64(sourceCtx + kOff_Owner_Actor, &sourceActor) && sourceActor >= kMinPointer)
                {
                    if (!IsPlayerEntity(static_cast<uintptr_t>(sourceActor)))
                        isEnemyAttacker = true;
                }
                else
                {
                    uintptr_t sub = 0;
                    if (ReadPtr(sourceCtx + kOff_Container_Sub, &sub) && sub >= kMinPointer)
                        isEnemyAttacker = true;
                }
            }

            if (delta < 0)
            {
                if (statusId == StatType_Health || statusId == 0)
                {
                    if (ShouldBlockPlayerDamage(st.godMode, isPlayerTarget, isMountTarget))
                    {
                        delta = 0; // complete damage immunity for player & mount
                    }
                    else if ((st.noFallDamage || Teleport::IsProtected() || Teleport::GetFlightEngaged()) &&
                             isPlayerTarget && !isEnemyAttacker)
                    {
                        // 100% Nullify ALL fall damage, cliff drops, gravity impacts & landing shock
                        delta = 0;
                    }
                    else
                    {
                        delta = ScaleDamage(owner, sourceCtx, delta);
                    }
                }
                else if (IsStaminaType(statusId))
                {
                    if (isPlayerTarget && (st.infStamina || Teleport::GetFlightEngaged()))
                    {
                        delta = 0; // zero-out player stamina drain
                    }
                    else if (isMountTarget && (st.infMountStamina || Teleport::GetFlightEngaged()))
                    {
                        delta = 0; // zero-out mount stamina drain
                    }
                }
                else if (isPlayerTarget && IsSpiritType(statusId) && st.infSpirit)
                {
                    delta = 0; // zero-out spirit drain
                }
            }

            return oDamageApply(targetOwner, statusId, time, delta, sourceCtx,
                                a6, a7, a8, a9, a10, out);
        }

        // --- Combat Timing & Hitbox Evaluator: Perfect Parry & Perfect Dodge (sub_1407219c0) ---
        using CombatTimingEval_t = bool(__fastcall*)(void* combatComp, void* hitData, float distance, uint8_t isGuardMode, void* outResult);
        CombatTimingEval_t oCombatTimingEval = nullptr;
        void* g_combatTimingTarget = nullptr;

        bool __fastcall hkCombatTimingEval(void* combatComp, void* hitData, float distance, uint8_t isGuardMode, void* outResult)
        {
            const bool orig = oCombatTimingEval ? oCombatTimingEval(combatComp, hitData, distance, isGuardMode, outResult) : false;
            const State& st = State::Get();

            return orig;
        }

    }

    bool Player::Install()
    {
        g_charMgrGlobal = ResolveCharMgrGlobal();
        if (!g_charMgrGlobal) {
            // Anchors are stale for this build. Installing nothing at all leaves
            // no way back short of a new release, so start the structural probe
            // and carry on: every hook below consults the tracked sets at
            // runtime, and those stay empty - so the hooks pass through
            // unchanged - until the probe fills the global in.
            LOG_WARN("player: no char-manager anchor matched - starting the structural probe "
                     "(retries every 5s until the world is loaded). Stat features stay inert "
                     "until it succeeds.");
            if (HANDLE h = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr))
                CloseHandle(h);
        }
        // TU 2.01 removed the old single stat-commit funnel.  The resolved
        // character manager plus the per-frame entry pins are the current
        // guard on this build; do not search/hook a stale ABI.
        if (core::UsesTu201CompatibleRevision(core::GetGameVersion().revision))
        {
            constexpr const char* modernCommit =
                "66 44 89 4C 24 20 48 89 54 24 10 53 55 56 57 41 56 48 83 EC 40 4C 8D 71 18 48 89 CF 48 8B 49 20 4C 89 C3 49 03 0E";
            mem::InstallHook("player: PE2850 stat guard", modernCommit,
                             "synchronous health/stamina protection unavailable",
                             &hkStatCommit, &oStatCommit, &g_commitTarget);
        }
        else
        {
            mem::InstallHook("player: stat-commit", kSig_StatCommit,
                             "direct write guard unavailable; current-character pins remain active",
                             &hkStatCommit, &oStatCommit, &g_commitTarget);
        }

        // DamageApply: try primary signature first, then Alt (TU 2.00 recompile shifted the prologue).
        if (!mem::InstallHook("player: damage-apply", kSig_DamageApply, "",
                              &hkDamageApply, &oDamageApply, &g_damageHookTarget))
        {
            if (mem::InstallHook("player: damage-apply (alt)", kSig_DamageApply_Alt, "damage multipliers disabled",
                                  &hkDamageApply, &oDamageApply, &g_damageHookTarget))
            {
                LOG_OK("player: damage-apply hook installed @ %p", g_damageHookTarget);
            }
            else
            {
                LOG_ERR("player: damage-apply signature NOT FOUND (tried primary + alt) - infinite stamina drain block disabled.");
            }
        }
        else
        {
            LOG_OK("player: damage-apply hook installed @ %p", g_damageHookTarget);
        }

        if (!g_damageHookTarget || !g_commitTarget) {
            mem::RemoveHook(&g_damageHookTarget);
            mem::RemoveHook(&g_commitTarget);
            return false;
        }
        return true;
    }

    void Player::Tick()
    {
        const State& st = State::Get();
        const bool statFeatureActive = AnyStatFeatureActive(st);
        const uint64_t now = GetTickCount64();
        const uint64_t requestedUntil =
            g_characterTrackingRequestedUntilMs.load(std::memory_order_acquire);
        if (!ShouldRefreshTrackedCharacters(statFeatureActive, now, requestedUntil))
        {
            ClearPlayerSets();
            g_playerPossessor.store(0,std::memory_order_release);
            return;
        }

        TickResolveSelf();
        if (st.infStamina || st.infMountStamina)
        {
            for (int i = 0; i < kMaxStatEntries; ++i)
            {
                if (st.infStamina) PinEntry(g_stamEntries[i].load(std::memory_order_relaxed));
                if (st.infMountStamina) PinEntry(g_mountStamEntries[i].load(std::memory_order_relaxed));
            }
        }
        if (st.infSpirit)
        {
            for (int i = 0; i < kMaxStatEntries; ++i)
                PinEntry(g_spiritEntries[i].load(std::memory_order_relaxed));
        }
    }

    void Player::RefreshSelf()
    {
        // Lightweight non-blocking pin on already-resolved atomic pointers without scanning char manager
        const State& st = State::Get();
        if (st.infStamina || st.infMountStamina)
        {
            for (int i = 0; i < kMaxStatEntries; ++i)
            {
                if (st.infStamina) PinEntry(g_stamEntries[i].load(std::memory_order_relaxed));
                if (st.infMountStamina) PinEntry(g_mountStamEntries[i].load(std::memory_order_relaxed));
            }
        }
        if (st.infSpirit)
        {
            for (int i = 0; i < kMaxStatEntries; ++i)
                PinEntry(g_spiritEntries[i].load(std::memory_order_relaxed));
        }
    }

    void Player::Remove()
    {
        mem::RemoveHook(&g_commitTarget);
        mem::RemoveHook(&g_damageHookTarget);
        mem::RemoveHook(&g_combatTimingTarget);
        for (int i = 0; i < kMaxPlayers; ++i)
        {
            g_hpEntries[i].store(0);
            g_actors[i].store(0);
            g_owners[i].store(0);
            g_targetOwners[i].store(0);
        }
        for (int i = 0; i < kMaxMounts; ++i)
        {
            g_mountActors[i].store(0);
            g_mountTargetOwners[i].store(0);
            g_mountOwners[i].store(0);
        }
        g_mountCount.store(0);
        g_currentFallbackLogged.store(false, std::memory_order_release);
        for (int i = 0; i < kMaxStatEntries; ++i)
        {
            g_stamEntries[i].store(0);
            g_mountStamEntries[i].store(0);
            g_spiritEntries[i].store(0);
        }
    }

    bool Player::Ready()
    {
        return g_hpEntries[0].load(std::memory_order_relaxed) >= kMinPointer &&
               g_actors[0].load(std::memory_order_relaxed) >= kMinPointer;
    }

    uintptr_t Player::GetActor(int index)
    {
        if (index < 0 || index >= kMaxPlayers) return 0;
        return g_actors[index].load(std::memory_order_acquire);
    }

    uintptr_t Player::GetOwner(int index)
    {
        if (index < 0 || index >= kMaxPlayers) return 0;
        return g_owners[index].load(std::memory_order_acquire);
    }

    int Player::GetTrackedPlayerCount()
    {
        const uint64_t now = GetTickCount64();
        g_characterTrackingRequestedUntilMs.store(
            now + kCharacterTrackingDemandMs, std::memory_order_release);

        int count = 0;
        for (int i = 0; i < kMaxPlayers; ++i)
        {
            if (g_actors[i].load(std::memory_order_acquire) >= kMinPointer)
                ++count;
        }
        return count;
    }

    uintptr_t Player::GetMountActor(int index)
    {
        if (index < 0 || index >= kMaxMounts) return 0;
        return g_mountActors[index].load(std::memory_order_acquire);
    }

    uintptr_t Player::GetMountOwner(int index)
    {
        if (index < 0 || index >= kMaxMounts) return 0;
        return g_mountOwners[index].load(std::memory_order_acquire);
    }

    int Player::GetTrackedMountCount()
    {
        return g_mountCount.load(std::memory_order_acquire);
    }
}
