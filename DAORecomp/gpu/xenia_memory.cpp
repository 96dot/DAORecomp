// xe::Memory for Xenia's GPU (see xenia/src/xenia/memory.h), on top of the
// runtime's guest memory. The interesting part is write watches: the GPU caches
// guest data (vertex/index buffers, textures, shared memory) and asks to be told
// when the guest overwrites it. Watched physical pages are made read-only in all
// three guest views; the first write faults into HandleAccessViolation, which
// fires the GPU's invalidation callbacks and makes the page writable again.
#include "stdafx.h"
#include "kernel/memory.h"
#include "kernel/types.h"

#include "xenia/memory.h"

using dao::kernel::g_memory;

namespace xe
{
    namespace
    {
        constexpr uint32_t kPageShift = 12;
        constexpr uint32_t kPageSize = 1u << kPageShift;
        constexpr uint32_t kPhysicalSize = 0x20000000;
        constexpr uint32_t kPageCount = kPhysicalSize >> kPageShift;

        uint32_t ToXeniaProtect(uint32_t xprotect)
        {
            using namespace dao::kernel;
            uint32_t result = 0;
            if (xprotect & (X_PAGE_READONLY | X_PAGE_EXECUTE_READ))
                result |= kMemoryProtectRead;
            if (xprotect & (X_PAGE_READWRITE | X_PAGE_EXECUTE_READWRITE | X_PAGE_WRITECOPY))
                result |= kMemoryProtectRead | kMemoryProtectWrite;
            if (xprotect & X_PAGE_NOCACHE)
                result |= kMemoryProtectNoCache;
            if (xprotect & X_PAGE_WRITECOMBINE)
                result |= kMemoryProtectWriteCombine;
            return result;
        }

        class PhysicalHeapView final : public VirtualHeap
        {
        public:
            bool QueryRegionInfo(uint32_t address, HeapAllocationInfo* out) override
            {
                dao::kernel::PageHeap::RegionInfo info;
                if (!dao::kernel::PhysicalHeap().Query(address & 0x1FFFFFFF, info))
                    return false;
                out->base_address = info.baseAddress;
                out->allocation_base = info.allocationBase;
                out->allocation_protect = ToXeniaProtect(info.allocationProtect);
                out->allocation_size = 0;
                out->region_size = info.regionSize;
                out->state = info.state == dao::kernel::X_MEM_COMMIT
                    ? (kMemoryAllocationReserve | kMemoryAllocationCommit)
                    : info.state == dao::kernel::X_MEM_RESERVE ? kMemoryAllocationReserve : 0;
                out->protect = ToXeniaProtect(info.protect);
                return true;
            }

            xe::memory::PageAccess QueryRangeAccess(uint32_t low, uint32_t high) override
            {
                auto access = xe::memory::PageAccess::kReadWrite;
                for (uint32_t page = low >> kPageShift; page <= (high >> kPageShift); page++)
                {
                    const auto pageAccess =
                        ToPageAccess(ToXeniaProtect(dao::kernel::PhysicalHeap().QueryProtect(page << kPageShift)));
                    if (pageAccess == xe::memory::PageAccess::kNoAccess)
                        return pageAccess;
                    if (pageAccess == xe::memory::PageAccess::kReadOnly)
                        access = pageAccess;
                }
                return access;
            }
        };
    }

    struct Memory::State
    {
        std::vector<uint64_t> watched = std::vector<uint64_t>(kPageCount / 64);
        std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*> callbacks;
        PhysicalHeapView physicalHeap;

        bool IsWatched(uint32_t page) const { return (watched[page >> 6] >> (page & 63)) & 1; }
        void SetWatched(uint32_t page, bool value)
        {
            if (value)
                watched[page >> 6] |= uint64_t(1) << (page & 63);
            else
                watched[page >> 6] &= ~(uint64_t(1) << (page & 63));
        }
    };

    static Memory* s_instance = nullptr;

