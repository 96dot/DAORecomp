// xam: users/profile, input, notifications, content (saves), enumerators, UI, messages.
#include "stdafx.h"
#include "function.h"
#include "text.h"
#include "module.h"
#include "thread.h"
#include "vfs.h"
#include "content/content.h"
#include "content/save_backups.h"
#include "files.h"

#include "gpu/video.h"
#include "gpu/xenia_host.h"
#include "input/input.h"
#include "ui/keyboard.h"
#include "ui/photo_mode.h"
#include "ui/cheats.h"

namespace dao::imp
{
    using namespace dao::kernel;
    namespace fs = std::filesystem;

    constexpr uint32_t X_E_FAIL = 0x80004005;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525;

    constexpr uint64_t kUserXuid = 0xB13EBABEBABEBABEull; // offline XUID (top nibble E = offline)
    static const char* kUserName = "Warden";

    // ---------------------------------------------------------- overlapped

    struct X_XOVERLAPPED
    {
        u32 result;
        u32 length;
        u32 context;
        u32 event;
        u32 completionRoutine;
        u32 completionContext;
        u32 extendedError;
    };

    static void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
    {
        if (!overlapped)
            return;
        auto* o = g_memory.Translate<X_XOVERLAPPED>(overlapped);
        o->result = result;
        o->length = length;
        o->extendedError = extendedError;
        if (const uint32_t event = o->event)
        {
            if (auto e = LookupHandle<Event>(event, ObjectType::Event))
                e->Set();
        }
        if (const uint32_t routine = o->completionRoutine)
        {
            std::shared_ptr<GuestThread> thread = LookupHandle<GuestThread>(o->context, ObjectType::Thread);
            if (!thread)
                thread = GuestThread::CurrentRef();
            if (thread)
                thread->QueueApc({ 0, routine, result, length, overlapped, 0 });
        }
    }

    // Returns the right value for an immediately completed XAM call.
    static uint32_t Finish(uint32_t overlapped, uint32_t result, uint32_t length = 0, uint32_t extendedError = 0)
    {
        if (!overlapped)
            return result;
        CompleteOverlapped(overlapped, result, extendedError ? extendedError : (result ? 0x80070000 | result : 0), length);
        return X_ERROR_IO_PENDING;
    }

    // ---------------------------------------------------------------- memory

    uint32_t XamAlloc(uint32_t flags, uint32_t size, u32* out)
    {
        if (!out)
            return X_ERROR_INVALID_PARAMETER;
        const uint32_t address = size < 0x10000 ? KernelAlloc(std::max<uint32_t>(size, 1))
            : VirtualHeap4K().Alloc((size + 0xFFF) & ~0xFFFu, 0x1000, true, X_PAGE_READWRITE, false);
        if (address && size >= 0x10000)
            memset(g_memory.Translate(address), 0, size);
        *out = address;
        return address ? X_ERROR_SUCCESS : 8; // X_ERROR_NOT_ENOUGH_MEMORY
    }

    uint32_t XamFree(uint32_t address)
    {
        if (address >= 0x80000000 && address < g_build.info.imageBase)
            KernelFree(address);
        else if (address)
            VirtualHeap4K().Release(address);
        return X_ERROR_SUCCESS;
    }

    // ------------------------------------------------------------ system info

    struct X_VIDEO_MODE
    {
        u32 displayWidth;
        u32 displayHeight;
        u32 isInterlaced;
        u32 isWidescreen;
        u32 isHiDef;
        be<float> refreshRate;
        u32 videoStandard;
        u32 unknown0x8A;
        u32 unknown0x01;
        u32 reserved[3];
    };

    void XGetVideoMode(X_VIDEO_MODE* mode)
    {
        memset(mode, 0, sizeof(*mode));
        mode->displayWidth = 1280;
        mode->displayHeight = 720;
        mode->isInterlaced = 0;
        mode->isWidescreen = 1;
        mode->isHiDef = 1;
        mode->refreshRate = 60.0f;
        mode->videoStandard = 1; // NTSC
        mode->unknown0x8A = 0x4A;
        mode->unknown0x01 = 0x01;
    }

    uint32_t XGetLanguage() { return 1; }       // English
    uint32_t XGetGameRegion() { return 0xFFFF; }
    uint32_t XGetAVPack() { return 8; }         // HDMI
    uint32_t XamGetSystemVersion() { return 0x20446700; }

    uint32_t XamGetExecutionId(u32* out)
    {
        if (out)
            *out = XexHeaderField(g_module.xexHeader, 0x00040006);
        return X_STATUS_SUCCESS;
    }

    void XamLoaderTerminateTitle()
    {
        LOGI("XamLoaderTerminateTitle - exiting");
        TerminateProcess(GetCurrentProcess(), 0);
    }

