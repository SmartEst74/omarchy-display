import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

BarWidget {
  id: root
  moduleName: "smartest74.omarchy-display"

  property string displayState: "starting"
  property int clients: 0
  property bool pollPending: false
  property bool responseComplete: false
  property bool processComplete: false
  property string responseText: ""
  property var statusData: null
  readonly property bool opened: controls.opened
  readonly property bool popoutSwitchClosing: controls.popoutSwitchClosing

  function open() { controls.open() }
  function close() { controls.close() }
  function closeForPopoutSwitch() { controls.closeForPopoutSwitch() }

  readonly property string statusDescription: {
    switch (displayState) {
      case "stopped": return "Stopped — virtual display is off."
      case "starting": return "Starting or checking status — sharing is not confirmed."
      case "ready": return "Ready — virtual output is available; no TV viewer is connected."
      case "sharing": return "Sharing — " + clients + " authenticated TV viewer" + (clients === 1 ? " is" : "s are") + " connected to the capture backend."
      default: return "Error — display status is unavailable or the host reported an error; sharing is not confirmed."
    }
  }
  readonly property color stateColor: displayState === "error"
    ? (bar ? bar.urgent : Commons.Color.urgent)
    : (displayState === "ready" || displayState === "sharing"
      ? Commons.Color.accent : button.foreground)

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  function failStatus() {
    pollPending = false
    deadline.stop()
    clients = 0
    statusData = null
    displayState = "error"
  }

  function refresh() {
    if (pollPending || statusProc.running) return
    responseText = ""
    responseComplete = false
    processComplete = false
    pollPending = true
    deadline.restart()
    statusProc.running = true
  }

  function positiveInteger(value) {
    return typeof value === "number" && isFinite(value)
      && value > 0 && value <= 2147483647 && Math.floor(value) === value
  }

  function optionalDimension(value) {
    return value === null || positiveInteger(value)
  }

  function validDetails(value) {
    if (!optionalDimension(value.width) || !optionalDimension(value.height)
        || !optionalDimension(value.fps) || !Array.isArray(value.workspaces)
        || !(value.tvWorkspace === null || positiveInteger(value.tvWorkspace))) return false
    var seen = {}
    for (var i = 0; i < value.workspaces.length; i++) {
      var workspace = value.workspaces[i]
      if (!workspace || !positiveInteger(workspace.id)
          || typeof workspace.windows !== "number" || !isFinite(workspace.windows)
          || workspace.windows < 0 || Math.floor(workspace.windows) !== workspace.windows
          || typeof workspace.onTV !== "boolean"
          || typeof workspace.recallAvailable !== "boolean" || seen[workspace.id]) return false
      seen[workspace.id] = true
    }
    var panel = value.panel
    if (panel !== null && (!panel || !positiveInteger(panel.width)
        || !positiveInteger(panel.height) || panel.source !== "samsung-device-api")) return false
    var receiver = value.receiver
    if (receiver !== null && (!receiver || !positiveInteger(receiver.viewportWidth)
        || !positiveInteger(receiver.viewportHeight)
        || !optionalDimension(receiver.panelWidth)
        || !optionalDimension(receiver.panelHeight))) return false
    var sizing = value.sizing
    return sizing && ["720p", "1080p", "native"].indexOf(sizing.preset) >= 0
      && positiveInteger(sizing.width) && positiveInteger(sizing.height)
      && [15, 30, 60].indexOf(sizing.fps) >= 0
      && Array.isArray(sizing.presets) && sizing.presets.length === 2
      && sizing.presets.indexOf("720p") >= 0 && sizing.presets.indexOf("1080p") >= 0
      && typeof sizing.nativeAvailable === "boolean"
      && optionalDimension(sizing.nativeWidth) && optionalDimension(sizing.nativeHeight)
      && (!sizing.nativeAvailable
        || (positiveInteger(sizing.nativeWidth) && positiveInteger(sizing.nativeHeight)))
      && typeof sizing.requiresRestart === "boolean"
  }

  function acceptResponse() {
    // stdout completion and process exit can arrive in either order. Never
    // publish a successful-looking payload before a successful process exit.
    if (!pollPending || !responseComplete || !processComplete) return
    var value
    try {
      value = JSON.parse(responseText)
    } catch (error) {
      failStatus()
      return
    }
    var states = ["stopped", "starting", "ready", "sharing", "error"]
    if (!value || typeof value !== "object" || Array.isArray(value)
        || value.schemaVersion !== 1 || states.indexOf(value.state) < 0
        || typeof value.running !== "boolean"
        || typeof value.clients !== "number" || !isFinite(value.clients)
        || value.clients < 0 || Math.floor(value.clients) !== value.clients
        || !(value.output === null || typeof value.output === "string")
        || typeof value.message !== "string"
        || value.viewOnly !== true || value.audio !== false
        || (value.state === "stopped" && value.running)
        || (value.state !== "sharing" && value.clients !== 0)
        || ((value.state === "ready" || value.state === "sharing")
          && (!value.running || typeof value.output !== "string" || value.output.length === 0))
        || (value.state === "sharing" && value.clients === 0)
        || !validDetails(value)) {
      failStatus()
      return
    }
    deadline.stop()
    pollPending = false
    clients = value.clients
    displayState = value.state
    // Keep only public control data. Never expose message, output or raw logs
    // to the popup, even if the host accidentally changes their contents.
    statusData = {
      running: value.running, width: value.width, height: value.height, fps: value.fps,
      tvWorkspace: value.tvWorkspace,
      workspaces: value.workspaces.map(function(workspace) {
        return {
          id: workspace.id, windows: workspace.windows, onTV: workspace.onTV,
          recallAvailable: workspace.recallAvailable
        }
      }),
      panel: value.panel === null ? null : {
        width: value.panel.width, height: value.panel.height, source: value.panel.source
      },
      receiver: value.receiver === null ? null : {
        viewportWidth: value.receiver.viewportWidth, viewportHeight: value.receiver.viewportHeight,
        panelWidth: value.receiver.panelWidth, panelHeight: value.receiver.panelHeight
      },
      sizing: {
        preset: value.sizing.preset, width: value.sizing.width, height: value.sizing.height,
        fps: value.sizing.fps, nativeAvailable: value.sizing.nativeAvailable,
        nativeWidth: value.sizing.nativeWidth, nativeHeight: value.sizing.nativeHeight,
        requiresRestart: value.sizing.requiresRestart
      }
    }
  }

  DisplayPanel {
    id: controls
    bar: root.bar
    anchorItem: button
    hostWidget: root
    status: root.statusData
    description: root.statusDescription
  }

  Process {
    id: statusProc
    command: ["omarchy-display", "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        if (!root.pollPending) return
        root.responseText = text
        root.responseComplete = true
        root.acceptResponse()
      }
    }
    // Consume diagnostics without displaying or forwarding raw host output.
    stderr: StdioCollector { waitForEnd: true }
    onExited: function(exitCode, exitStatus) {
      if (!root.pollPending) return
      if (exitCode !== 0 || exitStatus !== 0) {
        root.failStatus()
        return
      }
      root.processComplete = true
      root.acceptResponse()
    }
  }

  Timer {
    id: deadline
    interval: 4000
    onTriggered: {
      root.failStatus()
      statusProc.running = false
    }
  }

  Timer {
    interval: 5000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "\uf108"
    slotSize: Style.bar.statusSlot
    dimmed: root.displayState === "stopped"
    active: root.displayState === "sharing" || root.displayState === "error"
    activeColor: root.stateColor
    tooltipText: "Omarchy Display\n" + root.statusDescription
      + "\nIndependent TV workspace · view-only · no audio\nClick for controls; pairing requires an explicit action."
    onTooltipTextChanged: {
      if (tooltipHovered && root.bar) root.bar.showTooltip(button, tooltipText)
    }
    onPressed: function(mouseButton) {
      if (mouseButton === Qt.LeftButton) controls.toggle()
    }

    Rectangle {
      anchors.right: parent.right
      anchors.bottom: parent.bottom
      anchors.rightMargin: Style.space(2)
      anchors.bottomMargin: Style.space(3)
      width: Style.space(6)
      height: width
      radius: width / 2
      visible: root.displayState === "ready" || root.displayState === "sharing"
      color: root.displayState === "sharing" ? root.stateColor : "transparent"
      border.color: root.stateColor
      border.width: Math.max(1, Style.space(1))
    }

    Text {
      anchors.right: parent.right
      anchors.bottom: parent.bottom
      anchors.rightMargin: Style.space(1)
      anchors.bottomMargin: Style.space(1)
      visible: root.displayState === "error" || root.displayState === "starting"
      text: root.displayState === "error" ? "!" : "…"
      textFormat: Text.PlainText
      color: root.stateColor
      font.family: button.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: true
    }
  }
}
