#include "source-cursor.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace OmarchyDisplay {

SourceCursorTracker::~SourceCursorTracker() {
    shutdown();
}

void SourceCursorTracker::initialize(void (*invalidate)(), bool (*suspended)()) {
    shutdown();
    m_invalidate = invalidate;
    m_suspended = suspended;
}

void SourceCursorTracker::setSource(PHLMONITOR source, PHLWORKSPACE workspace) {
    reset();
    if (!source || !valid(workspace) || workspace->m_id <= 0 || workspace->m_isSpecialWorkspace)
        return;

    m_source = source;
    m_workspace = workspace;
    m_previous = snapshot();
    m_cursorChanged = Pointer::mgr()->m_events.cursorChanged.listen([this] { observe(true); });
    m_workspaceActive = Event::bus()->m_events.workspace.active.listen([this] { observe(false); });
    m_specialActive = Event::bus()->m_events.workspace.specialActive.listen([this] { observe(false); });
}

void SourceCursorTracker::reset() {
    m_cursorChanged.reset();
    m_workspaceActive.reset();
    m_specialActive.reset();
    m_source.reset();
    m_workspace.reset();
    m_previous = {};
}

void SourceCursorTracker::shutdown() {
    reset();
    m_invalidate = nullptr;
    m_suspended = nullptr;
}

SourceCursorSnapshot SourceCursorTracker::snapshot() const {
    SourceCursorSnapshot cursor;
    if (!m_suspended || m_suspended())
        return cursor;

    const auto source = m_source.lock();
    const auto workspace = m_workspace.lock();
    if (!source || !valid(workspace) || !source->m_enabled || !source->m_dpmsStatus || workspace->m_monitor != source ||
        source->m_activeWorkspace != workspace || source->m_activeSpecialWorkspace)
        return cursor;

    const auto pointer = Pointer::mgr().get();
    const auto position = pointer->position() - source->m_position;
    // The hotspot, not a cursor image overlapping a neighbouring output, owns it.
    if (!(position.x >= 0 && position.y >= 0 && position.x < source->m_size.x && position.y < source->m_size.y))
        return cursor;
    if (!g_pHyprRenderer || !g_pHyprRenderer->shouldRenderCursor() || !pointer->getCurrentCursorTexture())
        return cursor;

    cursor.visible = true;
    cursor.localPosition = position;
    return cursor;
}

void SourceCursorTracker::observeMovement() {
    observe(false);
}

void SourceCursorTracker::observe(bool imageChanged) {
    // In particular, cursor-surface callbacks during our own render must not
    // compare against the monitor's temporary active-workspace substitution.
    if (!m_suspended || m_suspended())
        return;

    const auto current = snapshot();
    const bool changed = current.visible != m_previous.visible ||
        (current.visible && (imageChanged || current.localPosition != m_previous.localPosition));
    m_previous = current;
    if (changed && m_invalidate)
        m_invalidate();
}

void SourceCursorTracker::compose(PHLMONITOR source, const SourceCursorSnapshot& cursor, const Time::steady_tp& now, CRegion& damage) {
    // Eligibility was sampled before the parent's temporary workspace override.
    // Its suspended callback is deliberately not consulted inside this render.
    if (!cursor.visible || !source || source != m_source.lock())
        return;
    Pointer::mgr()->renderSoftwareCursorsFor(source, now, damage, cursor.localPosition, true, true);
}

}
