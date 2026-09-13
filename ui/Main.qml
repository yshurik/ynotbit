import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    width: 1160
    height: 780
    minimumWidth: 900
    minimumHeight: 620
    title: session.document + " — Notbit"
    color: "#f5f7fa"
    property string folder: "Inbox"
    property var selected: ({})
    property color ink: "#182c3a"
    property color muted: "#758591"
    property color accent: "#197e76"
    font.family: "Helvetica Neue"
    font.pixelSize: 14
    onClosing: session.lock()
    Connections {
        target: session
        function onLocked() {
            root.selected = ({});
            composer.close();
            toField.clear();
            subjectField.clear();
            bodyField.clear();
        }
    }
    component FlatButton: Button {
        id: btn
        padding: 11
        leftPadding: 16
        rightPadding: 16
        contentItem: Text {
            text: btn.text
            color: btn.enabled ? root.ink : "#a3adb4"
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 7
            color: btn.down ? "#dbe5e9" : btn.hovered ? "#e8eff2" : "#ffffff"
            border.color: "#dbe3e8"
        }
    }
    menuBar: MenuBar {
        Menu {
            title: "File"
            Action {
                text: "Create vault…"
                enabled: !session.unlocked
                onTriggered: session.createVault()
            }
            Action {
                text: "Open vault…"
                enabled: !session.unlocked
                onTriggered: session.openVault()
            }
            MenuSeparator {}
            Action {
                text: "Create mailbox…"
                enabled: session.unlocked && !session.mailboxOpen
                onTriggered: session.createMailbox()
            }
            Action {
                text: "Open mailbox…"
                enabled: session.unlocked
                onTriggered: {
                    root.selected = ({});
                    session.openMailbox();
                }
            }
            Action {
                text: "Back up mailbox and vault…"
                enabled: session.mailboxOpen
                onTriggered: session.backup()
            }
            MenuSeparator {}
            Action {
                text: "Lock vault"
                enabled: session.unlocked
                shortcut: "Ctrl+L"
                onTriggered: session.lock()
            }
        }
        Menu {
            title: "Identity"
            Action {
                text: "Create identity…"
                enabled: session.unlocked
                onTriggered: session.addIdentity()
            }
            Action {
                text: "Join or create chan…"
                enabled: session.unlocked
                onTriggered: session.joinChannel()
            }
            Action {
                text: "Import keys.dat…"
                enabled: session.unlocked
                onTriggered: session.importIdentities()
            }
            Action {
                text: "Change vault password…"
                enabled: session.unlocked
                onTriggered: session.changePassword()
            }
            Action {
                text: "Inspect retained objects again"
                enabled: session.mailboxOpen
                onTriggered: session.rescan()
            }
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            color: "#fff"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 24
                spacing: 16
                Rectangle {
                    width: 36
                    height: 36
                    radius: 11
                    color: root.accent
                    Text {
                        anchors.centerIn: parent
                        text: "n"
                        font.pixelSize: 27
                        font.bold: true
                        color: "white"
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Text {
                        text: "notbit"
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        text: "PRIVATE CORRESPONDENCE"
                        font.pixelSize: 9
                        font.letterSpacing: 1.8
                        color: root.muted
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Rectangle {
                    implicitWidth: stateText.width + 24
                    implicitHeight: 30
                    radius: 15
                    color: session.unlocked ? "#e4f3ec" : "#edf1f5"
                    Text {
                        id: stateText
                        anchors.centerIn: parent
                        text: session.unlocked ? "Vault unlocked" : "Vault locked"
                        font.pixelSize: 12
                        color: root.accent
                    }
                }
                FlatButton {
                    text: session.unlocked ? "Lock vault" : "Unlock vault"
                    onClicked: session.unlocked ? session.lock() : session.unlockVault()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#e0e6eb"
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: session.error.length ? 48 : 0
            visible: session.error.length > 0
            color: "#fff0e6"
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                Text {
                    text: session.error
                    color: "#82461c"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                ToolButton {
                    text: "Dismiss"
                    onClicked: session.clearError()
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Rectangle {
                Layout.preferredWidth: 212
                Layout.fillHeight: true
                color: "#f1f5f7"
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 9
                    FlatButton {
                        Layout.fillWidth: true
                        text: "＋  Write a letter"
                        enabled: session.mailboxOpen
                        onClicked: composer.open()
                    }
                    Text {
                        text: "MAILBOX"
                        font.pixelSize: 10
                        font.letterSpacing: 1.6
                        color: root.muted
                        Layout.topMargin: 25
                        Layout.bottomMargin: 6
                    }
                    Repeater {
                        model: ["Inbox", "Drafts", "Channels", "Identities"]
                        delegate: Rectangle {
                            required property string modelData
                            Layout.fillWidth: true
                            height: 40
                            radius: 7
                            color: root.folder === modelData ? "#dcebe8" : "transparent"
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                text: modelData
                                color: root.folder === modelData ? root.accent : root.ink
                                font.weight: root.folder === modelData ? Font.DemiBold : Font.Normal
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    root.folder = modelData;
                                    root.selected = ({});
                                }
                            }
                        }
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                    Text {
                        text: "Your keys. Your mailbox."
                        color: root.muted
                        font.pixelSize: 12
                    }
                    Text {
                        text: "Development build · 0.1"
                        color: root.muted
                        font.pixelSize: 10
                    }
                }
            }
            Rectangle {
                Layout.fillHeight: true
                width: 1
                color: "#e0e6eb"
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ColumnLayout {
                    visible: !session.mailboxOpen && root.folder !== "Identities"
                    anchors.centerIn: parent
                    width: 490
                    spacing: 17
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        width: 68
                        height: 68
                        radius: 22
                        color: "#e1eeeb"
                        Text {
                            anchors.centerIn: parent
                            text: "✉"
                            font.pixelSize: 32
                            color: root.accent
                        }
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: session.unlocked ? "Open your correspondence" : "A quiet place for your letters"
                        font.pixelSize: 27
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        text: session.unlocked ? "Choose an encrypted mailbox from Documents, or create one. Its key stays in your vault." : "Unlock your vault to read your mailbox. The background node can keep collecting objects while your keys stay locked."
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        lineHeight: 1.5
                        color: root.muted
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 9
                        FlatButton {
                            text: session.unlocked ? "Open mailbox…" : "Open vault…"
                            onClicked: session.unlocked ? session.openMailbox() : session.openVault()
                        }
                        FlatButton {
                            text: session.unlocked ? "Create mailbox…" : "Create vault…"
                            onClicked: session.unlocked ? session.createMailbox() : session.createVault()
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 27
                        height: 1
                        color: "#dde5e9"
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 30
                        ColumnLayout {
                            Text {
                                text: "01  UNLOCK"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                text: "Your vault"
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        ColumnLayout {
                            Text {
                                text: "02  INSPECT"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                text: "Retained objects"
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        ColumnLayout {
                            Text {
                                text: "03  READ"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                text: "Your correspondence"
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                    }
                }
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 28
                    visible: root.folder === "Identities"
                    spacing: 16
                    Text {
                        text: "Identities & chans"
                        font.pixelSize: 25
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        text: session.unlocked ? "Private keys remain inside the encrypted vault. Chans share an identity among members." : "Unlock your vault to view identities."
                        color: root.muted
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        FlatButton {
                            text: "New identity"
                            enabled: session.unlocked
                            onClicked: session.addIdentity()
                        }
                        FlatButton {
                            text: "Join chan"
                            enabled: session.unlocked
                            onClicked: session.joinChannel()
                        }
                        FlatButton {
                            text: "Import keys"
                            enabled: session.unlocked
                            onClicked: session.importIdentities()
                        }
                    }
                    ListView {
                        Layout.fillHeight: true
                        Layout.fillWidth: true
                        spacing: 10
                        clip: true
                        model: session.identities
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 86
                            radius: 9
                            color: "white"
                            border.color: "#e0e7eb"
                            Column {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text {
                                    text: modelData.label + (modelData.chan ? " · Shared chan" : "")
                                    color: root.ink
                                    font.bold: true
                                }
                                TextEdit {
                                    text: modelData.address
                                    readOnly: true
                                    selectByMouse: true
                                    color: root.muted
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    visible: session.mailboxOpen && root.folder !== "Identities"
                    Rectangle {
                        Layout.preferredWidth: 300
                        Layout.fillHeight: true
                        color: "#fff"
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0
                            Column {
                                Layout.fillWidth: true
                                Layout.margins: 20
                                spacing: 6
                                Text {
                                    text: root.folder
                                    font.pixelSize: 24
                                    font.weight: Font.DemiBold
                                    color: root.ink
                                }
                                Text {
                                    text: session.document
                                    color: root.muted
                                    font.pixelSize: 11
                                }
                            }
                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: session.messages
                                delegate: Rectangle {
                                    required property var modelData
                                    width: ListView.view.width
                                    height: modelData.folder === root.folder ? 104 : 0
                                    visible: height > 0
                                    color: root.selected.hash === modelData.hash ? "#eaf3f1" : "white"
                                    Column {
                                        anchors.fill: parent
                                        anchors.margins: 17
                                        spacing: 7
                                        Text {
                                            width: parent.width
                                            text: modelData.subject || "Untitled letter"
                                            elide: Text.ElideRight
                                            font.weight: Font.DemiBold
                                            color: root.ink
                                        }
                                        Text {
                                            width: parent.width
                                            text: modelData.body.replace(/\n/g, " ")
                                            elide: Text.ElideRight
                                            color: root.muted
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            text: modelData.received
                                            color: root.muted
                                            font.pixelSize: 10
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.selected = modelData
                                    }
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 1
                                        color: "#edf1f4"
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        width: 1
                        Layout.fillHeight: true
                        color: "#e0e6eb"
                    }
                    Item {
                        Layout.fillHeight: true
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.centerIn: parent
                            visible: !root.selected.hash
                            spacing: 12
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "No letter selected"
                                color: root.ink
                                font.pixelSize: 21
                            }
                            Text {
                                text: "Letters appear after cached objects are decrypted."
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        ScrollView {
                            anchors.fill: parent
                            anchors.margins: 32
                            visible: !!root.selected.hash
                            contentWidth: availableWidth
                            ColumnLayout {
                                width: parent.width
                                spacing: 18
                                Text {
                                    Layout.fillWidth: true
                                    text: root.selected.subject || "Untitled letter"
                                    font.pixelSize: 25
                                    font.weight: Font.DemiBold
                                    color: root.ink
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.selected.folder === "Drafts" ? "DRAFT · NOT SENT" : "DECRYPTED · SAVED LOCALLY"
                                    color: root.accent
                                    font.pixelSize: 10
                                    font.letterSpacing: 1.1
                                }
                                TextEdit {
                                    Layout.fillWidth: true
                                    text: "From  " + (root.selected.from || "Choose an identity before sending") + "\nTo      " + (root.selected.to || "")
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    color: root.muted
                                    font.pixelSize: 12
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 1
                                    color: "#e0e6eb"
                                }
                                TextEdit {
                                    Layout.fillWidth: true
                                    text: root.selected.body || ""
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    color: root.ink
                                    font.pixelSize: 15
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 65
            color: "#fff"
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 23
                anchors.rightMargin: 23
                anchors.topMargin: 10
                anchors.bottomMargin: 10
                spacing: 5
                RowLayout {
                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        color: root.accent
                    }
                    Text {
                        text: session.status
                        color: root.ink
                        font.pixelSize: 11
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Text {
                        text: session.objectCount + " cached objects · " + (session.cacheBytes / 1048576).toFixed(1) + " MB"
                        color: root.muted
                        font.pixelSize: 11
                    }
                }
                Text {
                    text: session.activity
                    color: root.muted
                    font.pixelSize: 10
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
        }
    }
    Dialog {
        id: composer
        title: "Write a letter"
        modal: true
        width: 640
        height: 540
        anchors.centerIn: parent
        standardButtons: Dialog.Cancel
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            TextField {
                id: toField
                Layout.fillWidth: true
                placeholderText: "Recipient · BM-address"
            }
            TextField {
                id: subjectField
                Layout.fillWidth: true
                placeholderText: "Subject"
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    id: bodyField
                    placeholderText: "Take your time. Write something worth sending."
                    wrapMode: TextEdit.Wrap
                }
            }
            Text {
                Layout.fillWidth: true
                text: "This development build saves encrypted drafts. Sending and acknowledgments are still being integrated."
                wrapMode: Text.WordWrap
                color: root.muted
                font.pixelSize: 12
            }
            FlatButton {
                text: "Save encrypted draft"
                onClicked: {
                    session.saveDraft(toField.text, subjectField.text, bodyField.text);
                    if (!session.error) {
                        composer.close();
                        toField.clear();
                        subjectField.clear();
                        bodyField.clear();
                        root.folder = "Drafts";
                    }
                }
            }
        }
    }
}
