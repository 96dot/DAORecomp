// DAO: D3D12Provider's additions: logging why the device was removed and,
// with DAO_DRED=1, where the GPU stopped.
#include "xenia/ui/d3d12/d3d12_provider.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace d3d12 {

void D3D12Provider::LogDeviceRemoved() const {
  if (!device_) {
    return;
  }
  XELOGE("D3D12: device removed, reason {:08X}",
         uint32_t(device_->GetDeviceRemovedReason()));
  ID3D12DeviceRemovedExtendedData* dred;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
         node; node = node->pNext) {
      const uint32_t last = node->pLastBreadcrumbValue
                                ? *node->pLastBreadcrumbValue
                                : 0;
      // Only the command lists the GPU had started but not finished.
      if (!last || last >= node->BreadcrumbCount) {
        continue;
      }
      // The operations around the one the GPU stopped at, bracketed.
      constexpr uint32_t kOpsBefore = 24, kOpsAfter = 8;
      std::string ops;
      const uint32_t first = last > kOpsBefore ? last - kOpsBefore : 0;
      const uint32_t end = std::min(node->BreadcrumbCount, last + kOpsAfter);
      for (uint32_t i = first; i < end; ++i) {
        ops += fmt::format("{}{}{} ", i == last ? "[" : "",
                           uint32_t(node->pCommandHistory[i]),
                           i == last ? "]" : "");
      }
      XELOGE("D3D12: command list {} of {} operations completed: {}", last,
             node->BreadcrumbCount, ops);
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) &&
      page_fault.PageFaultVA) {
    XELOGE("D3D12: page fault at GPU address {:016X}", page_fault.PageFaultVA);
    for (const D3D12_DRED_ALLOCATION_NODE* node =
             page_fault.pHeadExistingAllocationNode;
         node; node = node->pNext) {
      XELOGE("D3D12: existing allocation at the address: type {} {}",
             uint32_t(node->AllocationType),
             node->ObjectNameA ? node->ObjectNameA : "");
    }
    for (const D3D12_DRED_ALLOCATION_NODE* node =
             page_fault.pHeadRecentFreedAllocationNode;
         node; node = node->pNext) {
      XELOGE("D3D12: recently freed allocation at the address: type {} {}",
             uint32_t(node->AllocationType),
             node->ObjectNameA ? node->ObjectNameA : "");
    }
  }
  dred->Release();
}

void D3D12Provider::DaoEnableDred() const {
  if (const char* dred = std::getenv("DAO_DRED"); dred && std::atoi(dred)) {
    ID3D12DeviceRemovedExtendedDataSettings* dred_settings;
    if (SUCCEEDED(
            pfn_d3d12_get_debug_interface_(IID_PPV_ARGS(&dred_settings)))) {
      dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred_settings->Release();
      XELOGI("D3D12: device removed extended data enabled");
    } else {
      XELOGW("D3D12: failed to enable the device removed extended data");
    }
  }
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
