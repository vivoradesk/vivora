import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: dlg
    width: 720
    height: 600
    minimumWidth: 640
    minimumHeight: 480
    title: "Vivora — Settings"
    color: theme.bg

    signal closed()
    onVisibleChanged: if (!visible) closed()

    // Local theme — mirrors main.qml (no shared QML singleton yet).
    QtObject {
        id: theme
        // Palette tuned toward the mockup — warmer-neutral, less yellow.
        readonly property color bg:        "#efece3"   // panel background
        readonly property color sidebar:   "#e8e3d6"   // slightly darker sidebar
        readonly property color hoverBg:   "#ded8c8"   // row / button hover
        readonly property color ctrlBg:    "#fbfaf5"   // near-white control fill
        readonly property color selected:  "#1a1a1f"   // selected sidebar item
        readonly property color selFg:     "#ffffff"
        readonly property color text:      "#1a1a1f"
        readonly property color textMuted: "#6f6b60"
        readonly property color border:    "#d4cdba"
        readonly property color accent:    "#3D6BFA"
        readonly property color warn:      "#b6801b"   // amber notice
        readonly property string monoFont: "JetBrains Mono, Cascadia Mono, Consolas, monospace"
    }

    property int currentIndex: 3   // default to Network (most-used)

    // Sidebar model: section headers + items.  `header` rows are
    // non-selectable group labels; `index` rows map to the panel stack.
    // Monochrome glyphs only — colour-emoji codepoints (🔗 etc.) force
    // emoji presentation and clash with the flat sidebar.  The ︎
    // variation selector pins any emoji-capable glyph to text style.
    readonly property var navModel: [
        { header: "GENERAL" },
        { icon: "◐", label: "Account",          index: 0 },
        { icon: "▢", label: "Appearance",       index: 1 },
        { icon: "⌘", label: "Shortcuts",        index: 2 },
        { icon: "↻", label: "Startup",          index: 8 },
        { header: "SESSION" },
        { icon: "⇄", label: "Network",          index: 3 },
        { icon: "▤", label: "Hosting",          index: 4 },
        { icon: "▷", label: "Viewing",          index: 9 },
        { icon: "✓", label: "Security",         index: 5 },
        { header: "ADVANCED" },
        { icon: "∞", label: "Self-hosted relay", index: 6 },
        { icon: "ⓘ", label: "About",            index: 7 }
    ]

    // Helper components ----------------------------------------------------

    component SectionTitle: ColumnLayout {
        property string title: ""
        property string subtitle: ""
        Layout.fillWidth: true
        spacing: 4
        Label {
            text: title
            color: theme.text
            font.pixelSize: 22
            font.bold: true
        }
        Label {
            text: subtitle
            visible: subtitle.length > 0
            color: theme.textMuted
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.bottomMargin: 8
        }
    }

    // A labelled row: title + helper text on the left, control(s) on the
    // right.  Mirrors the mockup's two-column field layout.  Controls
    // injected by the caller land in the inner `ctrl` RowLayout, so they
    // MUST size via Layout.preferredWidth (plain `width:` is ignored by
    // the layout).  A trailing spacer keeps them left-aligned.
    component Field: RowLayout {
        property string title: ""
        property string help: ""
        default property alias control: ctrl.data
        Layout.fillWidth: true
        Layout.topMargin: 12
        spacing: 16
        ColumnLayout {
            Layout.preferredWidth: 180
            Layout.alignment: Qt.AlignTop
            spacing: 2
            Label {
                text: title
                color: theme.text
                font.pixelSize: 13
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: help
                visible: help.length > 0
                color: theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
        // Controls left-align into a single column at a fixed offset
        // (right after the 180px label column) so they don't jump with
        // window width.  The RowLayout still fills width, leaving the
        // empty space on the right.
        RowLayout {
            id: ctrl
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 6
        }
    }

    component CreamField: TextField {
        implicitHeight: 38            // match combo / spin / browse height
        color: theme.text
        placeholderTextColor: theme.textMuted
        verticalAlignment: TextInput.AlignVCenter
        leftPadding: 12
        selectByMouse: true
        selectionColor: theme.accent
        selectedTextColor: "#ffffff"
        font.pixelSize: 13
        background: Rectangle {
            color: theme.ctrlBg
            border.color: parent.activeFocus ? theme.accent : theme.border
            border.width: 1
            radius: 8
        }
    }

    // Cream-styled combo box.  `entries` is an array of
    // { text, sub?, badge? } — sub shows mono after a · separator,
    // badge shows small-caps at the row's right edge.  Selected row
    // gets a leading ✓.
    component CreamCombo: ComboBox {
        id: combo
        property var entries: []
        Layout.preferredWidth: 250
        model: entries
        textRole: "text"
        font.pixelSize: 13

        contentItem: Label {
            leftPadding: 12
            rightPadding: 34
            text: combo.displayText
            color: theme.text
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            implicitHeight: 38
            color: theme.ctrlBg
            border.color: combo.activeFocus || combo.hovered ? theme.accent : theme.border
            border.width: 1
            radius: 8
        }
        indicator: Label {
            x: combo.width - width - 12
            y: (combo.height - height) / 2
            text: combo.popup.visible ? "▲" : "▼"
            color: theme.textMuted
            font.pixelSize: 9
        }
        delegate: ItemDelegate {
            width: combo.width
            height: (modelData.sub !== undefined && modelData.sub.length > 0) ? 46 : 38
            highlighted: combo.highlightedIndex === index
            background: Rectangle {
                color: highlighted ? theme.hoverBg : "transparent"
                radius: 6
            }
            contentItem: RowLayout {
                spacing: 6
                Label {
                    text: combo.currentIndex === index ? "✓" : "   "
                    color: theme.text
                    font.pixelSize: 12
                    Layout.preferredWidth: 14
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    RowLayout {
                        spacing: 6
                        Label {
                            text: modelData.text
                            color: theme.text
                            font.pixelSize: 13
                            font.bold: combo.currentIndex === index
                        }
                        Label {
                            visible: modelData.sub !== undefined && modelData.sub.length > 0
                            text: modelData.sub !== undefined ? "· " + modelData.sub : ""
                            color: theme.textMuted
                            font.pixelSize: 11
                            font.family: theme.monoFont
                        }
                    }
                }
                Label {
                    visible: modelData.badge !== undefined && modelData.badge.length > 0
                    text: modelData.badge !== undefined ? modelData.badge : ""
                    color: theme.textMuted
                    font.pixelSize: 10
                    font.letterSpacing: 1
                }
            }
        }
        popup: Popup {
            y: combo.height + 4
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + 12, 320)
            padding: 6
            background: Rectangle {
                color: theme.ctrlBg
                border.color: theme.border
                border.width: 1
                radius: 10
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator {}
            }
        }
    }

    // Cream-styled stepper: [−] value [+].  (Inline components can't be
    // nested, so the two step buttons are spelled out rather than shared.)
    component CreamSpin: RowLayout {
        id: spin
        property int from: 0
        property int to: 100
        property int value: 0
        signal modified(int v)
        spacing: 0

        // Decrement — rounded on the left edge only, joins the value
        // cell on the right (per-corner radius is Qt 6.7+).
        Rectangle {
            implicitWidth: 38; implicitHeight: 38
            topLeftRadius: 8; bottomLeftRadius: 8
            topRightRadius: 0; bottomRightRadius: 0
            color: decHover.hovered ? theme.hoverBg : theme.ctrlBg
            border.color: theme.border
            border.width: 1
            HoverHandler { id: decHover }
            Label { anchors.centerIn: parent; text: "−"; color: theme.text; font.pixelSize: 16 }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: { var nv = Math.max(spin.from, spin.value - 1); if (nv !== spin.value) spin.modified(nv) }
            }
        }
        // Value cell — flat, shares edges with the buttons.  -1 left
        // margin overlaps the borders so the divider is a single line.
        Rectangle {
            implicitWidth: 56; implicitHeight: 38
            Layout.leftMargin: -1
            Layout.rightMargin: -1
            color: theme.ctrlBg
            border.color: theme.border
            border.width: 1
            Label {
                anchors.centerIn: parent
                text: spin.value
                color: theme.text
                font.pixelSize: 13
                font.family: theme.monoFont
            }
        }
        // Increment — rounded on the right edge only.
        Rectangle {
            implicitWidth: 38; implicitHeight: 38
            topLeftRadius: 0; bottomLeftRadius: 0
            topRightRadius: 8; bottomRightRadius: 8
            color: incHover.hovered ? theme.hoverBg : theme.ctrlBg
            border.color: theme.border
            border.width: 1
            HoverHandler { id: incHover }
            Label { anchors.centerIn: parent; text: "+"; color: theme.text; font.pixelSize: 16 }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: { var nv = Math.min(spin.to, spin.value + 1); if (nv !== spin.value) spin.modified(nv) }
            }
        }
    }

    // Cream-themed slider with a value readout.  `zeroLabel` names the
    // 0 position (e.g. "Auto") since 0 conventionally means "no manual
    // override" in our settings.
    component CreamSlider: RowLayout {
        id: cslider
        property int from: 0
        property int to: 100
        property int value: 0
        property string suffix: ""
        property string zeroLabel: ""
        signal modified(int v)
        spacing: 12
        Slider {
            id: sl
            Layout.preferredWidth: 170
            from: cslider.from
            to: cslider.to
            stepSize: 1
            value: cslider.value
            onMoved: {
                var nv = Math.round(sl.value)
                if (nv !== cslider.value) cslider.modified(nv)
            }
            background: Rectangle {
                x: sl.leftPadding
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: sl.availableWidth
                height: 4
                radius: 2
                color: theme.border
                Rectangle {
                    width: sl.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: theme.accent
                }
            }
            handle: Rectangle {
                x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
                y: sl.topPadding + sl.availableHeight / 2 - height / 2
                width: 18; height: 18; radius: 9
                color: sl.pressed ? theme.hoverBg : theme.ctrlBg
                border.color: theme.border
                border.width: 1
            }
        }
        Label {
            text: cslider.value === 0 && cslider.zeroLabel !== ""
                  ? cslider.zeroLabel
                  : cslider.value + cslider.suffix
            color: theme.text
            font.pixelSize: 13
            font.family: theme.monoFont
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: 64
        }
    }

    // Cream toggle switch.
    component CreamSwitch: Switch {
        // Use default Switch behaviour; just recolour the groove/handle.
    }

    // Layout ---------------------------------------------------------------

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Sidebar ──────────────────────────────────────────────────
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 170
            color: theme.sidebar

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 14
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 2

                Repeater {
                    model: dlg.navModel
                    delegate: Item {
                        Layout.fillWidth: true
                        implicitHeight: modelData.header !== undefined ? 28 : 32

                        // Group header row
                        Label {
                            visible: modelData.header !== undefined
                            text: modelData.header || ""
                            color: theme.textMuted
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 1
                            anchors.left: parent.left
                            anchors.leftMargin: 6
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 4
                        }

                        // Selectable item row
                        Rectangle {
                            visible: modelData.index !== undefined
                            anchors.fill: parent
                            radius: 7
                            color: modelData.index === dlg.currentIndex
                                     ? theme.selected
                                     : (itemHover.hovered ? theme.hoverBg : "transparent")
                            HoverHandler { id: itemHover }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                Label {
                                    text: modelData.icon || ""
                                    color: modelData.index === dlg.currentIndex ? theme.selFg : theme.textMuted
                                    font.pixelSize: 13
                                    // Fixed width + centred so the variable-
                                    // width glyphs don't shift the labels.
                                    Layout.preferredWidth: 18
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Label {
                                    text: modelData.label || ""
                                    color: modelData.index === dlg.currentIndex ? theme.selFg : theme.text
                                    font.pixelSize: 13
                                    Layout.fillWidth: true
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: dlg.currentIndex = modelData.index
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }

        // ── Panel ────────────────────────────────────────────────────
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: panelStack.implicitHeight + 56
            clip: true
            ScrollBar.vertical: ScrollBar {}

            StackLayout {
                id: panelStack
                width: parent.width - 56
                x: 28
                y: 28
                currentIndex: dlg.currentIndex

                // ── 0: Account (VIV-31) ──────────────────────────────
                ColumnLayout {
                    id: accountSection
                    spacing: 0
                    property string accountErr: ""
                    property bool   busy: false
                    Connections {
                        target: App
                        function onAccountError(message) { accountSection.accountErr = message; accountSection.busy = false }
                        function onAccountChanged()       { accountSection.accountErr = ""; accountSection.busy = false }
                    }
                    SectionTitle {
                        title: "Account"
                        subtitle: "Sign in to sync your devices and unlock Pro features."
                    }

                    // Signed OUT — login / create account.
                    ColumnLayout {
                        visible: !App.accountLoggedIn
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: 10
                        CreamField {
                            id: emailField
                            Layout.preferredWidth: 300
                            placeholderText: "Email"
                            inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                        }
                        CreamField {
                            id: pwField
                            Layout.preferredWidth: 300
                            placeholderText: "Password"
                            echoMode: TextInput.Password
                            onAccepted: if (!accountSection.busy && text.length > 0) {
                                accountSection.busy = true
                                App.logIn(emailField.text.trim(), pwField.text)
                            }
                        }
                        Label {
                            visible: accountSection.accountErr !== ""
                            text: accountSection.accountErr
                            color: theme.warn
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.preferredWidth: 300
                        }
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                Layout.preferredHeight: 38
                                Layout.preferredWidth: loginLbl.implicitWidth + 32
                                radius: 8; color: theme.accent
                                opacity: accountSection.busy ? 0.5 : 1
                                Label { id: loginLbl; anchors.centerIn: parent; text: "Log in"
                                        color: "#ffffff"; font.pixelSize: 13; font.bold: true }
                                MouseArea { anchors.fill: parent; enabled: !accountSection.busy
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: { accountSection.busy = true
                                                 App.logIn(emailField.text.trim(), pwField.text) } }
                            }
                            Rectangle {
                                Layout.preferredHeight: 38
                                Layout.preferredWidth: signupLbl.implicitWidth + 32
                                radius: 8; color: theme.ctrlBg
                                border.color: theme.border; border.width: 1
                                opacity: accountSection.busy ? 0.5 : 1
                                Label { id: signupLbl; anchors.centerIn: parent; text: "Create account"
                                        color: theme.text; font.pixelSize: 13 }
                                MouseArea { anchors.fill: parent; enabled: !accountSection.busy
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: { accountSection.busy = true
                                                 App.signUp(emailField.text.trim(), pwField.text) } }
                            }
                            BusyIndicator {
                                running: accountSection.busy
                                visible: accountSection.busy
                                Layout.preferredHeight: 26
                                Layout.preferredWidth: 26
                            }
                        }
                    }

                    // Signed IN — account + license + actions.
                    ColumnLayout {
                        visible: App.accountLoggedIn
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        spacing: 10
                        Field {
                            title: "Signed in"
                            Label { text: App.accountEmail; color: theme.text; font.family: theme.monoFont }
                        }
                        Field {
                            title: "Plan"
                            Rectangle {
                                Layout.preferredHeight: 24
                                Layout.preferredWidth: planLbl.implicitWidth + 22
                                radius: 999
                                color: App.licensePro ? Qt.rgba(0.122, 0.643, 0.388, 0.14)
                                                      : theme.hoverBg
                                Label {
                                    id: planLbl
                                    anchors.centerIn: parent
                                    text: App.licensePro ? ("PRO · expires " + App.licenseExpiry) : "FREE"
                                    color: App.licensePro ? "#1FA463" : theme.textMuted
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }
                        }
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                visible: !App.licensePro
                                Layout.preferredHeight: 34
                                Layout.preferredWidth: upLbl.implicitWidth + 28
                                radius: 8; color: theme.accent
                                Label { id: upLbl; anchors.centerIn: parent; text: "Upgrade to Pro"
                                        color: "#ffffff"; font.pixelSize: 13; font.bold: true }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: App.openUpgradePage() }
                            }
                            Rectangle {
                                Layout.preferredHeight: 34
                                Layout.preferredWidth: refLbl.implicitWidth + 28
                                radius: 8; color: theme.ctrlBg
                                border.color: theme.border; border.width: 1
                                Label { id: refLbl; anchors.centerIn: parent; text: "Refresh license"
                                        color: theme.text; font.pixelSize: 13 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: App.refreshLicenseFromCloud() }
                            }
                            Rectangle {
                                Layout.preferredHeight: 34
                                Layout.preferredWidth: outLbl.implicitWidth + 28
                                radius: 8; color: theme.ctrlBg
                                border.color: theme.border; border.width: 1
                                Label { id: outLbl; anchors.centerIn: parent; text: "Log out"
                                        color: theme.text; font.pixelSize: 13 }
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: App.logOut() }
                            }
                        }
                    }

                    // Divider before the My Devices preview.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 22
                        Layout.bottomMargin: 22
                        Layout.preferredHeight: 1
                        color: theme.border
                    }

                    // ── My Devices (personal mesh) ───────────────────
                    // Front-end-ahead-of-backend preview driven by the isolated
                    // DeviceMockModel; account name/email come from the real
                    // signed-in identity when available.  Preview tiers with the
                    // VIVORA_DEVICES_VARIANT env var (pro | trial | free).
                    MyDevicesSettings {
                        Layout.fillWidth: true
                        accountName: App.accountLoggedIn && App.accountEmail.length > 0
                                     ? App.accountEmail.split("@")[0] : "Maxim Kozlov"
                        accountEmail: App.accountLoggedIn && App.accountEmail.length > 0
                                      ? App.accountEmail : "maxim@vivora.dev"
                        onUpgradeRequested: App.openUpgradePage()
                    }
                }

                // ── 1: Appearance ────────────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Appearance"
                        subtitle: "How Vivora looks. Dark and system themes are on the way."
                    }
                    Field {
                        title: "Theme"
                        help: "Light is the only theme today."
                        CreamCombo {
                            enabled: false
                            entries: [ { text: "Light" },
                                       { text: "Dark", sub: "soon" },
                                       { text: "System", sub: "soon" } ]
                            currentIndex: App.settings.theme
                            onActivated: App.settings.theme = currentIndex
                        }
                    }
                }

                // ── 2: Shortcuts ─────────────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Shortcuts"
                        subtitle: "Keyboard shortcuts. Customisation lands with the hotkey engine."
                    }
                    Field {
                        title: "Toggle fullscreen"
                        help: "In an active stream window."
                        Label { text: "F11"; color: theme.text; font.family: theme.monoFont }
                    }
                    Field {
                        title: "Toggle HUD"
                        help: "Latency / FPS overlay."
                        Label { text: "F9"; color: theme.text; font.family: theme.monoFont }
                    }
                }

                // ── 3: Network ───────────────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Network"
                        subtitle: "How Vivora reaches your peers. Defaults are tuned for low-latency LAN; tweak only if you know what you need."
                    }
                    Field {
                        title: "Rendezvous server"
                        help: "Where peers find each other by code."
                        CreamField {
                            Layout.preferredWidth: 220
                            placeholderText: "rdv.vivora.dev:7000"
                            text: App.settings.rendezvous
                            onEditingFinished: App.settings.rendezvous = text
                        }
                    }
                    Field {
                        title: "STUN server"
                        help: "Discovers your public address for hole-punching."
                        CreamField {
                            Layout.preferredWidth: 220
                            placeholderText: "stun.l.google.com:19302"
                            text: App.settings.stunServer
                            onEditingFinished: App.settings.stunServer = text
                        }
                    }
                    Field {
                        title: "Host UDP port"
                        help: "The port Vivora binds for incoming streams."
                        CreamField {
                            Layout.preferredWidth: 110
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: App.settings.hostPort
                            onEditingFinished: {
                                var v = parseInt(text)
                                if (v >= 1024 && v <= 65535) App.settings.hostPort = v
                                else text = App.settings.hostPort
                            }
                        }
                    }
                    Field {
                        title: "Reconnect timeout"
                        help: "When a stream drops, keep the window open and keep retrying for up to this many minutes before closing. 0 closes immediately."
                        CreamSpin {
                            from: 0; to: 60
                            value: App.settings.clientReconnectTimeoutMin
                            onModified: (v) => App.settings.clientReconnectTimeoutMin = v
                        }
                    }
                }

                // ── 4: Hosting ───────────────────────────────────────
                // Encoding/capture settings for when THIS device shares
                // its screen.  The client-side counterpart is Viewing (9).
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Hosting"
                        subtitle: "Encoding and capture when this device shares its screen. Auto picks the best available hardware encoder."
                    }
                    Field {
                        title: "Codec"
                        help: "HEVC is smaller; H.264 is most compatible."
                        CreamCombo {
                            entries: [ { text: "H.264", sub: "most compatible" },
                                       { text: "HEVC",  sub: "H.265", badge: "HW" } ]
                            currentIndex: App.settings.codecIndex
                            onActivated: App.settings.codecIndex = currentIndex
                        }
                    }
                    Field {
                        title: "Encoder"
                        help: "Hardware encoder backend."
                        CreamCombo {
                            entries: [ { text: "Auto" },
                                       { text: "AMD",    sub: "AMF" },
                                       { text: "NVIDIA", sub: "NVENC" },
                                       { text: "Intel",  sub: "QSV" } ]
                            currentIndex: App.settings.encoderIndex
                            onActivated: App.settings.encoderIndex = currentIndex
                        }
                    }
                    Field {
                        title: "Bitrate"
                        help: "Fixed encoder bitrate while sharing. Auto adapts from resolution and link."
                        CreamSlider {
                            from: 0; to: 100
                            suffix: " Mbps"
                            zeroLabel: "Auto"
                            value: App.settings.bitrateMbps
                            onModified: (v) => App.settings.bitrateMbps = v
                        }
                    }
                    // No "Display" field: viewers pick the captured monitor
                    // live from the in-stream panel (VIV-50).  The backend
                    // displayIndex setting still exists for CLI/ini use.
                    Field {
                        title: "Max framerate"
                        help: "Ceiling for the stream framerate while sharing. Lower saves bandwidth and GPU; applied when the next viewer connects."
                        CreamCombo {
                            id: fpsCombo
                            readonly property var fpsValues: [30, 60, 90, 120, 144]
                            entries: [ { text: "30 fps",  sub: "bandwidth saver" },
                                       { text: "60 fps",  sub: "default" },
                                       { text: "90 fps" },
                                       { text: "120 fps" },
                                       { text: "144 fps", badge: "MAX" } ]
                            currentIndex: {
                                var i = fpsValues.indexOf(App.settings.hostFps)
                                return i >= 0 ? i : 1
                            }
                            onActivated: App.settings.hostFps = fpsValues[currentIndex]
                        }
                    }
                    Field {
                        title: "HDR passthrough"
                        help: "Stream HDR10 metadata when the host display supports it."
                        CreamSwitch {
                            checked: App.settings.hdrPassthrough
                            onToggled: App.settings.hdrPassthrough = checked
                        }
                    }
                }

                // ── 5: Security ──────────────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Security"
                        subtitle: "Who can connect to your desktop, and how connections are approved."
                    }
                    Field {
                        title: "Approve connections"
                        help: "What happens when someone connects with your code."
                        CreamCombo {
                            Layout.preferredWidth: 250
                            entries: [ { text: "Always ask" },
                                       { text: "Ask for unknown only" },
                                       { text: "Auto-accept", sub: "advanced" } ]
                            currentIndex: App.settings.approvalMode
                            onActivated: App.settings.approvalMode = currentIndex
                        }
                    }
                    Field {
                        title: "One session at a time"
                        help: "Reject new connections while a session is active."
                        CreamSwitch {
                            checked: App.settings.singleSessionLock
                            onToggled: App.settings.singleSessionLock = checked
                        }
                    }
                    Field {
                        title: "Audio on by default"
                        help: "Pre-select the Audio grant when approving a new connection."
                        CreamSwitch {
                            checked: App.settings.audioGrantDefault
                            onToggled: App.settings.audioGrantDefault = checked
                        }
                    }
                    Field {
                        title: "Idle disconnect"
                        help: "Disconnect viewers after this many minutes of no input."
                        CreamSpin {
                            from: 1; to: 120
                            value: App.settings.idleTimeoutMin
                            onModified: (v) => App.settings.idleTimeoutMin = v
                        }
                    }
                    Field {
                        title: "Your key fingerprint"
                        help: "Read it to a peer out-of-band so they can verify it's really you."
                        RowLayout {
                            spacing: 8
                            Label {
                                text: App.myFingerprint.length > 0 ? App.myFingerprint : "—"
                                color: theme.text
                                font.family: theme.monoFont
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Rectangle {
                                Layout.preferredWidth: fpCopyLbl.implicitWidth + 18
                                Layout.preferredHeight: 26
                                radius: 7
                                color: fpCopyHover.hovered ? theme.hoverBg : theme.ctrlBg
                                border.color: theme.border
                                border.width: 1
                                HoverHandler { id: fpCopyHover }
                                Label {
                                    id: fpCopyLbl
                                    anchors.centerIn: parent
                                    text: fpCopyTimer.running ? "Copied" : "Copy"
                                    color: theme.text
                                    font.pixelSize: 11
                                }
                                Timer { id: fpCopyTimer; interval: 1200 }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        App.copyToClipboard(App.myFingerprint)
                                        fpCopyTimer.restart()
                                    }
                                }
                            }
                        }
                    }
                }

                // ── 6: Self-hosted relay ─────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Self-hosted relay"
                        subtitle: "Run your own relay for full sovereignty. Leave blank to use the Vivora-managed relay."
                    }
                    Field {
                        title: "Relay server"
                        help: "host:port of your relay daemon."
                        CreamField {
                            Layout.preferredWidth: 220
                            placeholderText: "relay.vivora.dev:7100"
                            text: App.settings.relay
                            onEditingFinished: App.settings.relay = text
                        }
                    }
                    Field {
                        title: "License token"
                        help: "Required by the managed relay; not by self-hosted."
                        RowLayout {
                            Layout.preferredWidth: 260
                            spacing: 6
                            CreamField {
                                Layout.fillWidth: true
                                text: App.settings.licenseFile
                                onEditingFinished: App.settings.licenseFile = text
                            }
                            // Cream-styled browse button to match the rest
                            // of the panel (default Qt Button is a grey
                            // pill that clashes).
                            Rectangle {
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 38
                                radius: 8
                                color: browseHover.hovered ? theme.hoverBg : theme.ctrlBg
                                border.color: theme.border
                                border.width: 1
                                HoverHandler { id: browseHover }
                                Label {
                                    anchors.centerIn: parent
                                    text: "…"
                                    color: theme.text
                                    font.pixelSize: 16
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: licenseDlg.open()
                                }
                            }
                        }
                    }
                    FileDialog {
                        id: licenseDlg
                        title: "Select license token"
                        onAccepted: App.settings.licenseFile =
                            selectedFile.toString().replace("file:///", "")
                    }
                    // VIV-29: the managed relay is Pro-gated.  Warn when it's
                    // configured (the default) but no Pro license is loaded —
                    // connections then fall back to direct + rendezvous only.
                    Label {
                        visible: !App.licensePro
                                 && App.settings.relay.toLowerCase().indexOf("vivora.dev") >= 0
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: theme.warn
                        text: "⚠  The managed relay (relay.vivora.dev) needs a Pro license. " +
                              "Without one, connections use direct + rendezvous hole-punching only — " +
                              "import a license in About, or point this at your own relay."
                    }
                }

                // ── 7: About ─────────────────────────────────────────
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "About Vivora"
                        subtitle: "Open-source, low-latency remote desktop."
                    }
                    Field {
                        title: "Version"
                        Label { text: "0.1.0"; color: theme.text; font.family: theme.monoFont }
                    }
                    Field {
                        title: "Peer code"
                        Label { text: App.myPeerCode; color: theme.text; font.family: theme.monoFont }
                    }
                    Field {
                        title: "License"
                        help: "Client + rendezvous + relay are AGPL-3.0."
                        Label { text: "AGPL-3.0"; color: theme.text }
                    }
                    // VIV-29: Pro license status, verified offline.
                    Field {
                        title: "Pro license"
                        help: "Unlocks the managed relay + commercial use. Verified offline."
                        RowLayout {
                            spacing: 8
                            Label {
                                text: App.licensePro
                                      ? ("Pro · expires " + App.licenseExpiry)
                                      : (App.licenseValid
                                         ? ("Active · expires " + App.licenseExpiry)
                                         : "Free — no Pro license")
                                color: App.licensePro ? theme.accent : theme.textMuted
                            }
                            Rectangle {
                                Layout.preferredWidth: importLbl.implicitWidth + 20
                                Layout.preferredHeight: 28
                                radius: 8
                                color: importHover.hovered ? theme.hoverBg : theme.ctrlBg
                                border.color: theme.border
                                border.width: 1
                                HoverHandler { id: importHover }
                                Label {
                                    id: importLbl
                                    anchors.centerIn: parent
                                    text: "Import…"
                                    color: theme.text
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: proLicenseDlg.open()
                                }
                            }
                        }
                    }
                    FileDialog {
                        id: proLicenseDlg
                        title: "Import Vivora Pro license"
                        onAccepted: App.importLicense(selectedFile.toString())
                    }
                    Field {
                        title: "Links"
                        ColumnLayout {
                            spacing: 2
                            Label {
                                text: "github.com/vivoradesk/vivora"
                                color: theme.accent
                                font.pixelSize: 12
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Qt.openUrlExternally("https://github.com/vivoradesk/vivora")
                                }
                            }
                            Label {
                                text: "vivora.dev"
                                color: theme.accent
                                font.pixelSize: 12
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Qt.openUrlExternally("https://vivora.dev")
                                }
                            }
                        }
                    }
                }

                // ── 8: Startup (VIV-18) ──────────────────────────────
                // Sidebar shows this under GENERAL; it lives at the end of
                // the stack so the existing panel indices stay stable.
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Startup"
                        subtitle: "How Vivora behaves when you sign in to this computer."
                    }
                    Field {
                        title: "Start at login"
                        help: App.settings.startAtLoginSupported
                              ? "Launch Vivora automatically when you sign in."
                              : "Windows only for now — macOS and Linux are on the way."
                        CreamSwitch {
                            enabled: App.settings.startAtLoginSupported
                            checked: App.settings.startAtLogin
                            onToggled: App.settings.startAtLogin = checked
                        }
                    }
                }

                // ── 9: Viewing ───────────────────────────────────────
                // Client-side caps applied when THIS device watches a
                // remote desktop.  Appended at the stack end so existing
                // panel indices stay stable (same pattern as Startup).
                ColumnLayout {
                    spacing: 0
                    SectionTitle {
                        title: "Viewing"
                        subtitle: "Limits for streams you watch from this device. Also adjustable mid-stream from the in-stream menu (Ctrl+F1)."
                    }
                    Field {
                        title: "Max framerate"
                        help: "Ceiling for the adaptive framerate when viewing. Auto lets it probe up to what this device sustains."
                        CreamCombo {
                            readonly property var fpsValues: [0, 30, 60, 90, 120, 144]
                            entries: [ { text: "Auto", sub: "adaptive" },
                                       { text: "30 fps" },
                                       { text: "60 fps" },
                                       { text: "90 fps" },
                                       { text: "120 fps" },
                                       { text: "144 fps" } ]
                            currentIndex: {
                                var i = fpsValues.indexOf(App.settings.viewFpsCap)
                                return i >= 0 ? i : 0
                            }
                            onActivated: App.settings.viewFpsCap = fpsValues[currentIndex]
                        }
                    }
                    Field {
                        title: "Max bitrate"
                        help: "Hard limit the host must respect for this viewer. Auto adapts to the link."
                        CreamCombo {
                            readonly property var kbpsValues: [0, 5000, 10000, 20000, 35000, 50000]
                            entries: [ { text: "Auto", sub: "adaptive" },
                                       { text: "5 Mbps" },
                                       { text: "10 Mbps" },
                                       { text: "20 Mbps" },
                                       { text: "35 Mbps" },
                                       { text: "50 Mbps" } ]
                            currentIndex: {
                                var i = kbpsValues.indexOf(App.settings.viewMaxKbps)
                                return i >= 0 ? i : 0
                            }
                            onActivated: App.settings.viewMaxKbps = kbpsValues[currentIndex]
                        }
                    }
                }
            }
        }
    }
}