    void XamLoaderLaunchTitle(const char* name, uint32_t flags)
    {
        LOGI("XamLoaderLaunchTitle('{}', {:#x}) - exiting", name ? name : "", flags);
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // ------------------------------------------------------------------ users

    uint32_t XamUserGetSigninState(uint32_t user)
    {
        return user == 0 ? 1 : 0; // signed in locally
    }

    uint32_t XamUserGetXUID(uint32_t user, uint32_t typeMask, u64* xuid)
    {
        if (!xuid)
            return X_E_INVALIDARG;
        *xuid = 0;
        if (user != 0)
            return X_E_NO_SUCH_USER;
        *xuid = kUserXuid;
        return 0;
    }

    uint32_t XamUserGetName(uint32_t user, char* buffer, uint32_t length)
    {
        if (user >= 4)
            return X_ERROR_INVALID_PARAMETER;
        if (user != 0)
        {
            if (length)
                buffer[0] = 0;
            return X_ERROR_NO_SUCH_USER;
        }
        const size_t n = std::min<size_t>(length ? length - 1 : 0, strlen(kUserName));
        memcpy(buffer, kUserName, n);
        if (length)
            buffer[n] = 0;
        return X_ERROR_SUCCESS;
    }

    struct X_USER_SIGNIN_INFO
    {
        u64 xuid;
        u32 flags;
        u32 signinState;
        u32 guestNum;
        u32 sponsorUserIndex;
        char name[16];
    };
    static_assert(sizeof(X_USER_SIGNIN_INFO) == 40);

    uint32_t XamUserGetSigninInfo(uint32_t user, uint32_t flags, X_USER_SIGNIN_INFO* info)
    {
        if (!info)
            return X_E_INVALIDARG;
        memset(info, 0, sizeof(*info));
        if (user >= 4)
            return X_E_NO_SUCH_USER;
        if (user == 0)
        {
            info->xuid = kUserXuid;
            info->signinState = 1;
            strncpy(info->name, kUserName, 15);
        }
        return 0;
    }

    uint32_t XamUserCheckPrivilege(uint32_t user, uint32_t mask, u32* out)
    {
        if (out)
            *out = 1;
        return (user == 0 || user == 0xFF) ? X_ERROR_SUCCESS : X_ERROR_NO_SUCH_USER;
    }

    uint32_t XamUserGetMembershipTierFromXUID(uint64_t) { return 0; }
    uint32_t XamUserGetOnlineCountryFromXUID(uint64_t) { return 103; }

    uint32_t XamUserAreUsersFriends(uint32_t user, u64* xuids, uint32_t count, u32* areFriends, uint32_t overlapped)
    {
        if (areFriends)
            *areFriends = 0;
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    // --------------------------------------------------------------- profile

    struct X_USER_PROFILE_SETTING
    {
        u32 source;
        u32 pad0;
        u64 xuidOrIndex; // user_index occupies the high word when the xuid is -1
        u32 settingId;
        u32 pad1;
        uint8_t dataType;
        uint8_t pad2[7];
        union
        {
            s32 s32Value;
            u64 u64Value;
            struct
            {
                u32 size;
                u32 ptr;
            } binary;
        };
    };
    static_assert(sizeof(X_USER_PROFILE_SETTING) == 40);

    // Settings are keyed by id; type is in the top nibble, max size in bits 16-27.
    static std::mutex s_profileMutex;
    static std::unordered_map<uint32_t, std::vector<uint8_t>> s_profileSettings;
    static bool s_profileLoaded = false;

    static fs::path ProfileDir()
    {
        return VfsUserRoot() / "profile" / fmt::format("{:08X}", g_module.titleId);
    }

    static void LoadProfile()
    {
        if (s_profileLoaded)
            return;
        s_profileLoaded = true;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(ProfileDir(), ec))
        {
            uint32_t id = 0;
            if (entry.path().extension() != ".bin" ||
                sscanf(dao::Utf8(entry.path().filename()).c_str(), "%08X.bin", &id) != 1)
                continue;
            const uint64_t size = entry.file_size(ec);
            if (ec || size > 0x1000) // (a setting holds at most 0xFFF bytes)
                continue;
            FILE* f = _wfopen(entry.path().c_str(), L"rb");
            if (!f)
                continue;
            std::vector<uint8_t> data(size_t(size), 0);
            const bool read = fread(data.data(), 1, data.size(), f) == data.size();
            fclose(f);
            if (read)
                s_profileSettings[id] = std::move(data);
        }
    }

    static void SaveSetting(uint32_t id, const std::vector<uint8_t>& data)
    {
        std::error_code ec;
        fs::create_directories(ProfileDir(), ec);
        // Written next to it and swapped in, so a crash mid-write keeps the old value
        // (not with WriteFileAtomically: this one is written through to the disk).
        const fs::path path = ProfileDir() / fmt::format("{:08X}.bin", id);
        const fs::path temp = fs::path(path).concat(L".tmp");
        FILE* f = _wfopen(temp.c_str(), L"wb");
        if (!f)
            return;
        const bool written = fwrite(data.data(), 1, data.size(), f) == data.size();
        if (fclose(f) != 0 || !written ||
            !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            LOGW("Profile setting {:08X}: can't save it", id);
            fs::remove(temp, ec);
        }
    }

    // Defaults for well-known gamer profile settings (type INT32 unless noted).
    static int32_t DefaultSetting(uint32_t id)
    {
        switch (id)
        {
        case 0x10040002: return 0;  // YAXIS_INVERSION
        case 0x10040003: return 1;  // OPTION_CONTROLLER_VIBRATION
        case 0x10040015: return 0;  // GAMER_DIFFICULTY (normal)
        case 0x10040018: return 1;  // GAMER_CONTROL_SENSITIVITY (medium)
        case 0x1004001D: return 0;  // GAMER_PREFERRED_COLOR_FIRST
        case 0x1004001E: return 0;  // GAMER_PREFERRED_COLOR_SECOND
        case 0x10040021: return 1;  // GAMER_ACTION_AUTO_AIM
        case 0x10040022: return 1;  // GAMER_ACTION_AUTO_CENTER
        case 0x10040023: return 0;  // GAMER_ACTION_MOVEMENT_CONTROL
        case 0x10040026: return 0;  // GAMER_RACE_TRANSMISSION
        case 0x10040027: return 0;  // GAMER_RACE_CAMERA_LOCATION
        case 0x10040028: return 0;  // GAMER_RACE_BRAKE_CONTROL
        case 0x10040029: return 0;  // GAMER_RACE_ACCELERATOR_CONTROL
        case 0x10040038: return 0;  // GAMER_TITLE_CRED_EARNED
        case 0x10040039: return 0;  // GAMER_TITLE_ACHIEVEMENTS_EARNED
        default: return 0;
        }
    }

    uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t user, uint32_t xuidCount, u64* xuids,
        uint32_t settingCount, u32* settingIds, u32* bufferSizePtr, uint8_t* buffer, uint32_t overlapped)
    {
        if (!settingCount || settingCount > 32 || !bufferSizePtr)
            return X_ERROR_INVALID_PARAMETER;

        const uint32_t users = xuids ? std::max<uint32_t>(xuidCount, 1) : 1;
        uint32_t dataSize = 0;
        for (uint32_t i = 0; i < settingCount; i++)
        {
            const uint32_t id = settingIds[i];
            const uint32_t type = id >> 28;
            if (type == 4 || type == 6) // WSTRING / BINARY
                dataSize += (id >> 16) & 0xFFF;
        }
        const uint32_t needed = 8 + users * (settingCount * sizeof(X_USER_PROFILE_SETTING) + dataSize);
        const uint32_t bufferSize = *bufferSizePtr;
        if (!buffer || bufferSize < needed)
        {
            if (!bufferSize)
                *bufferSizePtr = needed;
            return X_ERROR_INSUFFICIENT_BUFFER;
        }

        std::lock_guard lock(s_profileMutex);
        LoadProfile();

        const uint32_t base = g_memory.ToGuest(buffer);
        *reinterpret_cast<u32*>(buffer) = settingCount;
        *reinterpret_cast<u32*>(buffer + 4) = base + 8;
        auto* out = reinterpret_cast<X_USER_PROFILE_SETTING*>(buffer + 8);
        uint32_t data = base + 8 + settingCount * sizeof(X_USER_PROFILE_SETTING);

        for (uint32_t i = 0; i < settingCount; i++)
        {
            const uint32_t id = settingIds[i];
            auto& s = out[i];
            memset(&s, 0, sizeof(s));
            s.settingId = id;
            if (xuids)
                s.xuidOrIndex = kUserXuid;
            else
                s.xuidOrIndex = (uint64_t(user) << 32);
            const uint32_t type = id >> 28;
            s.dataType = uint8_t(type);

            auto it = s_profileSettings.find(id);
            if (type == 4 || type == 6)
            {
                const uint32_t maxSize = (id >> 16) & 0xFFF;
                s.binary.ptr = data;
                if (it != s_profileSettings.end())
                {
                    const uint32_t size = std::min<uint32_t>(maxSize, uint32_t(it->second.size()));
                    memcpy(g_memory.Translate(data), it->second.data(), size);
                    s.binary.size = size;
                    s.source = 2; // title
                }
                else
                {
                    s.binary.size = 0;
                    s.source = 1; // default
                }
                data += maxSize;
            }
            else if (it != s_profileSettings.end() && it->second.size() >= 8)
            {
                memcpy(&s.u64Value, it->second.data(), 8);
                s.source = 2;
            }
            else
            {
                s.s32Value = DefaultSetting(id);
                s.source = 1;
            }
        }
        LOGT("XamUserReadProfileSettings({:08X}, {}, {} settings)", titleId, user, settingCount);
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamUserWriteProfileSettings(uint32_t titleId, uint32_t user, uint32_t settingCount,
        X_USER_PROFILE_SETTING* settings, uint32_t overlapped)
    {
        if (!settingCount || !settings)
            return X_ERROR_INVALID_PARAMETER;
        std::lock_guard lock(s_profileMutex);
        LoadProfile();
        for (uint32_t i = 0; i < settingCount; i++)
        {
            const auto& s = settings[i];
            const uint32_t id = s.settingId;
            const uint32_t type = id >> 28;
            std::vector<uint8_t> value;
            if (type == 4 || type == 6)
            {
                // No more than the setting holds (its id carries the size), as reading it back.
                const uint32_t size = std::min<uint32_t>(s.binary.size, (id >> 16) & 0xFFF);
                const uint8_t* src = g_memory.Translate<uint8_t>(s.binary.ptr);
                value.assign(src, src + size);
            }
            else
            {
                value.resize(8);
                memcpy(value.data(), &s.u64Value, 8);
            }
            s_profileSettings[id] = value;
            SaveSetting(id, value);
        }
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    // ------------------------------------------------------------------ input

    struct X_INPUT_GAMEPAD
    {
        u16 buttons;
        uint8_t leftTrigger;
        uint8_t rightTrigger;
        be<int16_t> thumbLX, thumbLY, thumbRX, thumbRY;
    };

    struct X_INPUT_STATE
    {
        u32 packetNumber;
        X_INPUT_GAMEPAD gamepad;
    };
    static_assert(sizeof(X_INPUT_STATE) == 16);

    struct X_INPUT_CAPABILITIES
    {
        uint8_t type;
        uint8_t subType;
        u16 flags;
        X_INPUT_GAMEPAD gamepad;
        u16 leftMotor, rightMotor;
    };
    static_assert(sizeof(X_INPUT_CAPABILITIES) == 20);

    uint32_t XamInputGetCapabilities(uint32_t user, uint32_t flags, X_INPUT_CAPABILITIES* caps)
    {
        if (!caps)
            return X_ERROR_INVALID_PARAMETER;
        memset(caps, 0, sizeof(*caps));
        if ((flags & 0xFF) && (flags & 1) == 0) // non-gamepad devices
            return X_ERROR_DEVICE_NOT_CONNECTED;
        dao::input::PadState host;
        if (!dao::input::GetState(user == 0xFF ? 0 : user, host))
            return X_ERROR_DEVICE_NOT_CONNECTED;
        caps->type = 1;    // gamepad
        caps->subType = 1; // standard
        caps->gamepad.buttons = 0xF3FF;
        caps->gamepad.leftTrigger = 0xFF;
        caps->gamepad.rightTrigger = 0xFF;
        caps->gamepad.thumbLX = int16_t(0xFFC0);
        caps->gamepad.thumbLY = int16_t(0xFFC0);
        caps->gamepad.thumbRX = int16_t(0xFFC0);
        caps->gamepad.thumbRY = int16_t(0xFFC0);
        caps->leftMotor = 0xFFFF;
        caps->rightMotor = 0xFFFF;
        return X_ERROR_SUCCESS;
    }

    uint32_t XamInputGetState(uint32_t user, uint32_t flags, X_INPUT_STATE* state)
    {
        // Low latency waits here, before the frame's first read of the controller
        // (a frame: one VdSwap, g_video.swapCount).
        static std::atomic<uint64_t> s_lowLatencySwap{ UINT64_MAX };
        const uint64_t swap = dao::gpu::g_video.swapCount.load(std::memory_order_relaxed);
        if (s_lowLatencySwap.exchange(swap, std::memory_order_relaxed) != swap)
        {
            dao::gpu::XeniaLowLatencyFrameStart();
            // A swap during the wait is still this frame's (the game runs a frame
            // ahead at times): not another wait for it.
            s_lowLatencySwap.store(dao::gpu::g_video.swapCount.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        if (state)
            memset(state, 0, sizeof(*state));
        if ((flags & 0xFF) && (flags & 1) == 0)
            return X_ERROR_DEVICE_NOT_CONNECTED;
        dao::input::PadState host;
        if (!dao::input::GetState(user == 0xFF ? 0 : user, host))
            return X_ERROR_DEVICE_NOT_CONNECTED;
        // Nothing pressed while the on-screen keyboard has the controller.
        if (state && dao::ui::KeyboardHoldsInput())
            state->packetNumber = host.packetNumber;
        else if (state)
        {
            state->packetNumber = host.packetNumber;
            // (Photo mode's buttons are its own, and the cheats menu's all of the
            // controller: the first player's.)
            const bool first = user == 0 || user == 0xFF;
            state->gamepad.buttons = first
                ? dao::ui::CheatsFilterButtons(dao::ui::PhotoModeFilterButtons(host.buttons))
                : host.buttons;
            if (first && dao::ui::CheatsOpen())
            {
                state->gamepad.leftTrigger = state->gamepad.rightTrigger = 0;
                state->gamepad.thumbLX = state->gamepad.thumbLY = 0;
                state->gamepad.thumbRX = state->gamepad.thumbRY = 0;
            }
            else
            {
                state->gamepad.leftTrigger = host.leftTrigger;
                state->gamepad.rightTrigger = host.rightTrigger;
                state->gamepad.thumbLX = host.thumbLX;
                state->gamepad.thumbLY = host.thumbLY;
                state->gamepad.thumbRX = host.thumbRX;
                state->gamepad.thumbRY = host.thumbRY;
            }
        }
        return X_ERROR_SUCCESS;
    }

    uint32_t XamInputSetState(uint32_t user, uint32_t unk, u16* vibration)
    {
        const uint32_t index = user == 0xFF ? 0 : user;
        dao::input::PadState host;
        if (!dao::input::GetState(index, host))
            return X_ERROR_DEVICE_NOT_CONNECTED;
        if (vibration)
            dao::input::SetRumble(index, vibration[0], vibration[1]);
        return X_ERROR_SUCCESS;
    }

    // ---------------------------------------------------------- notifications

    struct NotifyListener final : KernelObject
    {
        NotifyListener() : KernelObject(ObjectType::NotifyListener) {}
        uint64_t mask = 0;
        std::deque<std::pair<uint32_t, uint32_t>> queue; // guarded by the dispatcher lock

        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return !queue.empty(); }
    };

    static std::mutex s_listenersMutex;
    static std::vector<std::weak_ptr<NotifyListener>> s_listeners;
    static bool s_notifiedSystem, s_notifiedLive;

    static void Enqueue(NotifyListener& listener, uint32_t id, uint32_t param)
    {
        auto lock = LockDispatcher();
        listener.queue.emplace_back(id, param);
        NotifyDispatcher(listener);
    }

    uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t maxVersion)
    {
        auto listener = std::make_shared<NotifyListener>();
        listener->mask = mask;
        {
            std::lock_guard lock(s_listenersMutex);
            s_listeners.push_back(listener);
        }
        if (!s_notifiedSystem && (mask & 1))
        {
            s_notifiedSystem = true;
            Enqueue(*listener, 0x00000009, 0); // XN_SYS_UI: hidden
            Enqueue(*listener, 0x0000000A, 1); // XN_SYS_SIGNINCHANGED
        }
        if (!s_notifiedLive && (mask & 2))
        {
            s_notifiedLive = true;
            Enqueue(*listener, 0x02000001, 0x001510F1); // XN_LIVE_CONNECTIONCHANGED: logon disconnected
            Enqueue(*listener, 0x02000003, 0);          // XN_LIVE_LINK_STATE_CHANGED
        }
        const uint32_t handle = CreateHandle(listener);
        LOGI("XamNotifyCreateListener({:#x}) -> {:08X}", mask, handle);
        return handle;
    }

    // A notification for every listener of its area (the id's top bits).
    static void Broadcast(uint32_t id, uint32_t param)
    {
        std::vector<std::shared_ptr<NotifyListener>> listeners;
        {
            std::lock_guard lock(s_listenersMutex);
            for (const auto& weak : s_listeners)
            {
                if (auto listener = weak.lock())
                    listeners.push_back(std::move(listener));
            }
        }
        for (const auto& listener : listeners)
        {
            if (listener->mask & (1ull << (id >> 25)))
                Enqueue(*listener, id, param);
        }
    }

    bool XNotifyGetNext(uint32_t handle, uint32_t matchId, u32* idOut, u32* paramOut)
    {
        if (paramOut)
            *paramOut = 0;
        if (!idOut)
            return false;
        *idOut = 0;
        auto listener = LookupHandle<NotifyListener>(handle, ObjectType::NotifyListener);
        if (!listener)
            return false;

        auto lock = LockDispatcher();
        for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
        {
            if (matchId && it->first != matchId)
                continue;
            *idOut = it->first;
            if (paramOut)
                *paramOut = it->second;
            LOGT("XNotifyGetNext -> {:08X} ({:08X})", it->first, it->second);
            listener->queue.erase(it);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------- enumerators

    struct Enumerator final : KernelObject
    {
        Enumerator() : KernelObject(ObjectType::Enumerator) {}
        uint32_t itemSize = 0;
        uint32_t itemsPerEnumerate = 1;
        std::vector<uint8_t> items;
        size_t next = 0;

        size_t Count() const { return itemSize ? items.size() / itemSize : 0; }
    };

    uint32_t XamEnumerate(uint32_t handle, uint32_t flags, uint8_t* buffer, uint32_t bufferSize, u32* itemsReturned,
        uint32_t overlapped)
    {
        auto e = LookupHandle<Enumerator>(handle, ObjectType::Enumerator);
        if (!e)
            return X_ERROR_INVALID_HANDLE;
        if (itemsReturned)
            *itemsReturned = 0;

        const size_t remaining = e->Count() - e->next;
        const size_t fit = e->itemSize ? bufferSize / e->itemSize : 0;
        const size_t count = std::min({ remaining, size_t(e->itemsPerEnumerate), fit });
        if (!count)
            return Finish(overlapped, overlapped ? X_ERROR_FUNCTION_FAILED : X_ERROR_NO_MORE_FILES, 0,
                0x80070000 | X_ERROR_NO_MORE_FILES);

        memcpy(buffer, e->items.data() + e->next * e->itemSize, count * e->itemSize);
        e->next += count;
        if (itemsReturned)
            *itemsReturned = uint32_t(count);
        return Finish(overlapped, X_ERROR_SUCCESS, uint32_t(count));
    }

    uint32_t XamCreateEnumeratorHandle(uint32_t user, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
        uint32_t extraSize, uint32_t itemCount, uint32_t flags, u32* handleOut)
    {
        auto e = std::make_shared<Enumerator>();
        e->itemSize = std::max<uint32_t>(extraSize, 1);
        e->itemsPerEnumerate = std::max<uint32_t>(itemCount, 1);
        if (handleOut)
            *handleOut = CreateHandle(e);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, u32* out)
    {
        auto e = LookupHandle<Enumerator>(handle, ObjectType::Enumerator);
        if (!e || !out)
            return X_ERROR_INVALID_HANDLE;
        // Hand out a guest copy of the extra data area.
        *out = 0;
        return X_ERROR_SUCCESS;
    }

    uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t user, uint64_t xuid, uint32_t flags,
        uint32_t offset, uint32_t count, u32* bufferSize, u32* handleOut)
    {
        if (!count || !bufferSize || !handleOut)
            return X_ERROR_INVALID_PARAMETER;
        auto e = std::make_shared<Enumerator>();
        e->itemSize = 0x1C; // XACHIEVEMENT_DETAILS without strings
        e->itemsPerEnumerate = count;
        *bufferSize = count * 0x1C;
        *handleOut = CreateHandle(e);
        return X_ERROR_SUCCESS;
    }

    // ---------------------------------------------------------------- content

    struct XCONTENT_DATA
    {
        u32 deviceId;
        u32 contentType;
        u16 displayName[128];
        char fileName[42];
        uint8_t padding[2];
    };
    static_assert(sizeof(XCONTENT_DATA) == 0x134);

    static_assert(sizeof(XCONTENT_DATA::displayName) == content::kDisplayNameSize);

    constexpr uint32_t kHddDeviceId = 1;

    static std::string ContentFileName(const XCONTENT_DATA& data)
    {
        return std::string(data.fileName, strnlen(data.fileName, sizeof(data.fileName)));
    }

    static fs::path ContentPath(uint32_t contentType, const std::string& fileName)
    {
        return content::ContentFolder(VfsUserRoot(), g_module.titleId, contentType) / fileName;
    }

    // Content creation and closing, one at a time (a save's folder is replaced in steps).
    static std::mutex s_contentMutex;
    // Saves being written (root -> save name): each has a <name>.writing file in
    // save_backups until the game closes it.
    static std::unordered_map<std::string, std::string> s_savesWriting;

    static fs::path SaveBackups()
    {
        return content::SaveBackupsFolder(VfsUserRoot());
    }

    // The mark of a save being written (see BackUpSave).
    static fs::path WritingMarker(const std::string& fileName)
    {
        // (Converted as the save's folder name is, in ContentPath.)
        return content::SaveWritingMarker(VfsUserRoot(), fs::path(fileName));
    }

    static uint64_t FolderSize(const fs::path& folder)
    {
        uint64_t size = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec))
                size += it->file_size(ec);
        }
        return size;
    }

