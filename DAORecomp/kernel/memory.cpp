#include "stdafx.h"
#include "memory.h"
#include "types.h"
#include "gpu/xenia_host.h"

namespace dao::kernel
{
    GuestMemory g_memory;

    static PageHeap s_heap4K;      // 0x00000000-0x3FFFFFFF
    static PageHeap s_heap64K;     // 0x40000000-0x7EFFFFFF
    static PageHeap s_physical;    // physical 0x00000000-0x1FFFFFFF

    constexpr uint64_t kPhysicalSize = 0x20000000;
    constexpr uint32_t kKernelHeapBase = 0x80100000;
    constexpr uint32_t kKernelHeapEnd = 0x82000000;

    bool GuestMemory::Init()
    {
        // Reserve 4 GB as a placeholder so the physical views can be mapped into it.
        const HANDLE process = GetCurrentProcess();
        void* placeholder = nullptr;
        for (uint64_t hint : { 0x100000000ull, 0ull })
        {
            placeholder = VirtualAlloc2(process, (void*)hint, PPC_MEMORY_SIZE,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
            if (placeholder)
                break;
        }
        if (!placeholder)
        {
            LOGE("Failed to reserve 4 GB guest address space ({})", GetLastError());
            return false;
        }

        uint8_t* p = static_cast<uint8_t*>(placeholder);

        // Split into [0, A), [A, C), [C, E), [E, 4G).
        for (uint64_t split : { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull })
        {
            const uint64_t from = split == 0xA0000000ull ? 0 : split - 0x20000000ull;
            if (!VirtualFree(p + from, split - from, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            {
                LOGE("Failed to split guest placeholder at {:X} ({})", split, GetLastError());
                return false;
            }
        }

        if (!VirtualAlloc2(process, p, 0xA0000000ull, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE, nullptr, 0))
        {
            LOGE("Failed to commit guest virtual memory ({})", GetLastError());
            return false;
        }

        const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
            0, DWORD(kPhysicalSize), nullptr);
        if (!section)
        {
            LOGE("Failed to create physical memory section ({})", GetLastError());
            return false;
        }

        // The 0xE0000000 view is offset by one page on the console: E0000000+x is physical
        // x+0x1000 (D3D relies on this when it computes GPU addresses). Split its last
        // page off so the view can start at section offset 0x1000.
        if (!VirtualFree(p + 0xE0000000ull, kPhysicalSize - 0x1000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            LOGE("Failed to split the 0xE0000000 placeholder ({})", GetLastError());
            return false;
        }

        for (uint64_t view : { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull })
        {
            const uint64_t offset = view == 0xE0000000ull ? 0x1000 : 0;
            if (!MapViewOfFile3(section, process, p + view, offset, kPhysicalSize - offset, MEM_REPLACE_PLACEHOLDER,
                PAGE_READWRITE, nullptr, 0))
            {
                LOGE("Failed to map physical view at {:X} ({})", view, GetLastError());
                return false;
            }
        }

        base = p;

        // Null page stays inaccessible so guest null dereferences fault loudly.
        DWORD oldProtect;
        VirtualProtect(base, 0x10000, PAGE_NOACCESS, &oldProtect);

        s_heap4K.Init("v00000000", 0x00000000, 0x40000000, 0x1000, 0x00000000);
        s_heap4K.AllocFixed(0, 0x10000, false, X_PAGE_NOACCESS);
        s_heap64K.Init("v40000000", 0x40000000, 0x3F000000, 0x10000, 0x40000000);
        s_physical.Init("physical", 0x00000000, uint32_t(kPhysicalSize), 0x1000, 0xA0000000);
        s_physical.AllocFixed(0x1FFF0000, 0x10000, false, X_PAGE_NOACCESS);

        // Fill the recompiled-function lookup table.
        size_t count = 0;
        const PPCFuncMapping* mappings = g_build.info.mappings;
        for (size_t i = 0; mappings[i].guest != 0; i++)
        {
            if (mappings[i].host != nullptr)
            {
                InsertFunction(uint32_t(mappings[i].guest), mappings[i].host);
                count++;
            }
        }

        LOGI("Guest memory at {}, {} recompiled functions mapped", (void*)base, count);
        return true;
    }

    // ---------------------------------------------------------------- PageHeap

    void PageHeap::Init(const char* name, uint32_t base, uint32_t size, uint32_t pageSize, uint32_t view)
    {
        m_name = name;
        m_base = base;
        m_size = size;
        m_pageSize = pageSize;
        m_view = view;
        m_pages.assign(size / pageSize, Page{});
    }

    void PageHeap::Commit(size_t i, ZeroList& zero)
    {
        auto& page = m_pages[i];
        if (page.state == 2)
            return;
        m_committedPages++;
        if (page.everCommitted)
        {
            if (!zero.empty() && zero.back().first + zero.back().second == i)
                zero.back().second++;
            else
                zero.push_back({ i, 1 });
        }
        page.everCommitted = 1;
        page.state = 2;
    }

    void PageHeap::ZeroPages(const ZeroList& zero)
    {
        for (const auto& [first, count] : zero)
        {
            const uint32_t address = m_view + uint32_t(first * m_pageSize);
            const uint32_t length = uint32_t(count * m_pageSize);
            dao::gpu::PrepareGuestWrite(address, length);
            memset(g_memory.Translate(address), 0, length);
        }
    }

    bool PageHeap::RangeFree(size_t first, size_t count) const
    {
        for (size_t i = first; i < first + count; i++)
            if (m_pages[i].state != 0)
                return false;
        return true;
    }

    void PageHeap::Mark(size_t first, size_t count, bool commit, uint32_t protect, ZeroList& zero)
    {
        for (size_t i = first; i < first + count; i++)
        {
            auto& page = m_pages[i];
            page.allocBase = uint32_t(first + 1);
            page.allocPages = i == first ? uint32_t(count) : 0;
            page.protect = uint16_t(protect);
            if (commit)
                Commit(i, zero);
            else if (page.state == 0)
            {
                page.state = 1;
            }
        }
    }

    uint32_t PageHeap::Alloc(uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown,
        uint32_t low, uint32_t high)
    {
        if (size == 0)
            return 0;

        ZeroList zero;
        const uint32_t address = AllocLocked(size, alignment, commit, protect, topDown, low, high, zero);
        ZeroPages(zero);
        return address;
    }

    uint32_t PageHeap::AllocLocked(uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown,
        uint32_t low, uint32_t high, ZeroList& zero)
    {
        std::lock_guard lock(m_mutex);
        const size_t count = (size_t(size) + m_pageSize - 1) / m_pageSize;
        const size_t align = std::max<size_t>(1, alignment / m_pageSize);
        const size_t total = m_pages.size();

        size_t lowPage = low > m_base ? (size_t(low) - m_base) / m_pageSize : 0;
        size_t highPage = high >= m_base + uint64_t(m_size) - 1 ? total : (size_t(high) - m_base + 1) / m_pageSize;
        lowPage = (lowPage + align - 1) / align * align;
        if (highPage < count || lowPage > highPage - count)
            return 0;

        auto tryAt = [&](size_t first) -> bool
            {
                if (!RangeFree(first, count))
                    return false;
                Mark(first, count, commit, protect, zero);
                return true;
            };

        if (topDown)
        {
            for (size_t first = (highPage - count) / align * align; ; first -= align)
            {
                if (tryAt(first))
                    return m_base + uint32_t(first * m_pageSize);
                if (first < lowPage + align)
                    break;
            }
        }
        else
        {
            for (size_t first = lowPage; first + count <= highPage; first += align)
            {
                // Skip quickly over used runs.
                if (m_pages[first].state != 0)
                    continue;
                if (tryAt(first))
                    return m_base + uint32_t(first * m_pageSize);
            }
        }
        return 0;
    }

    bool PageHeap::AllocFixed(uint32_t address, uint32_t size, bool commit, uint32_t protect)
    {
        ZeroList zero;
        const bool ok = AllocFixedLocked(address, size, commit, protect, zero);
        ZeroPages(zero);
        return ok;
    }

    bool PageHeap::AllocFixedLocked(uint32_t address, uint32_t size, bool commit, uint32_t protect, ZeroList& zero)
    {
        std::lock_guard lock(m_mutex);
        const size_t first = (address - m_base) / m_pageSize;
        const size_t last = (size_t(address) + size - 1 - m_base) / m_pageSize;
        if (last >= m_pages.size())
            return false;

        // Committing inside an existing reservation is allowed; overlapping a
        // different allocation is not.
        const uint32_t existing = m_pages[first].allocBase;
        for (size_t i = first; i <= last; i++)
        {
            if (m_pages[i].state != 0 && m_pages[i].allocBase != existing)
                return false;
        }

        if (existing != 0)
        {
            // Within an existing allocation: just change state/protection.
            for (size_t i = first; i <= last; i++)
            {
                if (commit)
                    Commit(i, zero);
                m_pages[i].protect = uint16_t(protect);
            }
            return true;
        }

        Mark(first, last - first + 1, commit, protect, zero);
        return true;
    }

    bool PageHeap::Decommit(uint32_t address, uint32_t size)
    {
        std::lock_guard lock(m_mutex);
        const size_t first = (address - m_base) / m_pageSize;
        const size_t last = (size_t(address) + std::max<uint32_t>(size, 1) - 1 - m_base) / m_pageSize;
        for (size_t i = first; i <= last && i < m_pages.size(); i++)
        {
            if (m_pages[i].state == 2)
            {
                m_pages[i].state = 1;
                m_committedPages--;
            }
        }
        return true;
    }

    bool PageHeap::Release(uint32_t address, uint32_t* releasedSize)
    {
        std::lock_guard lock(m_mutex);
        const size_t index = (address - m_base) / m_pageSize;
        if (index >= m_pages.size() || m_pages[index].allocBase == 0)
            return false;

        const size_t first = m_pages[index].allocBase - 1;
        const size_t count = m_pages[first].allocPages;
        for (size_t i = first; i < first + count; i++)
        {
            if (m_pages[i].state == 2)
                m_committedPages--;
            const uint8_t ever = m_pages[i].everCommitted;
            m_pages[i] = Page{};
            m_pages[i].everCommitted = ever;
        }
        if (releasedSize)
            *releasedSize = uint32_t(count * m_pageSize);
        return true;
    }

    bool PageHeap::Query(uint32_t address, RegionInfo& info)
    {
        std::lock_guard lock(m_mutex);
        const size_t index = (address - m_base) / m_pageSize;
        if (index >= m_pages.size())
            return false;

        const auto& page = m_pages[index];
        info.baseAddress = m_base + uint32_t(index * m_pageSize);
        info.state = page.state == 2 ? X_MEM_COMMIT : page.state == 1 ? X_MEM_RESERVE : X_MEM_FREE;
        info.protect = page.state == 2 ? page.protect : 0;
        if (page.allocBase)
        {
            const size_t first = page.allocBase - 1;
            info.allocationBase = m_base + uint32_t(first * m_pageSize);
            info.allocationProtect = m_pages[first].protect;
        }
        else
        {
            info.allocationBase = 0;
            info.allocationProtect = 0;
        }

        // Extend over pages with identical state/protection/allocation.
        size_t end = index + 1;
        while (end < m_pages.size() && m_pages[end].state == page.state &&
            m_pages[end].allocBase == page.allocBase && m_pages[end].protect == page.protect)
            end++;
        info.regionSize = uint32_t((end - index) * m_pageSize);
        return true;
    }

    uint32_t PageHeap::QueryAllocationSize(uint32_t address)
    {
        std::lock_guard lock(m_mutex);
        const size_t index = (address - m_base) / m_pageSize;
        if (index >= m_pages.size() || !m_pages[index].allocBase)
            return 0;
        return uint32_t(m_pages[m_pages[index].allocBase - 1].allocPages * m_pageSize);
    }

    bool PageHeap::SetProtect(uint32_t address, uint32_t size, uint32_t protect)
    {
        std::lock_guard lock(m_mutex);
        const size_t first = (address - m_base) / m_pageSize;
        const size_t last = (size_t(address) + std::max<uint32_t>(size, 1) - 1 - m_base) / m_pageSize;
        for (size_t i = first; i <= last && i < m_pages.size(); i++)
            m_pages[i].protect = uint16_t(protect);
        return true;
    }

    uint32_t PageHeap::QueryProtect(uint32_t address)
    {
        std::lock_guard lock(m_mutex);
        const size_t index = (address - m_base) / m_pageSize;
        if (index >= m_pages.size() || m_pages[index].state != 2)
            return 0;
        return m_pages[index].protect;
    }

    // ------------------------------------------------------------ kernel heap

    static std::mutex s_kernelMutex;
    static std::map<uint32_t, uint32_t> s_kernelFree{ { kKernelHeapBase, kKernelHeapEnd - kKernelHeapBase } };
    static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> s_kernelUsed; // addr -> (block, size)

    uint32_t KernelAlloc(uint32_t size, uint32_t alignment)
    {
        size = (std::max<uint32_t>(size, 1) + 15) & ~15u;
        alignment = std::max<uint32_t>(alignment, 16);

        std::lock_guard lock(s_kernelMutex);
        for (auto it = s_kernelFree.begin(); it != s_kernelFree.end(); ++it)
        {
            const uint32_t blockStart = it->first;
            const uint32_t blockSize = it->second;
            const uint32_t aligned = (blockStart + alignment - 1) & ~(alignment - 1);
            const uint32_t padding = aligned - blockStart;
            if (blockSize < padding + size)
                continue;

            s_kernelFree.erase(it);
            const uint32_t used = padding + size;
            if (blockSize > used)
                s_kernelFree.emplace(blockStart + used, blockSize - used);
            s_kernelUsed[aligned] = { blockStart, used };
            memset(g_memory.Translate(aligned), 0, size);
            return aligned;
        }

        LOGE("Kernel heap exhausted (request {:X})", size);
        return 0;
    }

    void KernelFree(uint32_t address)
    {
        if (!address)
            return;

        std::lock_guard lock(s_kernelMutex);
        auto used = s_kernelUsed.find(address);
        if (used == s_kernelUsed.end())
            return;

        uint32_t start = used->second.first;
        uint32_t size = used->second.second;
        s_kernelUsed.erase(used);

        // Coalesce with neighbours.
        auto next = s_kernelFree.lower_bound(start);
        if (next != s_kernelFree.end() && next->first == start + size)
        {
            size += next->second;
            next = s_kernelFree.erase(next);
        }
        if (next != s_kernelFree.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                prev->second += size;
                return;
            }
        }
        s_kernelFree.emplace(start, size);
    }

    // ------------------------------------------------------- heap lookup etc.

    PageHeap* LookupHeap(uint32_t address)
    {
        if (s_heap4K.Contains(address))
            return &s_heap4K;
        if (s_heap64K.Contains(address))
            return &s_heap64K;
        return nullptr;
    }

    PageHeap& VirtualHeap4K() { return s_heap4K; }
    PageHeap& PhysicalHeap() { return s_physical; }
    PageHeap& VirtualHeap64K() { return s_heap64K; }

    static uint32_t ViewBaseForPageSize(uint32_t pageSize)
    {
        if (pageSize >= 0x1000000)
            return 0xC0000000;
        if (pageSize >= 0x10000)
            return 0xA0000000;
        return 0xE0000000;
    }

    uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t pageSize, uint32_t protect,
        uint32_t minPhys, uint32_t maxPhys)
    {
        const uint32_t align = std::max(alignment, pageSize);
        const uint32_t rounded = (size + pageSize - 1) & ~(pageSize - 1);
        if (!rounded)
            return 0; // nothing asked for, or a size that wraps
        const uint32_t viewBase = ViewBaseForPageSize(pageSize);
        if (viewBase == 0xE0000000)
            minPhys = std::max<uint32_t>(minPhys, 0x1000); // physical page 0 has no 0xE alias
        const uint32_t phys = s_physical.Alloc(rounded, align, true, protect, true, minPhys,
            std::min<uint32_t>(maxPhys, uint32_t(kPhysicalSize - 1)));
        if (!phys)
            return 0;
        return viewBase == 0xE0000000 ? viewBase + phys - 0x1000 : viewBase + phys;
    }

    bool PhysicalFree(uint32_t virtualAddress)
    {
        return s_physical.Release(PhysicalAddressOf(virtualAddress));
    }

    uint32_t PhysicalAllocationSize(uint32_t virtualAddress)
    {
        return s_physical.QueryAllocationSize(PhysicalAddressOf(virtualAddress));
    }

    void MemoryStats(uint64_t& totalCommitted, uint64_t& physicalUsed)
    {
        totalCommitted = s_heap4K.BytesCommitted() + s_heap64K.BytesCommitted();
        physicalUsed = s_physical.BytesCommitted();
    }
}

extern "C" void* MmGetHostAddress(uint32_t ptr)
{
    return dao::kernel::g_memory.Translate(ptr);
}
