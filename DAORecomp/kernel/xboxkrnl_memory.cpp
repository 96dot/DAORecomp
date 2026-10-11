// xboxkrnl: virtual and physical memory.
#include "stdafx.h"
#include "function.h"

namespace dao::imp
{
    using namespace dao::kernel;

    static bool IsVirtualHeap(PageHeap* heap)
    {
        return heap == &VirtualHeap4K() || heap == &VirtualHeap64K();
    }

    uint32_t NtAllocateVirtualMemory(u32* baseInOut, u32* sizeInOut, uint32_t allocType, uint32_t protect,
        uint32_t debugMemory)
    {
        if (!baseInOut || !sizeInOut || !*sizeInOut)
            return X_STATUS_INVALID_PARAMETER;
        if (!(allocType & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE)))
            return X_STATUS_INVALID_PARAMETER;
        if (allocType & X_MEM_RESET)
            return X_STATUS_SUCCESS; // contents may be discarded; keeping them is valid

        uint32_t base = *baseInOut;
        int32_t requested = int32_t(uint32_t(*sizeInOut));
        uint32_t size = requested < 0 ? uint32_t(-requested) : uint32_t(requested);
        const bool commit = (allocType & X_MEM_COMMIT) != 0;

        PageHeap* heap;
        if (base)
        {
            heap = LookupHeap(base);
            if (!heap || !IsVirtualHeap(heap))
                return X_STATUS_INVALID_PARAMETER;
            const uint32_t page = heap->PageSize();
            const uint32_t end = (base + size + page - 1) & ~(page - 1);
            base &= ~(page - 1);
            size = end - base;
            if (!heap->AllocFixed(base, size, commit, protect))
            {
                LOGW("NtAllocateVirtualMemory: fixed {:08X}+{:#x} failed", base, size);
                return X_STATUS_NO_MEMORY;
            }
        }
        else
        {
            heap = (allocType & X_MEM_LARGE_PAGES) ? &VirtualHeap64K() : &VirtualHeap4K();
            size = (size + 0xFFFF) & ~0xFFFFu;
            base = heap->Alloc(size, 0x10000, commit, protect, (allocType & X_MEM_TOP_DOWN) != 0);
            if (!base)
            {
                LOGW("NtAllocateVirtualMemory: out of memory for {:#x} bytes", size);
                return X_STATUS_NO_MEMORY;
            }
        }

