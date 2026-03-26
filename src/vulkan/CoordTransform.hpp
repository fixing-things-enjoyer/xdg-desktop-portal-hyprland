#pragma once

#include <wayland-client-protocol.h>
#include <cstdint>

namespace xdph::vulkan {

    struct Region {
        int32_t x, y, width, height;
    };

    struct Dimensions {
        uint32_t width, height;
    };

    Region              logicalToPhysical(const Region& logical, const Dimensions& logicalSize, wl_output_transform transform);

    Region              physicalToLogical(const Region& physical, const Dimensions& physicalSize, wl_output_transform transform);

    Dimensions          getPhysicalDimensions(const Dimensions& logical, wl_output_transform transform);

    Dimensions          getLogicalDimensions(const Dimensions& physical, wl_output_transform transform);

    bool                transformSwapsDimensions(wl_output_transform transform);

    wl_output_transform invertTransform(wl_output_transform transform);

}
