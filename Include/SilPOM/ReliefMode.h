// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace SilPOM
{
// How a SilPOM mesh finds relief hits. Traverse marches the heightfield with
// the Maximum cells budget; every other mode is a trained (no-march) route.
// Values are serialized and mirrored by SP_RELIEF_* in BlendedFace.azsli.
enum class ReliefMode : std::uint32_t
{
    Traverse = 0,
    NoMarch = 1,           // neural candidate with grazing fill
    RectangleControl = 2,  // no-march, rectangle coverage control
    UnfilledControl = 3,   // no-march, no grazing fill
    Lod2px = 4,
    Lod4px = 5,            // smooth LOD
    Lod8px = 6,
    LodShell = 7,          // LOD shell only
    Lod4pxResolve = 8,     // smooth 4 px LOD + full-screen winning-hit resolve
};

namespace ReliefModes
{
// Budget given to modes converted from the legacy sentinels. The sentinels
// acted as an effectively unlimited budget, so the conversion keeps it large.
constexpr std::uint32_t ConvertedMaxCells = 65536u;

constexpr bool IsLod(ReliefMode mode)
{
    return mode >= ReliefMode::Lod2px && mode <= ReliefMode::Lod4pxResolve;
}
constexpr bool IsNoMarch(ReliefMode mode)
{
    return mode >= ReliefMode::NoMarch && mode <= ReliefMode::Lod4pxResolve;
}
constexpr bool IsValid(ReliefMode mode)
{
    return static_cast<std::uint32_t>(mode) <= static_cast<std::uint32_t>(ReliefMode::Lod4pxResolve);
}

// Older data stored the mode in Maximum cells (65525-65534). Returns false and
// leaves the arguments unchanged when the values are not in the legacy form.
constexpr bool UpgradeLegacy(std::uint32_t& maxCells, ReliefMode& mode)
{
    if (mode != ReliefMode::Traverse)
        return false;
    ReliefMode upgraded = ReliefMode::Traverse;
    switch (maxCells)
    {
    case 65534u: upgraded = ReliefMode::NoMarch; break;
    case 65531u: upgraded = ReliefMode::RectangleControl; break;
    case 65530u: upgraded = ReliefMode::UnfilledControl; break;
    case 65529u: upgraded = ReliefMode::Lod2px; break;
    case 65528u: upgraded = ReliefMode::Lod4px; break;
    case 65527u: upgraded = ReliefMode::Lod8px; break;
    case 65526u: upgraded = ReliefMode::LodShell; break;
    case 65525u: upgraded = ReliefMode::Lod4pxResolve; break;
    default: return false; // 65532 and 65533 were never modes
    }
    mode = upgraded;
    maxCells = ConvertedMaxCells;
    return true;
}
} // namespace ReliefModes
} // namespace SilPOM
