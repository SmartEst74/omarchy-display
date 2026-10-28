# Omarchy Display bar widget

Targets the installed Omarchy Quickshell plugin SDK: `qs.Ui.BarWidget`,
`qs.Ui.BarIconButton`, `qs.Ui.Panel`, `qs.Ui.KeyboardPanel`, `qs.Ui.Button`, and
`qs.Commons.Style`. The manifest uses schema version 1 and the `barWidget` entry
point. Install through the project installer; this directory is not a standalone
Quickshell configuration.

The monitor stays visible when stopped (dimmed). A hollow accent dot means the
virtual output is ready **without a TV viewer**; a filled accent dot means the
host reports an authenticated viewer on its capture backend. An exclamation
mark means error; an ellipsis means starting/checking. Hover for a text status.
All states are view-only with no audio.

The widget invokes the installed `omarchy-display status --json` every five
seconds, with one process per widget instance at a time and a four-second
deadline. Invalid data, unsuccessful exit, and timeout produce Error, never
Sharing. Host messages, output names, stderr, and pairing details are not shown.
The last completed status can remain visible until the next poll finishes.

Left-click opens the native Omarchy control panel. Start and Stop are explicit
actions. **Mirror** renders the same live numbered workspace on the computer
and TV without transferring its windows; switching computer workspaces does not
change the selected TV workspace. It requires the owned compositor plugin and
the exact supported Hyprland ABI; unavailable or mismatched Mirror fails, never
silently switches to Extend. **Extend** retains the independent TV workspace
transfer behavior. Its Bring back action returns a workspace only to a recorded
original monitor; recall is not offered in Mirror.

The panel's Mirror/Extend mode buttons save **next-start** configuration only.
They never change the running session or restart it. Pending mode changes require
an explicit Stop, then Start. Fresh configurations prefer Mirror; legacy settings
and running v1.1 sessions without a mode remain honestly classified as Extend.
The workspace row action is labelled Mirror or Extend for the running mode;
SUPER+CTRL+ALT+1..9/0 selects workspace 1..10 using that same current mode.
Installer migration accepts the exact owned v1.1 shortcut block and backs it up,
but refuses edited or unrelated bindings.

Resolution (720p, 1080p, or API-reported native panel size) and FPS (15, 30, 60)
are saved explicitly for the next start. Saving never restarts the host; pending
changes require an explicit Stop followed by Start. Native is disabled without a
reported native panel size. Current capture, receiver viewport, and receiver
panel dimensions are shown separately, including unknown values. Native panel
dimensions are reported by the Samsung device API, not physically measured.
Refresh TV info explicitly runs the read-only `omarchy-display detect` action;
it does not restart the display. FPS labels describe the configured capture cap,
not an achieved or measured frame rate.

Only the explicit Pairing button opens a terminal, using the existing
`omarchy-launch-floating-terminal-with-presentation` helper with fixed arguments
`omarchy-display pair`. The popup never shows pairing details. Status and actions
use argument arrays, not interpolated shell commands. Commands must be available
on the shell's PATH; no checkout or hard-coded home path is required.

After installation, useful local inspection commands are:

```sh
omarchy-display status --json
omarchy-shell shell rescanPlugins
```

When updating controls that the shell has already loaded, run
`omarchy-restart-shell` while unlocked to replace cached QML components. This
restarts the shell without resetting its settings; `omarchy-refresh-shell`
resets settings and is not the update command.

The installer/main integration check should inspect the actual bar while
stopped, ready without a viewer, sharing with an authenticated viewer, and after
the viewer disconnects. Check native popup opening, Escape/Tab navigation,
Start/Stop, numbered workspace Mirror/Extend/Bring back, and independent computer
workspace switching. Check mode save leaves the active mode unchanged until an
explicit Stop/Start, Mirror leaves window/workspace assignments and focus intact,
and unavailable plugin/ABI mismatch cannot fall back to Extend.
Check sizing save leaves the current session uninterrupted,
requires explicit Stop/Start, and keeps receiver viewport separate from panel
dimensions. Check Pairing opens only after an explicit click and failed/malformed
status cannot retain Sharing beyond the next poll deadline. The installer stages
and builds the compositor plugin rootlessly, recording its SHA-256 and exact ABI;
the host loads only that verified payload for Mirror and clears it before capture
cleanup. The supervisor fails closed if plugin health loses the selection.
