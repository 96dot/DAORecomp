// Cheats: the developers' debug scripts, run through the game's script engine. The
// 360 build keeps its designer scripts compiled into the executable (a table of
// 1,994, filled by sub_822E0978), the debug ones among them: those the PC
// version's console ran with "runscript <name> <value>". RunScript (sub_8260C940)
// runs one by name as the engine does for an event's script, with an object as
// its OBJECT_SELF (the player's character, as the console ran them, or a party
// member); the value goes where the console put it and the scripts read it: the
// module's RUNSCRIPT_VAR variable, set before the script runs and emptied after.
//
// The menu opens over the paused game in exploration or combat (as photo mode,
// from the mode controller's update), and its scripts run there, on the game's
// thread, between frames.
#include "stdafx.h"
#include "ui/cheats.h"
#include "kernel/game_build.h"
#include "kernel/game_modes.h"
#include "kernel/memory.h"
#include "kernel/thread.h"

namespace
{
    using namespace dao::kernel;

    // The script engine (a pointer). RunScript(engine, name, object, objectValid,
    // event) runs the script `name` (a wide string, which stays the caller's) with
    // `object` (an object's id) as its OBJECT_SELF; event -1: none (as
    // sub_8246D4C0 runs one).
    constexpr GameAddress kScriptEngine{ 0x83171F6C, 0x8324246C };
    constexpr GameAddress kRunScript{ 0x8260C940, 0x8263FFD8 };
    // The scripting commands, as the scripts call them (an object is its id, -1
    // for none; strings are passed by value: the command destroys them).
    // GetModule() and GetHero() (the player's character).
    constexpr GameAddress kGetModule{ 0x82473E88, 0x824B3AE8 };
    constexpr GameAddress kGetHero{ 0x8247B5A8, 0x824BB4B0 };
    // SetLocalString(object, name, value) and GetLocalInt(object, name).
    constexpr GameAddress kSetLocalString{ 0x82470010, 0x824AFCE8 };
    constexpr GameAddress kGetLocalInt{ 0x8246F9C0, 0x824AF698 };
    // IsImmortal(creature), and GetTag(result, object) (a wide string).
    constexpr GameAddress kIsImmortal{ 0x82473CB8, 0x824B3918 };
    constexpr GameAddress kGetTag{ 0x82472F88, 0x824B2C08 };
    // GetPartyList(result, -1): an array of the party's creatures (kArraySize
    // bytes; its first element's address at +8, past its last at +12);
    // DestroyArray(array).
    constexpr GameAddress kGetPartyList{ 0x82480FA0, 0x824C0EA8 };
    constexpr GameAddress kDestroyArray{ 0x8233B658, 0x8236CE70 };
    constexpr uint32_t kArraySize = 20;
    constexpr uint32_t kArrayBeginOffset = 8, kArrayEndOffset = 12;
    constexpr uint32_t kMaxParty = 8;
    // The game's wide strings (std::wstring, 28 bytes: the characters, or a
    // pointer to them once its capacity is 8 or more, at +4, the length at +20,
    // the capacity at +24). Construct(string, text) makes one of a null-terminated
    // text; Destroy(string, 1, 0) frees it.
    constexpr GameAddress kWideConstruct{ 0x825E9FF8, 0x8261CB00 };
    constexpr GameAddress kWideDestroy{ 0x82232B48, 0x82253270 };
    constexpr uint32_t kWideSize = 28;
    constexpr uint32_t kWideTextOffset = 4, kWideLengthOffset = 20, kWideCapacityOffset = 24;
    constexpr uint32_t kWideInlineCapacity = 8;

    // The game's paused byte; SetPaused(game, paused, 0), as photo mode pauses it.
    constexpr uint32_t kPausedOffset = 232;
    constexpr GameAddress kSetPaused{ 0x822659D0, 0x82285470 };

    // The variable the debug scripts read their value from (the module's).
    constexpr std::string_view kValueVariable = "RUNSCRIPT_VAR";
    // The module's variable that decides every skill check (sub_82980C98, which
    // gen00pt_skills asks), if not 0: kSkillCheckPass passes them, others fail
    // them. zz_override_skills sets it.
    constexpr std::string_view kSkillCheckVariable = "DEBUG_SKILL_CHECK_OVERRIDE";
    constexpr int32_t kSkillCheckPass = 1;

