import QtQuick
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui

Panel {
    id: root
    moduleName: "raw.omapower"
    manageIpc: false
    property var anchorItem: null
    property var hostWidget: null
    property bool adding: false
    property bool addingInFlight: false

    function submitCommand() {
        if (hostWidget && hostWidget.editWhitelist("--add", commandField.text)) addingInFlight = true
    }
    function whitelistEditFinished(success) {
        if (!addingInFlight) return
        addingInFlight = false
        if (success) { adding = false; commandField.text = "" }
    }

    Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }
    function switchPanel(direction) {
        if (root.bar && typeof root.bar.switchPanelFrom === "function")
            return root.bar.switchPanelFrom(root.hostWidget || root, direction)
        return false
    }

    KeyboardPanel {
        id: panel
        anchorItem: root.anchorItem
        owner: root.hostWidget || root
        bar: root.bar
        open: root.opened
        focusTarget: keyCatcher
        contentWidth: panel.fittedContentWidth(Style.space(340))
        contentHeight: panel.fittedContentHeight(content.implicitHeight)

        PanelKeyCatcher {
            id: keyCatcher
            anchors.fill: parent
            onCloseRequested: root.close()
            onTabRequested: function(direction) { root.switchPanel(direction) }

            Column {
                id: content
                width: parent.width
                spacing: Style.space(10)

                Text {
                    width: parent.width
                    text: strings.t("title")
                    textFormat: Text.PlainText
                    color: root.barForeground
                    font.family: root.bar ? root.bar.fontFamily : Style.font.family
                    font.pixelSize: Style.font.subtitle
                    font.bold: true
                }
                Toggle {
                    width: parent.width
                    label: strings.t("service.label")
                    description: root.hostWidget ? root.hostWidget.description : strings.t("status.unknown")
                    checked: root.hostWidget ? root.hostWidget.serviceState === "active" : false
                    enabled: !!root.hostWidget && !root.hostWidget.actionBusy && root.hostWidget.serviceState !== "unknown"
                             && root.hostWidget.serviceState !== "activating" && root.hostWidget.serviceState !== "deactivating"
                    foreground: root.barForeground
                    fontFamily: root.bar ? root.bar.fontFamily : Style.font.family
                    onClicked: root.hostWidget.toggleService()
                }
                Text {
                    width: parent.width
                    visible: !!root.hostWidget && root.hostWidget.actionError !== ""
                    text: root.hostWidget ? root.hostWidget.actionError : ""
                    textFormat: Text.PlainText
                    color: root.barForeground
                    wrapMode: Text.WordWrap
                }
                Text {
                    width: parent.width
                    text: strings.t("whitelist.title")
                    textFormat: Text.PlainText
                    color: root.barForeground
                    font.family: root.bar ? root.bar.fontFamily : Style.font.family
                    font.bold: true
                }
                Text {
                    width: parent.width
                    visible: !!root.hostWidget && (root.hostWidget.whitelistError !== "" || root.hostWidget.whitelistActionError !== "")
                    text: root.hostWidget ? root.hostWidget.whitelistError || root.hostWidget.whitelistActionError : ""
                    textFormat: Text.PlainText
                    color: root.barForeground
                    wrapMode: Text.WordWrap
                }
                Flickable {
                    id: chipViewport
                    width: parent.width
                    readonly property real inset: Style.space(3)
                    height: Math.min(Style.space(180), Math.max(Style.space(32), chips.implicitHeight + inset * 2))
                    contentWidth: width
                    contentHeight: chips.implicitHeight + inset * 2
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Flow {
                        id: chips
                        x: chipViewport.inset
                        y: chipViewport.inset
                        width: Math.max(0, chipViewport.width - chipViewport.inset * 2)
                        spacing: Style.space(6)
                        Repeater {
                            model: root.hostWidget ? root.hostWidget.commands : []
                            delegate: BorderSurface {
                                id: chip
                                required property string modelData
                                implicitWidth: chipLabel.implicitWidth + removeButton.implicitWidth + Style.space(12)
                                implicitHeight: Math.max(Style.space(30), removeButton.implicitHeight)
                                radius: Style.cornerRadius
                                color: "transparent"
                                borderSpec: Border.controlSpec("normal", root.barForeground, Commons.Color.accent)
                                Text {
                                    id: chipLabel
                                    anchors.left: parent.left
                                    anchors.leftMargin: Style.space(7)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: chip.modelData
                                    textFormat: Text.PlainText
                                    color: root.barForeground
                                    font.family: root.bar ? root.bar.fontFamily : Style.font.family
                                    font.pixelSize: Style.font.body
                                }
                                Button {
                                    id: removeButton
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "×"
                                    tooltipText: strings.t("whitelist.remove")
                                    focusable: true
                                    horizontalPadding: Style.space(5)
                                    foreground: root.barForeground
                                    enabled: !!root.hostWidget && !root.hostWidget.whitelistBusy && !root.hostWidget.whitelistError
                                    onClicked: root.hostWidget.editWhitelist("--remove", chip.modelData)
                                }
                            }
                        }
                        Button {
                            text: "+"
                            bordered: true
                            focusable: true
                            tooltipText: strings.t("whitelist.add")
                            foreground: root.barForeground
                            enabled: !!root.hostWidget && !root.hostWidget.whitelistBusy && !root.hostWidget.whitelistError
                            onClicked: { root.adding = true; Qt.callLater(function() { commandField.forceActiveFocus() }) }
                        }
                    }
                }
                Row {
                    width: parent.width
                    spacing: Style.space(6)
                    visible: root.adding
                    TextField {
                        id: commandField
                        width: Math.max(Style.space(80), parent.width - addButton.implicitWidth - cancelButton.implicitWidth - parent.spacing * 2)
                        placeholderText: strings.t("whitelist.placeholder")
                        foreground: root.barForeground
                        enabled: !!root.hostWidget && !root.hostWidget.whitelistBusy
                        onAccepted: root.submitCommand()
                    }
                    Button {
                        id: addButton
                        text: strings.t("whitelist.save")
                        bordered: true
                        focusable: true
                        foreground: root.barForeground
                        enabled: !!root.hostWidget && !root.hostWidget.whitelistBusy
                        onClicked: root.submitCommand()
                    }
                    Button {
                        id: cancelButton
                        text: "×"
                        focusable: true
                        foreground: root.barForeground
                        enabled: !!root.hostWidget && !root.hostWidget.whitelistBusy
                        onClicked: { root.adding = false; commandField.text = ""; root.hostWidget.whitelistActionError = "" }
                    }
                }
                Text {
                    width: parent.width
                    text: strings.t("service.hint")
                    textFormat: Text.PlainText
                    color: root.barForeground
                    opacity: 0.7
                    font.pixelSize: Style.font.caption
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
