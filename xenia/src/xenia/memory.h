/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

/**
 * DAO recomp: replacement for Xenia's xe::Memory.
 *
 * Guest memory belongs to the DAO runtime (DAORecomp/kernel/memory.*): one 4 GB
 * host reservation where guest address A lives at membase + A, with the three
 * physical views (0xA0000000, 0xC0000000, 0xE0000000 at +0x1000) mapped onto one
 * 512 MB section. This class exposes the subset of the Xenia Memory API the GPU
 * uses, implemented in DAORecomp/gpu/xenia_memory.cpp: address translation, the
 * physical heap queries, and write watches on physical pages (used to invalidate
 * GPU copies of guest data).
 */
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"

namespace xe {

enum class HeapType : uint8_t {
  kGuestVirtual,
  kGuestXex,
  kGuestPhysical,
  kHostPhysical,
};

enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

inline bool IsWritableProtect(uint32_t protect) {
  return (protect & kMemoryProtectWrite) ||
         (protect & kMemoryProtectWriteCombine);
}

inline xe::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable = IsWritableProtect(protect);
  if ((protect & kMemoryProtectRead) && !is_writable) {
    return xe::memory::PageAccess::kReadOnly;
  } else if ((protect & kMemoryProtectRead) && is_writable) {
    return xe::memory::PageAccess::kReadWrite;
  }
  return xe::memory::PageAccess::kNoAccess;
}

// Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct.
struct HeapAllocationInfo {
  uint32_t base_address;
  uint32_t allocation_base;
  uint32_t allocation_protect;
  uint32_t allocation_size;
  uint32_t region_size;
  uint32_t state;
  uint32_t protect;
};

// Query-only view of a guest heap.
class BaseHeap {
 public:
  virtual ~BaseHeap() = default;

  virtual bool QueryRegionInfo(uint32_t base_address,
                               HeapAllocationInfo* out_info) = 0;
  // The strictest access of the pages in [low_address, high_address].
  virtual xe::memory::PageAccess QueryRangeAccess(uint32_t low_address,
                                                  uint32_t high_address) = 0;
};

// The physical address space (0 - 0x1FFFFFFF).
class VirtualHeap : public BaseHeap {};

class Memory {
 public:
  Memory();
  ~Memory();

  inline uint8_t* virtual_membase() const { return virtual_membase_; }

  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + guest_address);
  }
  template <typename T>
  inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address) const {
    return TranslateVirtual<xe::be<T>*>(guest_address);
  }

  // The 0xA0000000 view, which maps physical address 0 at offset 0.
  inline uint8_t* physical_membase() const { return physical_membase_; }

  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ +
                               (guest_address & 0x1FFFFFFF));
  }

  uint32_t HostToGuestVirtual(const void* host_address) const;

  // Physical address for a virtual address in one of the physical views, or
  // UINT32_MAX.
  uint32_t GetPhysicalAddress(uint32_t address) const;

  // Returns start and length of the smallest physical memory region surrounding
  // the watched region that can be safely unwatched, if it doesn't matter,
  // return (0, UINT32_MAX).
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length,
      bool exact_range);
  void* RegisterPhysicalMemoryInvalidationCallback(
      PhysicalMemoryInvalidationCallback callback, void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  typedef void (*PhysicalMemoryReadCallback)(void* context_ptr,
                                             uint32_t physical_address_start,
                                             uint32_t length);
  void* RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback callback,
                                           void* callback_context);
  void UnregisterPhysicalMemoryReadCallback(void* callback_handle);

  // Write-protects the physical pages in the range (in every view) so the next
  // guest write invalidates GPU copies of them. Data providers are unsupported.
  void EnablePhysicalMemoryAccessCallbacks(
      uint32_t physical_address, uint32_t length,
      bool enable_invalidation_notifications, bool enable_data_providers);

  // Fires the invalidation callbacks for watched pages in a guest virtual range
  // and unwatches them. Must be called with the global critical region held.
  bool TriggerPhysicalMemoryCallbacks(
      global_unique_lock_type global_lock_locked_once, uint32_t virtual_address,
      uint32_t length, bool is_write, bool unwatch_exact_range,
      bool unprotect = true);

  VirtualHeap* GetPhysicalHeap();
  BaseHeap* LookupHeapByType(bool physical, uint32_t page_size);

  // DAO: handles a host access violation at host_address. Returns true if it
  // was a write to a watched page (now unwatched, so the access can be retried).
  bool HandleAccessViolation(void* host_address, bool is_write);

 private:
  struct State;

  uint8_t* virtual_membase_ = nullptr;
  uint8_t* physical_membase_ = nullptr;
  std::unique_ptr<State> state_;
  xe::global_critical_region global_critical_region_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