    // Sets the host protection of physical pages [first, first + count) in all views.
    static void ProtectPhysicalPages(uint32_t first, uint32_t count, bool readOnly)
    {
        const DWORD protect = readOnly ? PAGE_READONLY : PAGE_READWRITE;
        const size_t bytes = size_t(count) << kPageShift;
        const uint32_t offset = first << kPageShift;
        DWORD old;
        VirtualProtect(g_memory.Translate(0xA0000000 + offset), bytes, protect, &old);
        VirtualProtect(g_memory.Translate(0xC0000000 + offset), bytes, protect, &old);
        // 0xE0000000 + x is physical x + 0x1000; physical page 0 has no alias there.
        if (first == 0)
        {
            if (count > 1)
                VirtualProtect(g_memory.Translate(0xE0000000), bytes - kPageSize, protect, &old);
        }
        else
        {
            VirtualProtect(g_memory.Translate(0xE0000000 + offset - kPageSize), bytes, protect, &old);
        }
    }

    Memory::Memory() : state_(std::make_unique<State>())
    {
        virtual_membase_ = g_memory.base;
        physical_membase_ = g_memory.base + 0xA0000000;
        s_instance = this;
    }

    Memory::~Memory()
    {
        s_instance = nullptr;
    }

    uint32_t Memory::HostToGuestVirtual(const void* host_address) const
    {
        return g_memory.ToGuest(host_address);
    }

    uint32_t Memory::GetPhysicalAddress(uint32_t address) const
    {
        if (address < 0xA0000000)
            return UINT32_MAX;
        return dao::kernel::PhysicalAddressOf(address);
    }