    // Overwriting a save deletes the old one before the new one is written, so a crash in
    // between would lose both: the old one (folder and display name) is kept first in
    // userdata/save_backups/<name>, outside the saves' folder so the game doesn't list it.
    // A save the game didn't close (a crash while writing it) doesn't replace a backup.
    static void BackUpSave(const fs::path& path, const std::string& fileName)
    {
        // Room for the new save's own files and the log besides.
        constexpr uint64_t kSpareBytes = 64ull << 20;
        std::error_code ec;
        const fs::path backups = SaveBackups();
        const fs::path backup = backups / fileName;
        const fs::path staged = backups / (fileName + ".new");
        const fs::path nameFile = FromUtf8(fileName + ".name");
        if (fs::exists(WritingMarker(fileName), ec))
        {
            LOGW("XamContentCreateEx: '{}' wasn't closed cleanly; its backup stays as it was", fileName);
            return;
        }
        // The old save's space isn't freed until the new one is written.
        const fs::space_info space = fs::space(path, ec);
        if (!ec && space.available < FolderSize(path) + kSpareBytes)
        {
            LOGW("XamContentCreateEx: too little disk space to back up '{}'", fileName);
            return;
        }

        // Staged first: the old backup goes only once the new one is whole.
        fs::create_directories(backups, ec);
        fs::remove_all(staged, ec);
        fs::rename(path, staged, ec);
        if (ec)
        {
            // A folder with a file still open in it can't be moved: copied instead.
            ec.clear();
            fs::copy(path, staged, fs::copy_options::recursive, ec);
            if (ec)
            {
                LOGW("XamContentCreateEx: can't back up '{}' before it's overwritten ({})", fileName,
                    ec.message());
                fs::remove_all(staged, ec);
                return;
            }
        }
        fs::remove_all(backup, ec);
        if (!ec)
            fs::rename(staged, backup, ec);
        if (ec)
        {
            // (Kept there: the next backup of this save replaces it.)
            LOGW("XamContentCreateEx: can't put the backup of '{}' in place ({}); it's in save_backups as "
                "{}.new", fileName, ec.message(), fileName);
            return;
        }

        fs::copy_file(path.parent_path() / nameFile, backups / nameFile, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            // Better none than an older save's.
            fs::remove(backups / nameFile, ec);
            LOGW("XamContentCreateEx: backed up '{}' without its display name", fileName);
            return;
        }
        LOGI("XamContentCreateEx: backed up '{}' to save_backups", fileName);
    }

