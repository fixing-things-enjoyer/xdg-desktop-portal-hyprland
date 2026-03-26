#include "CoordTransform.hpp"

namespace xdph::vulkan {

    bool transformSwapsDimensions(wl_output_transform transform) {
        switch (transform) {
            case WL_OUTPUT_TRANSFORM_90:
            case WL_OUTPUT_TRANSFORM_270:
            case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            case WL_OUTPUT_TRANSFORM_FLIPPED_270: return true;
            default: return false;
        }
    }

    Dimensions getPhysicalDimensions(const Dimensions& logical, wl_output_transform transform) {
        if (transformSwapsDimensions(transform))
            return {logical.height, logical.width};
        return logical;
    }

    Dimensions getLogicalDimensions(const Dimensions& physical, wl_output_transform transform) {
        if (transformSwapsDimensions(transform))
            return {physical.height, physical.width};
        return physical;
    }

    wl_output_transform invertTransform(wl_output_transform transform) {
        switch (transform) {
            case WL_OUTPUT_TRANSFORM_NORMAL: return WL_OUTPUT_TRANSFORM_NORMAL;
            case WL_OUTPUT_TRANSFORM_90: return WL_OUTPUT_TRANSFORM_270;
            case WL_OUTPUT_TRANSFORM_180: return WL_OUTPUT_TRANSFORM_180;
            case WL_OUTPUT_TRANSFORM_270: return WL_OUTPUT_TRANSFORM_90;
            case WL_OUTPUT_TRANSFORM_FLIPPED: return WL_OUTPUT_TRANSFORM_FLIPPED;
            case WL_OUTPUT_TRANSFORM_FLIPPED_90: return WL_OUTPUT_TRANSFORM_FLIPPED_270;
            case WL_OUTPUT_TRANSFORM_FLIPPED_180: return WL_OUTPUT_TRANSFORM_FLIPPED_180;
            case WL_OUTPUT_TRANSFORM_FLIPPED_270: return WL_OUTPUT_TRANSFORM_FLIPPED_90;
            default: return WL_OUTPUT_TRANSFORM_NORMAL;
        }
    }

    static void transformPoint(int32_t& x, int32_t& y, int32_t w, int32_t h, wl_output_transform transform) {
        int32_t newX, newY;

        switch (transform) {
            case WL_OUTPUT_TRANSFORM_NORMAL: return;
            case WL_OUTPUT_TRANSFORM_90:
                newX = y;
                newY = w - 1 - x;
                break;
            case WL_OUTPUT_TRANSFORM_180:
                newX = w - 1 - x;
                newY = h - 1 - y;
                break;
            case WL_OUTPUT_TRANSFORM_270:
                newX = h - 1 - y;
                newY = x;
                break;
            case WL_OUTPUT_TRANSFORM_FLIPPED:
                newX = w - 1 - x;
                newY = y;
                break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_90:
                newX = y;
                newY = x;
                break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_180:
                newX = x;
                newY = h - 1 - y;
                break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_270:
                newX = h - 1 - y;
                newY = w - 1 - x;
                break;
            default: return;
        }

        x = newX;
        y = newY;
    }

    Region logicalToPhysical(const Region& logical, const Dimensions& logicalSize, wl_output_transform transform) {
        if (transform == WL_OUTPUT_TRANSFORM_NORMAL)
            return logical;

        int32_t x1 = logical.x;
        int32_t y1 = logical.y;
        int32_t x2 = logical.x + logical.width - 1;
        int32_t y2 = logical.y + logical.height - 1;

        int32_t w = static_cast<int32_t>(logicalSize.width);
        int32_t h = static_cast<int32_t>(logicalSize.height);

        transformPoint(x1, y1, w, h, transform);
        transformPoint(x2, y2, w, h, transform);

        int32_t minX = std::min(x1, x2);
        int32_t maxX = std::max(x1, x2);
        int32_t minY = std::min(y1, y2);
        int32_t maxY = std::max(y1, y2);

        return {minX, minY, maxX - minX + 1, maxY - minY + 1};
    }

    Region physicalToLogical(const Region& physical, const Dimensions& physicalSize, wl_output_transform transform) {
        if (transform == WL_OUTPUT_TRANSFORM_NORMAL)
            return physical;

        wl_output_transform inverse = invertTransform(transform);

        int32_t             x1 = physical.x;
        int32_t             y1 = physical.y;
        int32_t             x2 = physical.x + physical.width - 1;
        int32_t             y2 = physical.y + physical.height - 1;

        int32_t             w = static_cast<int32_t>(physicalSize.width);
        int32_t             h = static_cast<int32_t>(physicalSize.height);

        transformPoint(x1, y1, w, h, inverse);
        transformPoint(x2, y2, w, h, inverse);

        int32_t minX = std::min(x1, x2);
        int32_t maxX = std::max(x1, x2);
        int32_t minY = std::min(y1, y2);
        int32_t maxY = std::max(y1, y2);

        return {minX, minY, maxX - minX + 1, maxY - minY + 1};
    }

}