    void* Memory::RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
        void* callback_context)
    {
        auto lock = global_critical_region_.Acquire();
        auto* entry = new std::pair<PhysicalMemoryInvalidationCallback, void*>(callback, callback_context);
        state_->callbacks.push_back(entry);
        return entry;
    }

    void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle)
    {
        auto lock = global_critical_region_.Acquire();
        auto* entry = static_cast<std::pair<PhysicalMemoryInvalidationCallback, void*>*>(callback_handle);
        std::erase(state_->callbacks, entry);
        delete entry;
    }

    void* Memory::RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback, void*)
    {
        return nullptr;
    }

    void Memory::UnregisterPhysicalMemoryReadCallback(void*) {}

    void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
        bool enable_invalidation_notifications, bool enable_data_providers)
    {
        if (!enable_invalidation_notifications || !length)
            return;
        physical_address &= 0x1FFFFFFF;
        const uint32_t first = physical_address >> kPageShift;
        const uint32_t last = std::min<uint32_t>((physical_address + length - 1) >> kPageShift, kPageCount - 1);

        auto lock = global_critical_region_.Acquire();
        uint32_t runStart = UINT32_MAX;
        for (uint32_t page = first; page <= last + 1; page++)
        {
            bool arm = false;
            if (page <= last && !state_->IsWatched(page))
            {
                // Only writable committed pages can be written, so only they need a watch.
                const uint32_t protect = dao::kernel::PhysicalHeap().QueryProtect(page << kPageShift);
                arm = IsWritableProtect(ToXeniaProtect(protect));
            }
            if (arm)
            {
                state_->SetWatched(page, true);
                if (runStart == UINT32_MAX)
                    runStart = page;
            }
            else if (runStart != UINT32_MAX)
            {
                ProtectPhysicalPages(runStart, page - runStart, true);
                runStart = UINT32_MAX;
            }
        }
    }

    bool Memory::TriggerPhysicalMemoryCallbacks(global_unique_lock_type global_lock_locked_once,
        uint32_t virtual_address, uint32_t length, bool is_write, bool unwatch_exact_range, bool unprotect)
    {
        if (!length)
            return false;

        uint32_t physical;
        if (virtual_address >= 0xA0000000)
            physical = dao::kernel::PhysicalAddressOf(virtual_address);
        else
            return false; // only the physical views are ever watched

        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((physical + length - 1) >> kPageShift, kPageCount - 1);

        bool anyWatched = false;
        for (uint32_t page = first; page <= last && !anyWatched; page++)
            anyWatched = state_->IsWatched(page);
        if (!anyWatched)
            return false;

        const uint32_t start = first << kPageShift;
        const uint32_t size = (last - first + 1) << kPageShift;
        // Without unprotecting there's nothing to save by widening: the callbacks are
        // told to invalidate exactly the range.
        const bool exact = unwatch_exact_range || !unprotect;
        // Each callback returns the range it holds nothing in any more, as (first byte,
        // length): a fault widens its invalidation (the shared memory's up to 256 KB, the
        // primitive processor's to its buckets). Their common part is unwatched, within
        // a 4 MB window around the touched pages, as Xenia's PhysicalHeap::TriggerCallbacks
        // does: re-uploading a little extra costs less than a fault (and three
        // VirtualProtect calls) for each further page the game writes.
        uint32_t commonFirst = 0, commonLast = UINT32_MAX; // bytes, inclusive
        for (auto* callback : state_->callbacks)
        {
            const auto [rangeStart, rangeLength] = callback->first(callback->second, start, size, exact);
            if (exact)
                continue;
            const uint32_t rangeLast = rangeStart + std::min(std::max(rangeLength, 1u) - 1, UINT32_MAX - rangeStart);
            commonFirst = std::max(commonFirst, rangeStart);
            commonLast = std::min(commonLast, rangeLast);
        }
        uint32_t unwatchFirst = first, unwatchLast = last;
        if (!exact)
        {
            constexpr uint32_t kMaxExcess = 4u << 20;
            const uint32_t endByte = start + size - 1;
            // At least the touched pages, at most their 4 MB block, never past the end.
            const uint32_t from = std::max(std::min(commonFirst, start), start & ~(kMaxExcess - 1));
            const uint32_t to = std::min({ std::max(commonLast, endByte), endByte | (kMaxExcess - 1), kPhysicalSize - 1 });
            unwatchFirst = from >> kPageShift;
            unwatchLast = to >> kPageShift;
        }

        // Only pages that are watched (armed because they were writable) are made
        // writable again.
        if (unprotect)
        {
            uint32_t runStart = UINT32_MAX;
            for (uint32_t page = unwatchFirst; page <= unwatchLast + 1; page++)
            {
                const bool watched = page <= unwatchLast && state_->IsWatched(page);
                if (watched)
                {
                    state_->SetWatched(page, false);
                    if (runStart == UINT32_MAX)
                        runStart = page;
                }
                else if (runStart != UINT32_MAX)
                {
                    ProtectPhysicalPages(runStart, page - runStart, false);
                    runStart = UINT32_MAX;
                }
            }
        }
        return true;
    }

    VirtualHeap* Memory::GetPhysicalHeap()
    {
        return &state_->physicalHeap;
    }

    BaseHeap* Memory::LookupHeapByType(bool physical, uint32_t)
    {
        return physical ? &state_->physicalHeap : nullptr;
    }

    bool Memory::HandleAccessViolation(void* host_address, bool is_write)
    {
        if (!is_write || !g_memory.IsGuest(host_address))
            return false;
        const uint32_t address = g_memory.ToGuest(host_address);
        if (address < 0xA0000000)
            return false;
        auto lock = global_critical_region_.Acquire();
        if (TriggerPhysicalMemoryCallbacks(std::move(lock), address, 1, true, false))
            return true;
        // Another thread may have faulted on the same watched page and unprotected
        // it while this one waited for the lock: retry if the page is writable now.
        MEMORY_BASIC_INFORMATION info;
        return VirtualQuery(host_address, &info, sizeof(info)) && (info.Protect & PAGE_READWRITE);
    }
}

namespace dao::gpu
{
    bool HandleWatchedWrite(void* hostAddress, bool isWrite)
    {
        return xe::s_instance && xe::s_instance->HandleAccessViolation(hostAddress, isWrite);
    }

    void PrepareGuestWrite(uint32_t address, uint32_t length)
    {
        if (!xe::s_instance || address < 0xA0000000 || !length)
            return;
        auto lock = xe::global_critical_region::Acquire();
        xe::s_instance->TriggerPhysicalMemoryCallbacks(std::move(lock), address, length, true, true);
    }

    void InvalidateAllGpuCopies()
    {
        if (!xe::s_instance)
            return;
        // Watches and protection stay as they are: only the GPU's copies are dropped,
        // so everything is reloaded from guest memory on its next use.
        auto lock = xe::global_critical_region::Acquire();
        xe::s_instance->TriggerPhysicalMemoryCallbacks(std::move(lock), 0xA0000000, xe::kPhysicalSize, true, true,
            false);
    }
}