    static std::string RootName(const char* root)
    {
        std::string name = root ? root : "";
        if (!name.empty() && name.back() != ':')
            name += ':';
        return name;
    }

    uint32_t XamContentCreateEx(uint32_t user, const char* rootName, XCONTENT_DATA* data, uint32_t flags,
        u32* dispositionOut, u32* licenseOut, uint32_t cacheSize, uint64_t contentSize, uint32_t overlapped)
    {
        if (!data || !rootName)
            return X_ERROR_INVALID_PARAMETER;
        const std::string root = RootName(rootName);
        const std::string fileName = ContentFileName(*data);
        // The name becomes a folder in the saves' folder, which "create always" deletes
        // first: one folder there, never the folder itself or anything outside it.
        if (!content::IsContentName(fileName))
        {
            LOGW("XamContentCreateEx: refused the content name '{}'", fileName);
            return X_ERROR_INVALID_PARAMETER;
        }
        const fs::path path = ContentPath(data->contentType, fileName);
        const bool save = data->contentType == content::kContentSave;

        std::unique_lock lock(s_contentMutex);
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        // Content replaced by "create always" or "truncate existing".
        auto removeOld = [&]
        {
            if (save)
                BackUpSave(path, fileName);
            fs::remove_all(path, ec);
        };
        const uint32_t mode = flags & 0xF;
        uint32_t result = X_ERROR_SUCCESS;
        uint32_t disposition = 0;
        switch (mode)
        {
        case 1: // create new
            if (exists) result = X_ERROR_ALREADY_EXISTS; else disposition = 1;
            break;
        case 2: // create always
            if (exists)
                removeOld();
            disposition = 1;
            break;
        case 3: // open existing
            if (!exists) result = X_ERROR_FILE_NOT_FOUND; else disposition = 2;
            break;
        case 4: // open always
            disposition = exists ? 2 : 1;
            break;
        case 5: // truncate existing
            if (!exists) result = X_ERROR_FILE_NOT_FOUND;
            else
            {
                removeOld();
                disposition = 1;
            }
            break;
        default:
            result = X_ERROR_INVALID_PARAMETER;
            break;
        }

        if (result == X_ERROR_SUCCESS)
        {
            // Not mounted if its folder can't be made (a full disk, no permission): the
            // game then reports the failure instead of writing into nothing.
            fs::create_directories(path, ec);
            if (ec)
            {
                LOGE("XamContentCreateEx: can't create the folder of '{}' ({})", fileName, ec.message());
                result = X_ERROR_ACCESS_DENIED;
                disposition = 0;
                // Nor is content mounted at this root before (not closed) written into.
                if (auto it = s_savesWriting.find(root); it != s_savesWriting.end())
                {
                    fs::remove(WritingMarker(it->second), ec);
                    s_savesWriting.erase(it);
                }
                VfsDeleteSymlink(root);
                VfsUnmountDevice("\\Device\\Content\\" + root.substr(0, root.size() - 1));
            }
        }
        if (result == X_ERROR_SUCCESS)
        {
            // Display name, for enumeration later. Only for new content: opening a
            // save keeps its name (the game passes a fresh one, e.g. the time now).
            if (disposition == 1)
                content::WriteDisplayName(path.parent_path(), fileName, data->displayName);
            const std::string device = "\\Device\\Content\\" + root.substr(0, root.size() - 1);
            VfsMountDevice(device, path);
            VfsCreateSymlink(root, device);
            // Content mounted here before without being closed is done with.
            if (auto it = s_savesWriting.find(root); it != s_savesWriting.end())
            {
                fs::remove(WritingMarker(it->second), ec);
                s_savesWriting.erase(it);
            }
            // Being written until the game closes it (see BackUpSave).
            if (save && disposition == 1)
            {
                fs::create_directories(SaveBackups(), ec);
                if (FILE* f = _wfopen(WritingMarker(fileName).c_str(), L"wb"))
                {
                    fclose(f);
                    s_savesWriting[root] = fileName;
                }
                else
                {
                    LOGW("XamContentCreateEx: can't mark '{}' as being written", fileName);
                }
            }
        }
        lock.unlock();

        LOGI("XamContentCreateEx({}, '{}', type {:08X}, '{}', flags {:#x}) -> {} (disposition {})", user, root,
            uint32_t(data->contentType), fileName, flags, result, disposition);
        if (dispositionOut)
            *dispositionOut = disposition;
        // The game loads a DLC package's files only when this licence covers its
        // manifest's LicenseMask. The packages are the player's own (installed from
        // their console): all of their licences are granted.
        if (licenseOut)
            *licenseOut = data->contentType == content::kContentMarketplace ? 0xFFFFFFFF : 0;
        return Finish(overlapped, result);
    }

