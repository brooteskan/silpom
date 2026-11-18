// SPDX-License-Identifier: MIT
// ReliefMode replaces the mode sentinels formerly stored in Maximum cells.
// Checks the load-time conversion, and that the shader helpers keep the exact
// meaning of the old sentinel tests for every mode.
#include "../Include/SilPOM/ReliefMode.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
using uint = std::uint32_t;
#include "../Assets/Shaders/SilPOM/ReliefMode.azsli"
using namespace SilPOM;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// Frozen sentinel definitions from BlendedFace.azsli before ReliefMode.
namespace Legacy
{
bool UsesLod(uint m) { return m >= 65525u && m <= 65529u; }
bool UsesSmoothLod(uint m) { return m == 65528u || m == 65525u; }
bool NoMarch(uint m) { return m == 65534u || m == 65531u || m == 65530u || UsesLod(m); }
bool FillEnabled(uint m) { return m == 65534u || UsesLod(m); }
float LodPixels(uint m) { const uint mode = UsesSmoothLod(m) ? 65528u : m; return 2.0f * float(1u << (65529u - mode)); }
}

int main()
{
    try
    {
        static_assert(SP_RELIEF_TRAVERSE == uint(ReliefMode::Traverse));
        static_assert(SP_RELIEF_NO_MARCH == uint(ReliefMode::NoMarch));
        static_assert(SP_RELIEF_RECTANGLE_CONTROL == uint(ReliefMode::RectangleControl));
        static_assert(SP_RELIEF_UNFILLED_CONTROL == uint(ReliefMode::UnfilledControl));
        static_assert(SP_RELIEF_LOD_2PX == uint(ReliefMode::Lod2px));
        static_assert(SP_RELIEF_LOD_4PX == uint(ReliefMode::Lod4px));
        static_assert(SP_RELIEF_LOD_8PX == uint(ReliefMode::Lod8px));
        static_assert(SP_RELIEF_LOD_SHELL == uint(ReliefMode::LodShell));
        static_assert(SP_RELIEF_LOD_4PX_RESOLVE == uint(ReliefMode::Lod4pxResolve));

        const uint sentinels[] = {65534u, 65531u, 65530u, 65529u, 65528u, 65527u, 65526u, 65525u};
        for (uint sentinel : sentinels)
        {
            uint cells = sentinel;
            ReliefMode mode = ReliefMode::Traverse;
            Require(ReliefModes::UpgradeLegacy(cells, mode), "A legacy sentinel was not converted");
            Require(cells == ReliefModes::ConvertedMaxCells, "Converted budget must be 65536");
            Require(ReliefModes::IsValid(mode) && ReliefModes::IsNoMarch(mode), "Converted mode must be a no-march mode");
            const uint m = uint(mode);
            Require(SilPomReliefNoMarch(m) == Legacy::NoMarch(sentinel), "NoMarch differs from the sentinel test");
            Require(SilPomReliefUsesLod(m) == Legacy::UsesLod(sentinel), "UsesLod differs from the sentinel test");
            Require(ReliefModes::IsLod(mode) == Legacy::UsesLod(sentinel), "C++ IsLod differs from the sentinel test");
            Require(SilPomReliefUsesSmoothLod(m) == Legacy::UsesSmoothLod(sentinel), "UsesSmoothLod differs");
            Require(SilPomReliefFillEnabled(m) == Legacy::FillEnabled(sentinel), "FillEnabled differs");
            // The shell mode returns depth 0 before using a pixel budget.
            if (Legacy::UsesLod(sentinel) && sentinel != 65526u)
                Require(SilPomReliefLodPixels(m) == Legacy::LodPixels(sentinel), "LOD pixel budget differs");
            Require(!ReliefModes::UpgradeLegacy(cells, mode), "Conversion must be idempotent");
        }
        Require(uint(ReliefMode::Lod4pxResolve) == 8u && !SilPomReliefNoMarch(SP_RELIEF_TRAVERSE), "Traverse must march");

        // Ordinary budgets, unused values and already-converted data are kept.
        for (uint cells : {1u, 1024u, 4096u, 65524u, 65532u, 65533u, 65535u, 65536u})
        {
            uint kept = cells;
            ReliefMode mode = ReliefMode::Traverse;
            Require(!ReliefModes::UpgradeLegacy(kept, mode) && kept == cells && mode == ReliefMode::Traverse,
                "A traversal budget was treated as a mode");
        }
        uint cells = 65525u;
        ReliefMode explicitMode = ReliefMode::NoMarch;
        Require(!ReliefModes::UpgradeLegacy(cells, explicitMode) && cells == 65525u && explicitMode == ReliefMode::NoMarch,
            "An explicit mode must not be overridden by Maximum cells");
        Require(!ReliefModes::IsValid(static_cast<ReliefMode>(9u)), "Out-of-range modes must be rejected");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "ReliefMode tests passed\n";
    return 0;
}