    // Guest memory for the calls' arguments, at these offsets in it: the
    // script's name, the variable's name and value (wide strings), the text
    // they're made of (up to kMaxText characters and a null), and the party's
    // array.
    constexpr uint32_t kScratchName = 0x00, kScratchVariable = 0x20, kScratchValue = 0x40;
    constexpr uint32_t kScratchText = 0x60, kScratchArray = 0xC0, kScratchSize = 0xE0;
    constexpr size_t kMaxText = 40;
    static_assert(kWideSize <= kScratchVariable - kScratchName && kWideSize <= kScratchValue - kScratchVariable &&
        kWideSize <= kScratchText - kScratchValue);
    static_assert((kMaxText + 1) * 2 <= kScratchArray - kScratchText);
    static_assert(kArraySize <= kScratchSize - kScratchArray);

    // How long an open asked for waits for the game to be where the menu opens
    // (the Graphics tab's button closes the menus, then asks).
    constexpr auto kMenuWait = std::chrono::seconds(1);

    // The buttons the game doesn't see while the menu is open: all (its triggers and
    // sticks neither, kernel/xam.cpp), the paused game's radial menus and camera
    // otherwise answering them.
    constexpr uint16_t kMenuButtons = 0xFFFF;

    struct Request
    {
        std::string name;
        std::string value;
        dao::ui::cheats::Scope scope;
    };
    std::mutex s_requestsMutex;
    std::vector<Request> s_requests; // while the menu is open

    // Asked for (any thread): the time, or 0.
    std::atomic<int64_t> s_toggleAt{ 0 };
    std::atomic<bool> s_open{ false };
    std::atomic<uint16_t> s_heldOver{ 0 };
    // The last game mode outside the menus (-1 before the first).
    std::atomic<int32_t> s_worldMode{ -1 };
    // The game's state the menu shows (set on the game thread).
    std::atomic<bool> s_immortal{ false };
    std::atomic<bool> s_skillChecks{ false };

    // Game thread only.
    bool s_pausedHere = false;
    uint32_t s_scratch = 0; // kScratchSize bytes, allocated the first time
    // The creatures the cheats made immortal (EachMortal), and the player's
    // character then (another after a load: the ids are the game's no more).
    std::vector<uint32_t> s_madeImmortal;
    uint32_t s_madeImmortalHero = 0;

    // Never 0 (which is no toggle asked for).
    int64_t Now()
    {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() | 1;
    }

    void SetPaused(uint32_t game, bool paused)
    {
        CallGuest(kSetPaused.Get(), { game, paused ? 1u : 0u, 0 });
    }

    // The guest scratch memory at `offset` (kScratch*).
    uint32_t Scratch(uint32_t offset)
    {
        if (!s_scratch)
            s_scratch = KernelAlloc(kScratchSize);
        return s_scratch + offset;
    }

    // A wide string of `text` (ASCII, up to kMaxText characters) made at `string`.
    void ConstructWide(uint8_t* base, uint32_t string, std::string_view text)
    {
        const uint32_t chars = Scratch(kScratchText);
        for (size_t i = 0; i < text.size(); ++i)
            PPC_STORE_U16(chars + uint32_t(i) * 2, uint16_t(uint8_t(text[i])));
        PPC_STORE_U16(chars + uint32_t(text.size()) * 2, 0);
        CallGuest(kWideConstruct.Get(), { string, chars });
    }

    // A wide string's text (up to kMaxText characters, and no more than it has
    // room for; printable ASCII, others as '?').
    std::string ReadWide(uint8_t* base, uint32_t string)
    {
        const uint32_t capacity = PPC_LOAD_U32(string + kWideCapacityOffset);
        const bool inline_ = capacity < kWideInlineCapacity;
        const uint32_t room = inline_ ? kWideInlineCapacity - 1 : capacity;
        const uint32_t length = std::min({ PPC_LOAD_U32(string + kWideLengthOffset), room, uint32_t(kMaxText) });
        const uint32_t chars = inline_ ? string + kWideTextOffset : PPC_LOAD_U32(string + kWideTextOffset);
        std::string text;
        for (uint32_t i = 0; i < length; ++i)
        {
            const uint16_t c = PPC_LOAD_U16(chars + i * 2);
            text += c >= 0x20 && c < 0x7F ? char(c) : '?';
        }
        return text;
    }

    // The module's RUNSCRIPT_VAR.
    void SetValue(uint8_t* base, uint32_t module, std::string_view value)
    {
        const uint32_t name = Scratch(kScratchVariable), text = Scratch(kScratchValue);
        ConstructWide(base, name, kValueVariable);
        ConstructWide(base, text, value);
        CallGuest(kSetLocalString.Get(), { module, name, text });
    }