    uint32_t XamContentClose(const char* rootName, uint32_t overlapped)
    {
        const std::string root = RootName(rootName);
        {
            // Unmounted under the lock too, so not after the same root was created again.
            std::lock_guard lock(s_contentMutex);
            if (auto it = s_savesWriting.find(root); it != s_savesWriting.end())
            {
                std::error_code ec;
                fs::remove(WritingMarker(it->second), ec);
                s_savesWriting.erase(it);
            }
            VfsDeleteSymlink(root);
            VfsUnmountDevice("\\Device\\Content\\" + root.substr(0, root.size() - 1));
        }
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamContentCreateEnumerator(uint32_t user, uint32_t deviceId, uint32_t contentType, uint32_t flags,
        uint32_t itemsPerEnumerate, u32* bufferSize, u32* handleOut)
    {
        if (!handleOut)
            return X_ERROR_INVALID_PARAMETER;
        if (deviceId && deviceId != kHddDeviceId)
            return X_ERROR_DEVICE_NOT_CONNECTED;
        if (bufferSize)
            *bufferSize = std::max<uint32_t>(itemsPerEnumerate, 1) * sizeof(XCONTENT_DATA);

        auto e = std::make_shared<Enumerator>();
        e->itemSize = sizeof(XCONTENT_DATA);
        e->itemsPerEnumerate = std::max<uint32_t>(itemsPerEnumerate, 1);

        std::error_code ec;
        const fs::path dir = ContentPath(contentType, "");
        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            const std::optional<std::string> guestName = dao::ToAnsi(entry.path().filename());
            if (!entry.is_directory(ec) || !guestName)
                continue;
            XCONTENT_DATA item{};
            item.deviceId = kHddDeviceId;
            item.contentType = contentType;
            const std::string& name = *guestName;
            memcpy(item.fileName, name.data(), std::min<size_t>(name.size(), sizeof(item.fileName)));
            FILE* f = _wfopen((dir / (name + ".name")).c_str(), L"rb");
            if (f)
            {
                fread(item.displayName, 1, sizeof(item.displayName), f);
                fclose(f);
            }
            const size_t at = e->items.size();
            e->items.resize(at + sizeof(item));
            memcpy(e->items.data() + at, &item, sizeof(item));
        }
        LOGI("XamContentCreateEnumerator(type {:08X}) -> {} items", contentType, e->Count());
        *handleOut = CreateHandle(e);
        return X_ERROR_SUCCESS;
    }

