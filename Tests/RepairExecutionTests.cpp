// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/RepairExecution.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using SilPOM::RepairExecution::Mode;
using SilPOM::RepairExecution::Preparation;
using Candidate = SilPOM::RepairRegion::Mode;
std::size_t checks = 0;
void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void CheckPreparationOrder(bool classifyFirst)
{
    Preparation preparation(Candidate::Regional, 5);
    Require(preparation.ReservedItemCount() == 5, "Regional setup lost candidate capacity");
    Require(!preparation.IsSealed() && !preparation.BothPrepared(), "Preparation started finalized");
    Require(!preparation.Seal(true, true), "A pair sealed before either compile callback");
    Require(!preparation.Submits(0, 0, 5), "An unsealed pair could submit");
    preparation.Prepared(classifyFirst);
    preparation.Prepared(classifyFirst);
    Require(!preparation.BothPrepared(), "One pass reported twice counted as both passes");
    Require(!preparation.Seal(true, false), "One compile callback finalized a fallback pair");
    Require(!preparation.IsSealed() && preparation.DrawCount() == 0, "An early seal changed execution");
    preparation.Prepared(!classifyFirst);
    Require(preparation.BothPrepared() && preparation.Seal(true, false), "Both compile callbacks did not seal");
    Require(preparation.GetMode() == Mode::FullScreen && preparation.DrawCount() == 1,
        "Uncertain regional evidence did not restore the full repair pair");
    Require(preparation.ReservedItemCount() == 5, "Late fallback changed already allocated graph capacity");
    Require(preparation.Seal(false, true) && preparation.GetMode() == Mode::FullScreen,
        "A later validation changed a sealed decision");
    preparation.Prepared(classifyFirst);
    Require(preparation.DrawCount() == 1, "Late preparation altered a sealed draw schedule");
}

void CheckDecision(Candidate candidate, std::size_t rectangles, bool baseline, bool coverage, Mode expected)
{
    Preparation preparation(candidate, rectangles);
    const auto reserved = std::max(std::size_t{1}, rectangles);
    Require(preparation.ReservedItemCount() == reserved, "Setup did not reserve fallback capacity");
    preparation.Prepared(true);
    preparation.Prepared(false);
    Require(preparation.Seal(baseline, coverage) && preparation.IsSealed(), "Prepared pair did not finalize");
    Require(preparation.GetMode() == expected, "Baseline and optimization contracts selected the wrong mode");
    const auto count = expected == Mode::FullScreen ? std::size_t{1}
        : expected == Mode::Regional ? rectangles : std::size_t{0};
    Require(preparation.DrawCount() == count && count <= reserved, "Final schedule exceeded graph capacity");

    // Model partitioned execution groups, including one with an empty range.
    // Both scopes must draw the same indices exactly once, even when a late
    // fallback leaves most of the regional slots unused.
    for (const std::size_t partitionWidth : {std::size_t{1}, std::size_t{2}, reserved + 1})
    {
        std::array<std::vector<unsigned>, 2> submitted{
            std::vector<unsigned>(reserved), std::vector<unsigned>(reserved)};
        for (auto& pass : submitted)
        {
            Require(!preparation.Submits(0, 0, 0), "Empty submit range issued a draw");
            for (std::size_t start = 0; start < reserved; start += partitionWidth)
            {
                const auto end = std::min(start + partitionWidth, reserved);
                for (std::size_t index = 0; index < reserved + 2; ++index)
                    if (preparation.Submits(index, start, end)) ++pass[index];
            }
        }
        Require(submitted[0] == submitted[1], "Classification and fill consumed different schedules");
        for (std::size_t index = 0; index < reserved; ++index)
            Require(submitted[0][index] == (index < count ? 1u : 0u),
                "Partitioned execution duplicated, omitted, or exceeded a final draw");
    }
    const auto sealed = preparation.GetMode();
    Require(preparation.Seal(!baseline, !coverage) && preparation.GetMode() == sealed,
        "Repeated finalization changed an execution decision");
}

void CheckScratchCoverageAfterFallback(Candidate candidate, std::size_t candidateRectangles)
{
    Preparation preparation(candidate, candidateRectangles);
    preparation.Prepared(true);
    preparation.Prepared(false);
    Require(preparation.Seal(true, false), "Recoverable fallback did not seal");
    std::array<bool, 32> initialized{};
    unsigned classifications = 0, fills = 0;
    // Each reserved slot gets its own context. Classification must initialize
    // the entire fill domain once, including outside the candidate islands.
    for (std::size_t start = 0; start < preparation.ReservedItemCount(); ++start)
        if (preparation.Submits(0, start, start + 1))
        {
            initialized.fill(true);
            ++classifications;
        }
    for (std::size_t start = 0; start < preparation.ReservedItemCount(); ++start)
        if (preparation.Submits(0, start, start + 1))
        {
            for (bool sample : initialized) Require(sample, "Fallback fill read an uninitialized scratch sample");
            ++fills;
        }
    Require(classifications == 1 && fills == 1, "Fallback must execute exactly one complete pair");
}
}

int main()
{
    try
    {
        CheckPreparationOrder(true);
        CheckPreparationOrder(false);
        for (const auto candidate : {Candidate::NoWork, Candidate::Regional, Candidate::FullScreenFallback})
        {
            const std::size_t rectangles = candidate == Candidate::NoWork ? 0
                : candidate == Candidate::Regional ? 7 : 1;
            for (bool baseline : {false, true})
                for (bool coverage : {false, true})
                {
                    const auto expected = !baseline ? Mode::Unavailable : !coverage ? Mode::FullScreen
                        : candidate == Candidate::NoWork ? Mode::NoWork
                        : candidate == Candidate::Regional ? Mode::Regional : Mode::FullScreen;
                    CheckDecision(candidate, rectangles, baseline, coverage, expected);
                }
        }
        CheckScratchCoverageAfterFallback(Candidate::Regional, 7);
        CheckScratchCoverageAfterFallback(Candidate::NoWork, 0);
        std::cout << checks << " repair execution checks passed: paired sealing, fallback capacity and partitioned submissions\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