        LOGT("NtAllocateVirtualMemory {:08X}+{:#x} type {:#x} protect {:#x}", base, size, allocType, protect);
        *baseInOut = base;
        *sizeInOut = size;
        return X_STATUS_SUCCESS;
    }

    uint32_t NtFreeVirtualMemory(u32* baseInOut, u32* sizeInOut, uint32_t freeType, uint32_t debugMemory)
    {
        if (!baseInOut || !*baseInOut)
            return X_STATUS_INVALID_PARAMETER;
        const uint32_t base = *baseInOut;
        PageHeap* heap = LookupHeap(base);
        if (!heap || !IsVirtualHeap(heap))
            return X_STATUS_INVALID_PARAMETER;

        uint32_t size = sizeInOut ? uint32_t(*sizeInOut) : 0;
        bool ok;
        if (freeType == X_MEM_DECOMMIT)
        {
            size = (size + heap->PageSize() - 1) & ~(heap->PageSize() - 1);
            ok = heap->Decommit(base, size);
        }
        else
        {
            ok = heap->Release(base, &size);
        }
        if (!ok)
            return X_STATUS_UNSUCCESSFUL;
        if (sizeInOut)
            *sizeInOut = size;
        return X_STATUS_SUCCESS;
    }

    struct X_MEMORY_BASIC_INFORMATION
    {
        u32 baseAddress;
        u32 allocationBase;
        u32 allocationProtect;
        u32 regionSize;
        u32 state;
        u32 protect;
        u32 type;
    };

    uint32_t NtQueryVirtualMemory(uint32_t address, X_MEMORY_BASIC_INFORMATION* info, uint32_t regionType)
    {
        PageHeap* heap = LookupHeap(address);
        PageHeap::RegionInfo region;
        if (!heap || !info || !heap->Query(address, region))
            return X_STATUS_INVALID_PARAMETER;
        info->baseAddress = region.baseAddress;
        info->allocationBase = region.allocationBase;
        info->allocationProtect = region.allocationProtect;
        info->regionSize = region.regionSize;
        info->state = region.state;
        info->protect = region.protect;
        info->type = X_MEM_PRIVATE;
        return X_STATUS_SUCCESS;
    }

    uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect, uint32_t minAddress,
        uint32_t maxAddress, uint32_t alignment)
    {
        uint32_t pageSize = 0x1000;
        if (protect & X_MEM_LARGE_PAGES)
            pageSize = 0x10000;
        else if (protect & X_MEM_16MB_PAGES)
            pageSize = 0x1000000;

        const uint32_t address = PhysicalAlloc(size, std::max(alignment, pageSize), pageSize, protect & 0xFFFF,
            minAddress, maxAddress ? maxAddress : 0xFFFFFFFF);
        LOGT("MmAllocatePhysicalMemoryEx({:#x}, {:#x}, {:#x}, {:08X}-{:08X}, align {:#x}) = {:08X}",
            flags, size, protect, minAddress, maxAddress, alignment, address);
        if (!address)
            LOGE("MmAllocatePhysicalMemoryEx: out of physical memory ({:#x} bytes)", size);
        return address;
    }

    void MmFreePhysicalMemory(uint32_t type, uint32_t address)
    {
        PhysicalFree(address);
    }

    uint32_t MmQueryAddressProtect(uint32_t address)
    {
        if (IsPhysicalAddress(address))
            return X_PAGE_READWRITE;
        PageHeap* heap = LookupHeap(address);
        if (!heap)
            return (address - g_build.info.imageBase < g_build.info.imageSize) ? X_PAGE_READWRITE : 0;
        return heap->QueryProtect(address);
    }

    void MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protect)
    {
        if (PageHeap* heap = LookupHeap(address))
            heap->SetProtect(address, size, protect & 0xFFFF);
    }

    uint32_t MmQueryAllocationSize(uint32_t address)
    {
        if (IsPhysicalAddress(address))
            return PhysicalAllocationSize(address);
        PageHeap* heap = LookupHeap(address);
        return heap ? heap->QueryAllocationSize(address) : 0;
    }

    struct X_MM_QUERY_STATISTICS_SECTION
    {
        u32 availablePages;
        u32 totalVirtualMemoryBytes;
        u32 reservedVirtualMemoryBytes;
        u32 physicalPages;
        u32 poolPages;
        u32 stackPages;
        u32 imagePages;
        u32 heapPages;
        u32 virtualPages;
        u32 pageTablePages;
        u32 cachePages;
    };

    struct X_MM_QUERY_STATISTICS_RESULT
    {
        u32 size;
        u32 totalPhysicalPages;
        u32 kernelPages;
        X_MM_QUERY_STATISTICS_SECTION title;
        X_MM_QUERY_STATISTICS_SECTION system;
        u32 highestPhysicalPage;
    };
    static_assert(sizeof(X_MM_QUERY_STATISTICS_RESULT) == 104);

    uint32_t MmQueryStatistics(X_MM_QUERY_STATISTICS_RESULT* stats)
    {
        if (!stats)
            return X_STATUS_INVALID_PARAMETER;
        if (stats->size != sizeof(X_MM_QUERY_STATISTICS_RESULT))
            return X_STATUS_BUFFER_TOO_SMALL;

        uint64_t committed, physicalUsed;
        MemoryStats(committed, physicalUsed);

        memset(stats, 0, sizeof(*stats));
        stats->size = sizeof(X_MM_QUERY_STATISTICS_RESULT);
        stats->totalPhysicalPages = 0x20000;
        stats->kernelPages = 0x100;
        const uint32_t usedPages = uint32_t((physicalUsed + committed) / 0x1000);
        stats->title.availablePages = usedPages < 0x20000 - 0x100 ? 0x20000 - 0x100 - usedPages : 0;
        stats->title.totalVirtualMemoryBytes = 0x2FFE0000;
        stats->title.reservedVirtualMemoryBytes = uint32_t(committed);
        stats->title.physicalPages = uint32_t(physicalUsed / 0x1000);
        stats->title.poolPages = 0x10;
        stats->title.stackPages = 0x100;
        stats->title.imagePages = g_build.info.imageSize / 0x1000;
        stats->title.heapPages = 0x100;
        stats->title.virtualPages = uint32_t(committed / 0x1000);
        stats->title.pageTablePages = 0x100;
        stats->title.cachePages = 0x100;
        stats->highestPhysicalPage = 0x1FFFF;
        return X_STATUS_SUCCESS;
    }

    uint32_t MmGetPhysicalAddress(uint32_t address)
    {
        return PhysicalAddressOf(address);
    }

    // Only used for XMA context registers on the console; returning the source
    // address is enough because the XMA layer reads contexts from guest memory.
    uint32_t MmMapIoSpace(uint32_t unk, uint32_t address, uint32_t size, uint32_t flags)
    {
        LOGI("MmMapIoSpace({}, {:08X}, {:#x}, {:#x})", unk, address, size, flags);
        return address;
    }
}

DAO_IMPORT_AS(NtAllocateVirtualMemory, dao::imp::NtAllocateVirtualMemory)
DAO_IMPORT_AS(NtFreeVirtualMemory, dao::imp::NtFreeVirtualMemory)
DAO_IMPORT_AS(NtQueryVirtualMemory, dao::imp::NtQueryVirtualMemory)
DAO_IMPORT_AS(MmAllocatePhysicalMemoryEx, dao::imp::MmAllocatePhysicalMemoryEx)
DAO_IMPORT_AS(MmFreePhysicalMemory, dao::imp::MmFreePhysicalMemory)
DAO_IMPORT_AS(MmQueryAddressProtect, dao::imp::MmQueryAddressProtect)
DAO_IMPORT_AS(MmSetAddressProtect, dao::imp::MmSetAddressProtect)
DAO_IMPORT_AS(MmQueryAllocationSize, dao::imp::MmQueryAllocationSize)
DAO_IMPORT_AS(MmQueryStatistics, dao::imp::MmQueryStatistics)
DAO_IMPORT_AS(MmGetPhysicalAddress, dao::imp::MmGetPhysicalAddress)
DAO_IMPORT_AS(MmMapIoSpace, dao::imp::MmMapIoSpace)
