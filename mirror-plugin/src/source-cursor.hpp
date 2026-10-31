#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

namespace OmarchyDisplay {

struct SourceCursorSnapshot {
    bool visible = false;
    Vector2D localPosition{};
};

class SourceCursorTracker {
  public:
    SourceCursorTracker() = default;
    ~SourceCursorTracker();
    SourceCursorTracker(const SourceCursorTracker&) = delete;
    SourceCursorTracker& operator=(const SourceCursorTracker&) = delete;

    void initialize(void (*invalidate)(), bool (*suspended)());
    void setSource(PHLMONITOR source, PHLWORKSPACE workspace);
    void reset();
    void shutdown();
    void observeMovement();
    SourceCursorSnapshot snapshot() const;
    void compose(PHLMONITOR source, const SourceCursorSnapshot& cursor, const Time::steady_tp& now, CRegion& damage);

  private:
    void observe(bool imageChanged);

    void (*m_invalidate)() = nullptr;
    bool (*m_suspended)() = nullptr;
    PHLMONITORREF m_source;
    PHLWORKSPACEREF m_workspace;
    SourceCursorSnapshot m_previous;
    CHyprSignalListener m_cursorChanged;
    CHyprSignalListener m_workspaceActive;
    CHyprSignalListener m_specialActive;
};

}
