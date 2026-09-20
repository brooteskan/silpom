// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/PreviewSampling.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

static void Check(bool passed)
{
    if (!passed) { std::cerr << "Preview sampling check failed\n"; std::exit(1); }
}
int main()
{
    using namespace SilPOM;
    Check(SampleColumns(0, 4) == 0 && SampleColumns(5, 4) == 2);
    Check(SampleCount({5, 3}, 2) == 6);
    SampleExtent small[]{{5, 3}, {3, 2}};
    Check(ChoosePreviewStride(small, 2, 21) == 1);
    Check(ChoosePreviewStride(small, 2, 20) == 2);
    Check(ChoosePreviewStride(small, 2, 1) == 0); // cannot silently drop a view
    SampleExtent full[]{{3840, 2160}, {256, 256}};
    const auto stride = ChoosePreviewStride(full, 2, 4096);
    Check(stride > 1 && SampleCount(full[0], stride) + SampleCount(full[1], stride) <= 4096);
    SampleExtent empty[]{{0, 5}, {10, 0}};
    Check(ChoosePreviewStride(empty, 2, 0) == 1);
    SampleExtent extreme[]{{UINT32_MAX, UINT32_MAX}};
    Check(ChoosePreviewStride(extreme, 1, 4) == (1u << 31));
    std::mt19937 random(9421);
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        SampleExtent views[32];
        const auto count = 1 + random() % 32;
        const auto budget = 32 + random() % (65536 - 31);
        for (unsigned i = 0; i < count; ++i) views[i] = {random() % 8193, random() % 8193};
        const auto block = ChoosePreviewStride(views, count, budget);
        Check(block && !(block & (block - 1)));
        std::uint64_t total = 0, previous = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            total += SampleCount(views[i], block);
            if (block > 1) previous += SampleCount(views[i], block / 2);
            if (views[i].width && views[i].height)
            {
                // Every raster pixel, including a partial final block, maps
                // to a record inside the allocated grid.
                Check((views[i].width - 1) / block < SampleColumns(views[i].width, block));
                Check((views[i].height - 1) / block < SampleColumns(views[i].height, block));
            }
        }
        Check(total <= budget && (block == 1 || previous > budget));
    }
    std::cout << "Preview sampling: boundaries and 10000 randomized budgets passed\n";
}