    void RunScript(uint8_t* base, std::string_view name, uint32_t self)
    {
        const uint32_t string = Scratch(kScratchName);
        ConstructWide(base, string, name);
        CallGuest(kRunScript.Get(), { PPC_LOAD_U32(kScriptEngine.Get()), string, self, 1, UINT32_MAX });
        CallGuest(kWideDestroy.Get(), { string, 1, 0 });
    }

    bool IsImmortal(uint32_t creature)
    {
        return uint32_t(CallGuest(kIsImmortal.Get(), { creature })) != 0;
    }

    std::string GetTag(uint8_t* base, uint32_t object)
    {
        const uint32_t string = Scratch(kScratchName);
        CallGuest(kGetTag.Get(), { string, object });
        std::string tag = ReadWide(base, string);
        CallGuest(kWideDestroy.Get(), { string, 1, 0 });
        return tag;
    }

    // The party's creatures (the player's character among them).
    std::vector<uint32_t> PartyMembers(uint8_t* base)
    {
        const uint32_t array = Scratch(kScratchArray);
        CallGuest(kGetPartyList.Get(), { array, UINT32_MAX });
        std::vector<uint32_t> members;
        const uint32_t begin = PPC_LOAD_U32(array + kArrayBeginOffset), end = PPC_LOAD_U32(array + kArrayEndOffset);
        if (begin % 4 == 0 && end >= begin && (end - begin) % 4 == 0)
        {
            for (uint32_t at = begin; at < end && members.size() < kMaxParty; at += 4)
                members.push_back(PPC_LOAD_U32(at));
        }
        else
            LOGW("Cheats: the party's list isn't one ({:08X}-{:08X})", begin, end);
        CallGuest(kDestroyArray.Get(), { array });
        return members;
    }

    void RunRequest(uint8_t* base, const Request& request)
    {
        using dao::ui::cheats::Scope;
        const uint32_t module = uint32_t(CallGuest(kGetModule.Get()));
        const uint32_t hero = uint32_t(CallGuest(kGetHero.Get()));
        if (!PPC_LOAD_U32(kScriptEngine.Get()) || module == UINT32_MAX || hero == UINT32_MAX)
        {
            LOGW("Cheats: no game to run {} in", request.name);
            return;
        }
        // Run as `self` with `value`.
        const auto run = [&](uint32_t self, std::string_view value) {
            LOGI("Cheats: running {}{}{}", request.name, value.empty() ? "" : " ", value);
            SetValue(base, module, value);
            RunScript(base, request.name, self);
            SetValue(base, module, {});
        };
        if (request.scope == Scope::Hero)
        {
            run(hero, request.value);
            return;
        }
        if (s_madeImmortalHero != hero)
        {
            s_madeImmortal.clear();
            s_madeImmortalHero = hero;
        }
        const std::vector<uint32_t> members = PartyMembers(base);
        // MadeImmortal: those the cheats made immortal; if none (the cheat was used
        // in an earlier session), all who are.
        const bool anyMade = std::ranges::any_of(members, [](uint32_t member) {
            return std::ranges::find(s_madeImmortal, member) != s_madeImmortal.end();
        });
        for (const uint32_t member : members)
        {
            switch (request.scope)
            {
            case Scope::EachTag:
                if (const std::string tag = GetTag(base, member); !tag.empty())
                    run(hero, tag);
                break;
            case Scope::EachMortal:
                if (!IsImmortal(member))
                {
                    run(member, request.value);
                    s_madeImmortal.push_back(member);
                }
                break;
            case Scope::MadeImmortal:
                if (IsImmortal(member) &&
                    (!anyMade || std::ranges::find(s_madeImmortal, member) != s_madeImmortal.end()))
                    run(member, request.value);
                break;
            case Scope::Hero:
                break;
            }
        }
        if (request.scope == Scope::MadeImmortal)
            s_madeImmortal.clear();
    }

    // What the menu shows of the game.
    void ReadState(uint8_t* base)
    {
        const uint32_t hero = uint32_t(CallGuest(kGetHero.Get()));
        s_immortal = hero != UINT32_MAX && IsImmortal(hero);
        const uint32_t module = uint32_t(CallGuest(kGetModule.Get()));
        int32_t skillCheck = 0;
        if (module != UINT32_MAX)
        {
            const uint32_t name = Scratch(kScratchVariable);
            ConstructWide(base, name, kSkillCheckVariable);
            skillCheck = int32_t(CallGuest(kGetLocalInt.Get(), { module, name }));
        }
        s_skillChecks = skillCheck == kSkillCheckPass;
    }