    uint32_t XamContentGetCreator(uint32_t user, XCONTENT_DATA* data, u32* isCreator, u64* creatorXuid,
        uint32_t overlapped)
    {
        if (isCreator)
            *isCreator = 1;
        if (creatorXuid)
            *creatorXuid = kUserXuid;
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamContentGetDeviceState(uint32_t deviceId, uint32_t overlapped)
    {
        const uint32_t result = (deviceId == kHddDeviceId || deviceId == 0) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
        return Finish(overlapped, result);
    }

    struct X_CONTENT_DEVICE_DATA
    {
        u32 deviceId;
        u32 deviceType;
        u64 totalBytes;
        u64 freeBytes;
        u16 name[28];
    };
    static_assert(sizeof(X_CONTENT_DEVICE_DATA) == 0x50);

    uint32_t XamContentGetDeviceData(uint32_t deviceId, X_CONTENT_DEVICE_DATA* data)
    {
        if (deviceId != kHddDeviceId)
            return X_ERROR_DEVICE_NOT_CONNECTED;
        memset(data, 0, sizeof(*data));
        data->deviceId = kHddDeviceId;
        data->deviceType = 1; // HDD
        data->totalBytes = 20ull << 30;
        data->freeBytes = 16ull << 30;
        const char* name = "Hard Drive";
        for (size_t i = 0; name[i] && i < 27; i++)
            data->name[i] = uint16_t(name[i]);
        return X_ERROR_SUCCESS;
    }

    // --------------------------------------------------------------------- UI

    uint32_t XamShowDeviceSelectorUI(uint32_t user, uint32_t contentType, uint32_t contentFlags,
        uint64_t totalRequested, u32* deviceIdOut, uint32_t overlapped)
    {
        if (deviceIdOut)
            *deviceIdOut = kHddDeviceId;
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamShowSigninUI(uint32_t panes, uint32_t flags)
    {
        return X_ERROR_SUCCESS;
    }

    // XamShowMessageBoxUI(user, title, text, buttonCount, buttons, focus, flags, result*, overlapped)
    uint32_t XamShowMessageBoxUI(uint32_t user, const u16* title, const u16* text, uint32_t buttonCount,
        uint32_t buttons, uint32_t focusButton, uint32_t flags, u32* resultOut, uint32_t overlapped)
    {
        auto narrow = [](const u16* s) {
            std::string out;
            for (size_t i = 0; s && s[i] != 0 && i < 1024; i++)
                out += char(uint16_t(s[i]) < 0x80 ? uint16_t(s[i]) : '?');
            return out;
        };
        LOGW("XamShowMessageBoxUI: '{}': '{}' -> button {}", narrow(title), narrow(text), uint32_t(focusButton));
        if (resultOut)
            *resultOut = focusButton;
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XamShowMessageBoxUIEx(uint32_t user, const u16* title, const u16* text, uint32_t buttonCount,
        uint32_t buttons, uint32_t focusButton, uint32_t flags, u32* resultOut, uint32_t overlapped)
    {
        return XamShowMessageBoxUI(user, title, text, buttonCount, buttons, focusButton, flags, resultOut, overlapped);
    }

    uint32_t XamShowDirtyDiscErrorUI(uint32_t user)
    {
        LOGE("XamShowDirtyDiscErrorUI - a read failed");
        TerminateProcess(GetCurrentProcess(), 1);
        return 0;
    }

    // XamShowKeyboardUI(user, flags, default, title, desc, buffer, length, overlapped):
    // the on-screen keyboard (ui/keyboard.cpp). With an overlapped, it completes when
    // the player is done (ERROR_CANCELLED if they cancelled), the system UI shown
    // meanwhile as the console's is (XN_SYS_UI).
    uint32_t XamShowKeyboardUI(uint32_t user, uint32_t flags, const u16* defaultText, const u16* title,
        const u16* description, u16* buffer, uint32_t length, uint32_t overlapped)
    {
        if (!buffer || !length)
            return X_ERROR_INVALID_PARAMETER;
        constexpr size_t kMaxGuestText = 1024;
        auto read = [](const u16* text) {
            std::u16string out;
            for (size_t i = 0; text && i < kMaxGuestText && text[i] != 0; i++)
                out += char16_t(uint16_t(text[i]));
            return out;
        };
        const dao::ui::KeyboardRequest request{ read(title), read(description), read(defaultText), length - 1 };
        auto run = [request, buffer, length]() -> uint32_t {
            constexpr uint32_t kSystemUi = 0x00000009;   // XN_SYS_UI: param 1 shown, 0 hidden
            Broadcast(kSystemUi, 1);
            const auto text = dao::ui::RunKeyboard(request);
            if (text)
            {
                const size_t count = std::min<size_t>(text->size(), length - 1);
                for (size_t i = 0; i < count; i++)
                    buffer[i] = uint16_t((*text)[i]);
                buffer[count] = 0;
            }
            Broadcast(kSystemUi, 0);
            return text ? X_ERROR_SUCCESS : X_ERROR_CANCELLED;
        };
        if (!overlapped)
            return run();
        std::thread([run, overlapped] {
            SetThreadDescription(GetCurrentThread(), L"Keyboard");
            const uint32_t result = run();
            CompleteOverlapped(overlapped, result, result ? 0x80070000 | result : 0, 0);
        }).detach();
        return X_ERROR_IO_PENDING;
    }

    uint32_t XamShowAchievementsUI(uint32_t user, uint32_t unk) { return X_ERROR_SUCCESS; }
    uint32_t XamShowMarketplaceUI(uint32_t user, uint32_t type, uint64_t offerId, uint32_t contentTypes)
    {
        return X_ERROR_SUCCESS;
    }

    // --------------------------------------------------------------- messages

    uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t param1, uint32_t param2)
    {
        LOGT("XMsgInProcessCall(app {:02X}, msg {:08X}, {:08X}, {:08X})", app, message, param1, param2);
        return X_ERROR_SUCCESS;
    }

    uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t length)
    {
        LOGT("XMsgStartIORequest(app {:02X}, msg {:08X}, buf {:08X}+{:#x})", app, message, buffer, length);
        return Finish(overlapped, X_ERROR_SUCCESS);
    }

    uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
        uint32_t length, uint32_t taskArgs)
    {
        return XMsgStartIORequest(app, message, overlapped, buffer, length);
    }

