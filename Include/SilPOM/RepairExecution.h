// SPDX-License-Identifier: MIT
#pragma once
#include "RepairRegion.h"
#include <cstddef>

namespace SilPOM::RepairExecution
{
enum class Mode { Unavailable, FullScreen, Regional, NoWork };

// Framegraph setup reserves capacity from a candidate plan. The paired compile
// callbacks then seal one decision before either scope records any commands.
class Preparation
{
public:
    Preparation(RepairRegion::Mode candidate, std::size_t rectangleCount)
        : m_candidate(candidate), m_rectangleCount(rectangleCount) {}

    std::size_t ReservedItemCount() const
    {
        return std::max(std::size_t{1}, m_rectangleCount);
    }

    void Prepared(bool classify)
    {
        if (classify) m_classifyPrepared = true;
        else m_fillPrepared = true;
    }

    bool BothPrepared() const { return m_classifyPrepared && m_fillPrepared; }
    bool IsSealed() const { return m_sealed; }
    Mode GetMode() const { return m_mode; }

    // Baseline validity describes execution safety under the declared graph;
    // coverage validity describes only the regional/no-work optimization.
    // Repeated calls cannot revise a decision already exposed to recording.
    bool Seal(bool baselineValid, bool coverageValid)
    {
        if (m_sealed) return true;
        if (!BothPrepared()) return false;
        if (!baselineValid) m_mode = Mode::Unavailable;
        else if (!coverageValid) m_mode = Mode::FullScreen;
        else
        {
            switch (m_candidate)
            {
            case RepairRegion::Mode::FullScreenFallback: m_mode = Mode::FullScreen; break;
            case RepairRegion::Mode::Regional: m_mode = Mode::Regional; break;
            case RepairRegion::Mode::NoWork: m_mode = Mode::NoWork; break;
            }
        }
        m_sealed = true;
        return true;
    }

    std::size_t DrawCount() const
    {
        if (!m_sealed) return 0;
        switch (m_mode)
        {
        case Mode::FullScreen: return 1;
        case Mode::Regional: return m_rectangleCount;
        default: return 0;
        }
    }

    bool Submits(std::size_t index, std::size_t start, std::size_t end) const
    {
        return index >= start && index < end && index < DrawCount();
    }

private:
    RepairRegion::Mode m_candidate;
    std::size_t m_rectangleCount;
    Mode m_mode = Mode::Unavailable;
    bool m_classifyPrepared = false;
    bool m_fillPrepared = false;
    bool m_sealed = false;
};
}
