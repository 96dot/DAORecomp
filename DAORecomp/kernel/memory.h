#pragma once
// Guest address space (4 GB, big-endian data) and the page allocators behind
// NtAllocateVirtualMemory / MmAllocatePhysicalMemoryEx.
//
// Map (follows the real console / Xenia):
//   0x00000000-0x3FFFFFFF  virtual, 4 KB pages
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages (thread stacks come from the top)
//   0x80000000-0x81FFFFFF  kernel data + kernel heap (our objects, PCRs, TLS)
//   0x82000000-...         XEX image, then the recompiled-function lookup table
//                          (its place and size depend on the build: game_build.h)
//   0xA0000000/0xC0000000/0xE0000000  three views of the same 512 MB physical memory (0xE is offset by +0x1000)
#include "game_build.h"

namespace dao::kernel
{
    struct GuestMemory
    {
        uint8_t* base = nullptr;

        bool Init();

        template<typename T = void>
        T* Translate(uint32_t address) const noexcept
        {
            return reinterpret_cast<T*>(base + address);
        }

        uint32_t ToGuest(const void* host) const noexcept
        {
            return host ? uint32_t(static_cast<const uint8_t*>(host) - base) : 0;
        }

        bool IsGuest(const void* host) const noexcept
        {
            return host >= base && host < base + PPC_MEMORY_SIZE;
        }

        // The running build's table, where its recompiled code looks up indirect
        // calls (PPC_LOOKUP_FUNC with that build's ppc_config.h): an 8-byte slot per
        // 4-byte instruction, hence * 2.
        PPCFunc** FunctionSlot(uint32_t guest) const noexcept
        {
            const DaoBuildInfo& build = g_build.info;
            return reinterpret_cast<PPCFunc**>(
                base + build.imageBase + build.imageSize + uint64_t(guest - build.codeBase) * 2);
        }

        PPCFunc* FindFunction(uint32_t guest) const noexcept
        {
            if (guest - g_build.info.codeBase >= g_build.info.codeSize)
                return nullptr;
            return *FunctionSlot(guest);
        }

        void InsertFunction(uint32_t guest, PPCFunc* host) noexcept
        {
            *FunctionSlot(guest) = host;
        }

        // Calls guest code (PPC_CALL_INDIRECT_FUNC for the runtime).
        void Call(uint32_t guest, PPCContext& ctx) const
        {
            (*FunctionSlot(guest))(ctx, base);
        }
    };

    extern GuestMemory g_memory;

    // Page-granular allocator over one guest range. All memory is host-committed up
    // front; "reserve" vs "commit" is bookkeeping, plus zeroing when a page is committed
    // again (a first commit gets fresh, zeroed host pages).
    class PageHeap
    {
    public:
        // view: the guest address that page 0 is reached at (for zeroing), when the heap's
        // addresses aren't guest addresses themselves (the physical heap's).
        void Init(const char* name, uint32_t base, uint32_t size, uint32_t pageSize, uint32_t view);

        // Returns 0 on failure.
        uint32_t Alloc(uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown,
            uint32_t low = 0, uint32_t high = 0xFFFFFFFF);
        bool AllocFixed(uint32_t address, uint32_t size, bool commit, uint32_t protect);
        bool Decommit(uint32_t address, uint32_t size);
        bool Release(uint32_t address, uint32_t* releasedSize = nullptr);

        struct RegionInfo
        {
            uint32_t baseAddress;
            uint32_t allocationBase;
            uint32_t allocationProtect;
            uint32_t regionSize;
            uint32_t state;   // X_MEM_COMMIT / X_MEM_RESERVE / X_MEM_FREE
            uint32_t protect;
        };
        bool Query(uint32_t address, RegionInfo& info);
        uint32_t QueryAllocationSize(uint32_t address);
        bool SetProtect(uint32_t address, uint32_t size, uint32_t protect);
        uint32_t QueryProtect(uint32_t address);

        bool Contains(uint32_t address) const { return address >= m_base && address - m_base < m_size; }
        uint32_t PageSize() const { return m_pageSize; }
        uint32_t Base() const { return m_base; }
        uint32_t Size() const { return m_size; }
        uint64_t BytesCommitted() const { return m_committedPages * uint64_t(m_pageSize); }

    private:
        struct Page
        {
            uint32_t allocBase = 0;   // first page index of the allocation + 1 (0 = free)
            uint32_t allocPages = 0;  // only valid on the first page
            uint16_t protect = 0;
            uint8_t state = 0;        // 0 free, 1 reserved, 2 committed
            uint8_t everCommitted = 0;
        };

        // Runs of pages (first, count) committed again, zeroed by ZeroPages once the lock is
        // released: the zeroing can hit pages the GPU watches, and its handler takes Xenia's
        // lock, which is held around calls into this heap.
        using ZeroList = std::vector<std::pair<size_t, size_t>>;

        bool RangeFree(size_t first, size_t count) const;
        void Mark(size_t first, size_t count, bool commit, uint32_t protect, ZeroList& zero);
        uint32_t AllocLocked(uint32_t size, uint32_t alignment, bool commit, uint32_t protect, bool topDown,
            uint32_t low, uint32_t high, ZeroList& zero);
        bool AllocFixedLocked(uint32_t address, uint32_t size, bool commit, uint32_t protect, ZeroList& zero);
        void Commit(size_t i, ZeroList& zero);
        void ZeroPages(const ZeroList& zero);

        std::mutex m_mutex;
        const char* m_name = "";
        uint32_t m_base = 0, m_size = 0, m_pageSize = 0, m_view = 0;
        std::vector<Page> m_pages;
        uint64_t m_committedPages = 0;
    };

    // Small-object allocator for kernel-owned guest structures (KTHREAD, PCR, TLS,
    // dispatcher headers for handle-based objects, XAM buffers, ...).
    uint32_t KernelAlloc(uint32_t size, uint32_t alignment = 16);
    void KernelFree(uint32_t address);

    PageHeap* LookupHeap(uint32_t address);
    PageHeap& PhysicalHeap(); // physical addresses 0x00000000-0x1FFFFFFF
    PageHeap& VirtualHeap4K();
    PageHeap& VirtualHeap64K();

    // Physical memory: one allocator for the shared 512 MB, viewed at 0xA/0xC/0xE.
    uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t pageSize, uint32_t protect,
        uint32_t minPhys, uint32_t maxPhys);
    bool PhysicalFree(uint32_t virtualAddress);
    uint32_t PhysicalAllocationSize(uint32_t virtualAddress);
    inline uint32_t PhysicalAddressOf(uint32_t virtualAddress)
    {
        return (virtualAddress & 0x1FFFFFFF) + (virtualAddress >= 0xE0000000 ? 0x1000 : 0);
    }
    inline bool IsPhysicalAddress(uint32_t address) { return address >= 0xA0000000; }

    void MemoryStats(uint64_t& totalCommitted, uint64_t& physicalUsed);
}