    // The world paused (unless it is already) and the menu shown.
    void Open(uint8_t* base, uint32_t game)
    {
        s_pausedHere = !PPC_LOAD_U8(game + kPausedOffset);
        if (s_pausedHere)
            SetPaused(game, true);
        ReadState(base);
        {
            std::lock_guard lock(s_requestsMutex);
            s_requests.clear();
            s_open = true;
        }
        LOGI("Cheats: menu open");
        dao::ui::cheats::ShowOverlay();
    }

    void Close(uint8_t* base, uint32_t game)
    {
        if (s_pausedHere && game && PPC_LOAD_U8(game + kPausedOffset))
            SetPaused(game, false);
        s_pausedHere = false;
        {
            std::lock_guard lock(s_requestsMutex);
            s_open = false;
            s_requests.clear();
        }
        LOGI("Cheats: menu closed");
        dao::ui::cheats::CloseOverlay();
    }
}

namespace dao::ui
{
    void ToggleCheats()
    {
        s_toggleAt = Now();
    }

    bool CheatsOpen()
    {
        return s_open.load(std::memory_order_relaxed);
    }

    bool CheatsAvailable()
    {
        const int32_t mode = s_worldMode.load(std::memory_order_relaxed);
        return mode == kModeExplore || mode == kModeCombat;
    }

    uint16_t CheatsFilterButtons(uint16_t buttons)
    {
        if (s_open.load(std::memory_order_relaxed))
        {
            s_heldOver.store(buttons & kMenuButtons, std::memory_order_relaxed);
            return buttons & ~kMenuButtons;
        }
        // Each button still down from the menu stays hidden until let go.
        const uint16_t held = s_heldOver.fetch_and(buttons, std::memory_order_relaxed) & buttons;
        return buttons & ~held;
    }

    void UpdateCheats(uint8_t* base, int32_t mode, uint32_t game)
    {
        if (mode != kModeMenu)
            s_worldMode.store(mode, std::memory_order_relaxed);
        const bool playing = game && (mode == kModeExplore || mode == kModeCombat);
        const int64_t toggleAt = s_toggleAt.load(std::memory_order_relaxed);
        if (toggleAt)
        {
            if (s_open)
            {
                s_toggleAt = 0;
                Close(base, game);
                return;
            }
            if (Now() - toggleAt > std::chrono::nanoseconds(kMenuWait).count())
            {
                // Asked for while the game wasn't updating (loading), or in a menu
                // that didn't close.
                s_toggleAt = 0;
                LOGI("Cheats: not available (asked for too long ago, mode {})", mode);
            }
            else if (playing)
            {
                s_toggleAt = 0;
                Open(base, game);
            }
            else if (mode != kModeMenu)
            {
                // In a conversation, a cutscene, photo mode or on the title screen.
                s_toggleAt = 0;
                LOGI("Cheats: not available in mode {}", mode);
            }
            // (In a menu: waits for it to close.)
        }
        if (!s_open)
            return;
        if (!playing)
        {
            // The game left (a load, a cutscene a script started).
            Close(base, game);
            return;
        }
        // Paused again if a mode switch let the world go on (combat ending, say).
        if (!PPC_LOAD_U8(game + kPausedOffset))
        {
            SetPaused(game, true);
            s_pausedHere = true;
        }

        std::vector<Request> requests;
        {
            std::lock_guard lock(s_requestsMutex);
            requests.swap(s_requests);
        }
        if (requests.empty())
            return;
        for (const Request& request : requests)
            RunRequest(base, request);
        ReadState(base);
    }

    namespace cheats
    {
        void Run(std::string_view name, std::string_view value, Scope scope)
        {
            if (name.empty() || name.size() > kMaxText || value.size() > kMaxText)
            {
                LOGW("Cheats: {} not run (its name or value is too long)", name);
                return;
            }
            std::lock_guard lock(s_requestsMutex);
            if (s_open)
                s_requests.push_back({ std::string(name), std::string(value), scope });
        }

        State GetState()
        {
            return { s_immortal.load(std::memory_order_relaxed), s_skillChecks.load(std::memory_order_relaxed) };
        }
    }
}
