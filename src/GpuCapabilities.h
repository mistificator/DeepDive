#pragma once
#include <QString>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

struct AdapterCapability {
    QString name;
    size_t dedicatedVramMB = 0;
    size_t sharedSystemMB = 0;
    bool isDiscrete = false;
    int score = 0;
};

namespace GpuUtils {

inline std::vector<AdapterCapability> enumerateAndSelectBestGpu() {
    std::vector<AdapterCapability> list;

#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory6)))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

                AdapterCapability cap;
                cap.name = QString::fromWCharArray(desc.Description);
                cap.dedicatedVramMB = static_cast<size_t>(desc.DedicatedVideoMemory / (1024 * 1024));
                cap.sharedSystemMB  = static_cast<size_t>(desc.SharedSystemMemory / (1024 * 1024));

                // Оценка производительности по выделенной VRAM и архитектуре вендора (NVIDIA = 0x10DE, AMD = 0x1002)
                bool isVendorDiscrete = (desc.VendorId == 0x10DE || desc.VendorId == 0x1002);
                cap.isDiscrete = isVendorDiscrete && (cap.dedicatedVramMB >= 1024);
                cap.score = static_cast<int>(cap.dedicatedVramMB) + (cap.isDiscrete ? 100000 : 0);
                list.push_back(cap);
            }
            adapter.Reset();
        }
    }
#endif

    std::sort(list.begin(), list.end(), [](const AdapterCapability& a, const AdapterCapability& b) {
        return a.score > b.score;
    });
    return list;
}

} // namespace GpuUtils
