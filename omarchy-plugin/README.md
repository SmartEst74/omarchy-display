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
actions. The numbered **TV workspace** stays selected independently of physical
computer workspace switches. Send selects/moves a workspace onto the TV output;
Bring back returns it to the computer. This does not promise simultaneous
duplicated rendering on both outputs.
Bring back is enabled only where the host has recorded an original monitor;
the virtual output's automatically created workspace has no such destination.

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

The installer/main integration check should inspect the actual bar while
stopped, ready without a viewer, sharing with an authenticated viewer, and after
the viewer disconnects. Check native popup opening, Escape/Tab navigation,
Start/Stop, numbered workspace Send/Bring back, and independent computer
workspace switching. Check sizing save leaves the current session uninterrupted,
requires explicit Stop/Start, and keeps receiver viewport separate from panel
dimensions. Check Pairing opens only after an explicit click and failed/malformed
status cannot retain Sharing beyond the next poll deadline. No build or runtime
verification was performed while creating this widget.
