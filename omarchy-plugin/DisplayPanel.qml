import QtQuick
import QtQuick.Controls as Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

Panel {
  id: root
  moduleName: "smartest74.omarchy-display"
  manageIpc: false

  property Item anchorItem: null
  property var hostWidget: null
  property var status: null
  property string description: "Checking display status."
  property string draftPreset: "1080p"
  property int draftFps: 30
  property bool draftChanged: false
  property bool actionPending: false
  property string actionMessage: ""
  readonly property bool busy: actionPending || actionProc.running
  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  function syncSizing() {
    if (!status || draftChanged) return
    draftPreset = status.sizing.preset
    draftFps = status.sizing.fps
  }

  function dimensions(width, height) {
    return width && height ? width + " × " + height : "unknown"
  }

  function runAction(argumentsList, pendingMessage) {
    if (busy) return
    actionMessage = pendingMessage
    actionPending = true
    actionProc.command = ["omarchy-display"].concat(argumentsList)
    actionDeadline.restart()
    actionProc.running = true
  }

  function workspaceAction(command, workspaceId) {
    if (!status || !status.running || ["workspace", "recall"].indexOf(command) < 0
        || typeof workspaceId !== "number" || workspaceId <= 0
        || Math.floor(workspaceId) !== workspaceId) return
    runAction([command, String(workspaceId)], command === "workspace"
      ? "Selecting TV workspace…" : "Bringing workspace back…")
  }

  function applySizing() {
    if (!status || ["720p", "1080p", "native"].indexOf(draftPreset) < 0
        || [15, 30, 60].indexOf(draftFps) < 0
        || (draftPreset === "native" && !status.sizing.nativeAvailable)) return
    runAction(["size", draftPreset, "--fps", String(draftFps)], "Saving next-start sizing…")
  }

  function showPairing() {
    root.close()
    Quickshell.execDetached([
      "omarchy-launch-floating-terminal-with-presentation",
      "omarchy-display", "pair"
    ])
  }

  onStatusChanged: {
    syncSizing()
    if (actionMessage === "Command completed. Refreshing status…")
      actionMessage = "Command completed."
  }
  onOpenedChanged: if (opened) {
    draftChanged = false
    syncSizing()
    if (hostWidget) hostWidget.refresh()
    Qt.callLater(function() { panelContent.forceActiveFocus() })
  }

  Process {
    id: actionProc
    stdout: StdioCollector { waitForEnd: true }
    stderr: StdioCollector { waitForEnd: true }
    onExited: function(exitCode, exitStatus) {
      if (!root.actionPending) return
      root.actionPending = false
      actionDeadline.stop()
      if (exitCode === 0 && exitStatus === 0) {
        root.actionMessage = "Command completed. Refreshing status…"
        root.draftChanged = false
      } else {
        root.actionMessage = "Command failed. Check local controls for details; no private diagnostics are shown here."
      }
      if (root.hostWidget) root.hostWidget.refresh()
    }
  }

  Timer {
    id: actionDeadline
    interval: 60000
    onTriggered: {
      root.actionPending = false
      actionProc.running = false
      root.actionMessage = "Command did not complete. Check the current host state before trying again."
      if (root.hostWidget) root.hostWidget.refresh()
    }
  }

  component PanelText: Text {
    width: parent.width
    textFormat: Text.PlainText
    color: root.foreground
    font.family: root.fontFamily
    font.pixelSize: Style.font.bodySmall
    wrapMode: Text.WordWrap
  }

  KeyboardPanel {
    id: panel
    anchorItem: root.anchorItem
    owner: root.hostWidget || root
    bar: root.bar
    open: root.opened
    focusTarget: panelContent
    contentWidth: panel.fittedContentWidth(Style.space(440))
    contentHeight: panel.fittedContentHeight(content.implicitHeight, Style.space(660))

    Item {
      id: panelContent
      anchors.fill: parent
      Keys.onEscapePressed: root.close()

      Flickable {
        id: scroll
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        Controls.ScrollBar.vertical: Controls.ScrollBar { policy: Controls.ScrollBar.AsNeeded }

        Column {
          id: content
          width: scroll.width
          spacing: Style.spacing.sm

          PanelText {
            text: "Omarchy Display"
            font.pixelSize: Style.font.title
            font.bold: true
          }
          PanelText { text: root.description }
          PanelText {
            text: "Independent TV workspace · view-only · no audio\nThe TV stays on the selected numbered workspace while you switch workspaces on the computer. A workspace is moved, not duplicated on both screens."
          }

          Flow {
            width: parent.width
            spacing: Style.spacing.sm
            Button {
              id: startButton
              text: "Start"
              focusable: true
              bordered: true
              enabled: !root.busy && (!root.status || !root.status.running)
              onClicked: root.runAction(["start"], "Starting virtual output…")
            }
            Button {
              text: "Stop"
              focusable: true
              bordered: true
              enabled: !root.busy && (!root.status || root.status.running)
              onClicked: root.runAction(["stop"], "Stopping virtual output…")
            }
            Button {
              text: "Pairing"
              focusable: true
              bordered: true
              enabled: !root.busy
              onClicked: root.showPairing()
            }
            Button {
              text: "Refresh TV info"
              focusable: true
              bordered: true
              enabled: !root.busy
              tooltipText: "Read Samsung device API resolution without restarting the display."
              onClicked: root.runAction(["detect"], "Reading TV-reported resolution…")
            }
          }
          PanelText {
            visible: root.actionMessage !== ""
            text: root.actionMessage
          }

          PanelSeparator { width: parent.width }
          PanelText {
            text: "TV workspace: " + (root.status && root.status.tvWorkspace !== null
              ? root.status.tvWorkspace : "unknown")
            font.bold: true
          }
          PanelText { text: "Send selects a workspace for the TV. Bring back returns it to the computer without mirroring." }
          PanelText {
            visible: !root.status || root.status.workspaces.length === 0
            text: root.status ? "No numbered workspaces are available." : "Workspace status unavailable."
          }
          Repeater {
            model: root.status ? root.status.workspaces : []
            delegate: Row {
              required property var modelData
              width: content.width
              spacing: Style.spacing.sm
              Text {
                width: Math.max(0, parent.width - sendButton.width - recallButton.width - parent.spacing * 2)
                anchors.verticalCenter: parent.verticalCenter
                text: "Workspace " + modelData.id + " · " + modelData.windows + " window"
                  + (modelData.windows === 1 ? "" : "s") + (modelData.onTV ? " · on TV" : "")
                  + (root.status && modelData.id === root.status.tvWorkspace ? " · selected" : "")
                textFormat: Text.PlainText
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.bodySmall
                wrapMode: Text.WordWrap
              }
              Button {
                id: sendButton
                text: "Send"
                focusable: true
                bordered: true
                enabled: !root.busy && root.status && root.status.running
                  && modelData.id !== root.status.tvWorkspace
                onClicked: root.workspaceAction("workspace", modelData.id)
              }
              Button {
                id: recallButton
                text: "Bring back"
                focusable: true
                bordered: true
                tooltipText: modelData.recallAvailable
                  ? "Return to this session's original computer monitor."
                  : "No original monitor was recorded for this workspace."
                enabled: !root.busy && root.status && root.status.running
                  && modelData.onTV && modelData.recallAvailable
                onClicked: root.workspaceAction("recall", modelData.id)
              }
            }
          }

          PanelSeparator { width: parent.width }
          PanelText {
            text: "Stream and receiver"
            font.bold: true
          }
          PanelText {
            text: "Current capture: " + (root.status
              ? root.dimensions(root.status.width, root.status.height) + " · "
                + (root.status.fps === null ? "configured FPS cap unknown"
                  : "configured cap " + root.status.fps + " FPS (not measured)")
              : "unknown")
          }
          PanelText {
            text: "Receiver viewport: " + (root.status && root.status.receiver
              ? root.dimensions(root.status.receiver.viewportWidth, root.status.receiver.viewportHeight)
              : "unknown (no receiver report)")
          }
          PanelText {
            text: "TV resolution (Samsung device API): " + (root.status && root.status.panel
              ? root.dimensions(root.status.panel.width, root.status.panel.height)
              : "unknown") + " · not a physical measurement"
          }

          PanelSeparator { width: parent.width }
          PanelText {
            text: "Next-start resolution and FPS"
            font.bold: true
          }
          PanelText {
            text: root.status ? "Saved: " + root.status.sizing.preset + " · "
              + root.dimensions(root.status.sizing.width, root.status.sizing.height)
              + " · " + root.status.sizing.fps + " FPS" : "Saved settings unavailable."
          }
          Flow {
            width: parent.width
            spacing: Style.spacing.sm
            Repeater {
              model: ["720p", "1080p", "native"]
              delegate: Button {
                required property string modelData
                text: modelData === "native" ? "Native" : modelData
                focusable: true
                bordered: true
                selected: root.draftPreset === modelData
                enabled: !root.busy && root.status !== null
                  && (modelData !== "native" || root.status.sizing.nativeAvailable)
                onClicked: {
                  root.draftPreset = modelData
                  root.draftChanged = true
                }
              }
            }
          }
          PanelText {
            text: root.status && root.status.sizing.nativeAvailable
              ? "Native (Samsung device API): " + root.dimensions(root.status.sizing.nativeWidth, root.status.sizing.nativeHeight)
              : "TV-reported native resolution unknown — Native is unavailable."
          }
          Row {
            spacing: Style.spacing.sm
            Repeater {
              model: [15, 30, 60]
              delegate: Button {
                required property int modelData
                text: modelData + " FPS"
                focusable: true
                bordered: true
                selected: root.draftFps === modelData
                enabled: !root.busy && root.status !== null
                onClicked: {
                  root.draftFps = modelData
                  root.draftChanged = true
                }
              }
            }
          }
          Button {
            text: "Save for next start"
            focusable: true
            bordered: true
            enabled: !root.busy && root.status !== null && root.draftChanged
              && (root.draftPreset !== "native" || root.status.sizing.nativeAvailable)
            onClicked: root.applySizing()
          }
          PanelText {
            text: root.status && root.status.sizing.requiresRestart
              ? "Pending changes require Stop, then Start. Saving never restarts or interrupts the TV automatically."
              : "Sizing changes apply on the next start. If running, explicitly Stop, then Start; saving never interrupts the TV."
          }
        }
      }
    }
  }
}