    uint32_t XMsgCancelIORequest(uint32_t overlapped, bool wait)
    {
        return X_ERROR_SUCCESS;
    }

    // ------------------------------------------------------------------ voice

    uint32_t XamVoiceHeadsetPresent(uint32_t voice) { return 0; }
    uint32_t XamVoiceCreate(uint32_t user, uint32_t flags, u32* out)
    {
        if (out)
            *out = 0;
        return X_ERROR_ACCESS_DENIED;
    }
    uint32_t XamVoiceClose(uint32_t voice) { return 0; }
    uint32_t XamVoiceSubmitPacket(uint32_t voice, uint32_t size, uint32_t data) { return X_ERROR_ACCESS_DENIED; }
}

DAO_IMPORT_AS(XamAlloc, dao::imp::XamAlloc)
DAO_IMPORT_AS(XamFree, dao::imp::XamFree)
DAO_IMPORT_AS(XGetVideoMode, dao::imp::XGetVideoMode)
DAO_IMPORT_AS(XGetLanguage, dao::imp::XGetLanguage)
DAO_IMPORT_AS(XGetGameRegion, dao::imp::XGetGameRegion)
DAO_IMPORT_AS(XGetAVPack, dao::imp::XGetAVPack)
DAO_IMPORT_AS(XamGetSystemVersion, dao::imp::XamGetSystemVersion)
DAO_IMPORT_AS(XamGetExecutionId, dao::imp::XamGetExecutionId)
DAO_IMPORT_AS(XamLoaderTerminateTitle, dao::imp::XamLoaderTerminateTitle)
DAO_IMPORT_AS(XamLoaderLaunchTitle, dao::imp::XamLoaderLaunchTitle)
DAO_IMPORT_AS(XamUserGetSigninState, dao::imp::XamUserGetSigninState)
DAO_IMPORT_AS(XamUserGetXUID, dao::imp::XamUserGetXUID)
DAO_IMPORT_AS(XamUserGetName, dao::imp::XamUserGetName)
DAO_IMPORT_AS(XamUserGetSigninInfo, dao::imp::XamUserGetSigninInfo)
DAO_IMPORT_AS(XamUserCheckPrivilege, dao::imp::XamUserCheckPrivilege)
DAO_IMPORT_AS(XamUserGetMembershipTierFromXUID, dao::imp::XamUserGetMembershipTierFromXUID)
DAO_IMPORT_AS(XamUserGetOnlineCountryFromXUID, dao::imp::XamUserGetOnlineCountryFromXUID)
DAO_IMPORT_AS(XamUserAreUsersFriends, dao::imp::XamUserAreUsersFriends)
DAO_IMPORT_AS(XamUserReadProfileSettings, dao::imp::XamUserReadProfileSettings)
DAO_IMPORT_AS(XamUserWriteProfileSettings, dao::imp::XamUserWriteProfileSettings)
DAO_IMPORT_AS(XamInputGetCapabilities, dao::imp::XamInputGetCapabilities)
DAO_IMPORT_AS(XamInputGetState, dao::imp::XamInputGetState)
DAO_IMPORT_AS(XamInputSetState, dao::imp::XamInputSetState)
DAO_IMPORT_AS(XamNotifyCreateListener, dao::imp::XamNotifyCreateListener)
DAO_IMPORT_AS(XNotifyGetNext, dao::imp::XNotifyGetNext)
DAO_IMPORT_AS(XamEnumerate, dao::imp::XamEnumerate)
DAO_IMPORT_AS(XamCreateEnumeratorHandle, dao::imp::XamCreateEnumeratorHandle)
DAO_IMPORT_AS(XamGetPrivateEnumStructureFromHandle, dao::imp::XamGetPrivateEnumStructureFromHandle)
DAO_IMPORT_AS(XamUserCreateAchievementEnumerator, dao::imp::XamUserCreateAchievementEnumerator)
DAO_IMPORT_AS(XamContentCreateEx, dao::imp::XamContentCreateEx)
DAO_IMPORT_AS(XamContentClose, dao::imp::XamContentClose)
DAO_IMPORT_AS(XamContentCreateEnumerator, dao::imp::XamContentCreateEnumerator)
DAO_IMPORT_AS(XamContentGetCreator, dao::imp::XamContentGetCreator)
DAO_IMPORT_AS(XamContentGetDeviceState, dao::imp::XamContentGetDeviceState)
DAO_IMPORT_AS(XamContentGetDeviceData, dao::imp::XamContentGetDeviceData)
DAO_IMPORT_AS(XamShowDeviceSelectorUI, dao::imp::XamShowDeviceSelectorUI)
DAO_IMPORT_AS(XamShowSigninUI, dao::imp::XamShowSigninUI)
DAO_IMPORT_AS(XamShowMessageBoxUI, dao::imp::XamShowMessageBoxUI)
DAO_IMPORT_AS(XamShowMessageBoxUIEx, dao::imp::XamShowMessageBoxUIEx)
DAO_IMPORT_AS(XamShowDirtyDiscErrorUI, dao::imp::XamShowDirtyDiscErrorUI)
DAO_IMPORT_AS(XamShowKeyboardUI, dao::imp::XamShowKeyboardUI)
DAO_IMPORT_AS(XamShowAchievementsUI, dao::imp::XamShowAchievementsUI)
DAO_IMPORT_AS(XamShowMarketplaceUI, dao::imp::XamShowMarketplaceUI)
DAO_IMPORT_AS(XMsgInProcessCall, dao::imp::XMsgInProcessCall)
DAO_IMPORT_AS(XMsgStartIORequest, dao::imp::XMsgStartIORequest)
DAO_IMPORT_AS(XMsgStartIORequestEx, dao::imp::XMsgStartIORequestEx)
DAO_IMPORT_AS(XMsgCancelIORequest, dao::imp::XMsgCancelIORequest)
DAO_IMPORT_AS(XamVoiceHeadsetPresent, dao::imp::XamVoiceHeadsetPresent)
DAO_IMPORT_AS(XamVoiceCreate, dao::imp::XamVoiceCreate)
DAO_IMPORT_AS(XamVoiceClose, dao::imp::XamVoiceClose)
DAO_IMPORT_AS(XamVoiceSubmitPacket, dao::imp::XamVoiceSubmitPacket)
