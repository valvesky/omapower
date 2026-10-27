import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Services.UPower
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Whitelist.js" as Whitelist

BarWidget {
    id: root
    moduleName: "raw.omapower"

    property string serviceState: "unknown"
    readonly property string powerProfile: {
        switch (PowerProfiles.profile) {
        case PowerProfile.Performance: return "performance"
        case PowerProfile.Balanced: return "balanced"
        case PowerProfile.PowerSaver: return "power-saver"
        default: return "unknown"
        }
    }
    property bool holding: false
    property bool suppressed: false
    property bool daemonAvailable: false
    property string detectionMode: ""
    property bool statusValid: false
    property var commands: []
    property string whitelistError: ""
    property string whitelistActionError: ""
    property bool whitelistBusy: false
    property string actionError: ""
    property bool actionBusy: false
    readonly property bool opened: panelLoader.item ? panelLoader.item.opened : false
    readonly property bool popoutSwitchClosing: panelLoader.item ? panelLoader.item.popoutSwitchClosing : false
    readonly property string configHome: Quickshell.env("XDG_CONFIG_HOME") || (Quickshell.env("HOME") + "/.config")
    readonly property string description: (serviceState === "unknown" ? strings.t("status.unknown")
        : serviceState === "failed" ? strings.t("status.failed")
        : serviceState !== "active" ? strings.t("status.off")
        : !statusValid ? strings.t("status.starting")
        : !daemonAvailable ? strings.t("status.daemonMissing")
        : holding ? strings.t("status.holding")
        : suppressed ? strings.t("status.manual") : strings.t("status.idle"))
        + (serviceState === "active" && statusValid && detectionMode === "polling" ? " — " + strings.t("status.polling") : "")

    Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

    function applyWhitelist(state) {
        const result = state.configError ? null : Whitelist.parse(JSON.stringify(state))
        commands = result ? result.commands : []
        whitelistError = result ? "" : strings.t("whitelist.error")
    }
    function editWhitelist(action, name) {
        if (whitelistBusy || whitelistError) return false
        if (!Whitelist.validName(name)) { whitelistActionError = strings.t("whitelist.invalid"); return false }
        whitelistActionError = ""
        whitelistBusy = true
        whitelistAction.command = ["sh", "-c", "d=$(readlink -f -- \"$1\") || exit 1; exec \"$d/../omapower\" \"$2\" \"$3\"", "omapower", root.configHome + "/omarchy/plugins/raw.omapower", action, name]
        whitelistAction.running = true
        return true
    }
    function open() {
        if (!panelLoader.item) return
        if (!serviceMonitor.running) { monitorRetry.stop(); serviceMonitor.running = true }
        statusFile.reload()
        panelLoader.item.open()
    }
    function close() { if (panelLoader.item) panelLoader.item.close() }
    function toggle() { if (opened) close(); else open() }
    function closeForPopoutSwitch() { if (panelLoader.item) panelLoader.item.closeForPopoutSwitch() }
    function injectPanel() {
        if (!panelLoader.item) return
        panelLoader.item.bar = root.bar
        panelLoader.item.anchorItem = button
        panelLoader.item.hostWidget = root
    }
    function toggleService() {
        if (actionBusy || serviceState === "unknown" || serviceState === "activating" || serviceState === "deactivating") return
        actionError = ""
        actionBusy = true
        serviceAction.command = ["systemctl", "--user", serviceState === "active" ? "stop" : "start", "omapower.service"]
        serviceAction.running = true
    }

    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight
    onBarChanged: injectPanel()
    onServiceStateChanged: {
        if (serviceState === "active") statusFile.reload()
        else statusValid = false
    }
    Component.onCompleted: serviceMonitor.running = true

    Loader {
        id: panelLoader
        active: true
        visible: false
        source: Qt.resolvedUrl("Panel.qml")
        onLoaded: { root.injectPanel(); Qt.callLater(root.injectPanel) }
        onStatusChanged: if (status === Loader.Error) console.error("Omapower: panel failed to load")
    }
    WidgetButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        // Font Awesome glyphs follow the bar foreground instead of emoji rendering.
        fontFamily: "Font Awesome 7 Free Solid"
        centerFigures: false
        text: root.powerProfile === "performance" ? "\uf0e7"
            : root.powerProfile === "balanced" ? "\uf6ad" : root.powerProfile === "power-saver" ? "\uf1b8" : "\uf059"
        tooltipText: root.description
        onPressed: function(buttonCode) { if (buttonCode === Qt.LeftButton) root.toggle() }
    }

    Process {
        id: serviceMonitor
        command: ["sh", "-c", "d=$(readlink -f -- \"$1\") || exit 1; exec \"$d/../omapower\" --watch-widget", "omapower", root.configHome + "/omarchy/plugins/raw.omapower"]
        stdout: SplitParser {
            onRead: function(data) {
                try {
                    const state = JSON.parse(data)
                    if (typeof state.service === "string") root.serviceState = state.service
                    if (typeof state.configError === "boolean") root.applyWhitelist(state)
                } catch (e) { root.serviceState = "unknown" }
            }
        }
        onExited: { root.serviceState = "unknown"; monitorRetry.restart() }
    }
    // Recovery only: no timer runs while the D-Bus monitor is healthy.
    Timer { id: monitorRetry; interval: 5000; repeat: false; onTriggered: serviceMonitor.running = true }
    Process {
        id: serviceAction
        onExited: function(code) {
            root.actionBusy = false
            root.actionError = code === 0 ? "" : strings.t("action.failed")
        }
    }
    Process {
        id: whitelistAction
        onExited: function(code) {
            root.whitelistBusy = false
            root.whitelistActionError = code === 0 ? "" : strings.t("whitelist.saveFailed")
            if (panelLoader.item) panelLoader.item.whitelistEditFinished(code === 0)
        }
    }
    FileView {
        id: statusFile
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/omapower.json"
        printErrors: false
        watchChanges: true
        onFileChanged: reload()
        onLoaded: {
            try {
                const state = JSON.parse(text())
                if (typeof state.holding !== "boolean" || typeof state.suppressed !== "boolean") throw new Error("bad status")
                root.holding = state.holding
                root.suppressed = state.suppressed
                root.daemonAvailable = state.available !== false
                root.detectionMode = state.monitor || ""
                root.statusValid = true
            } catch (e) { root.statusValid = false }
        }
        onLoadFailed: root.statusValid = false
    }
}
