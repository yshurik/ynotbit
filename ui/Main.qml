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
    title: session.document + " — ynotbit"
    color: "#f5f7fa"
    property string folder: "Inbox"
    property var selected: ({})
    property string draftId: ""
    property bool dirty: false
    property bool loadingDraft: false
    property var senders: []
    property var history: []
    function saveCurrent() {
        if (!dirty || !session.mailboxOpen)
            return true;
        var sender = fromField.currentIndex >= 0 ? senders[fromField.currentIndex].address : "";
        var id = session.saveLetter(draftId, sender, toField.text, subjectField.text, bodyField.text, broadcastField.checked ? "broadcast" : "direct");
        if (!id)
            return false;
        draftId = id;
        dirty = false;
        return true;
    }
    function editLetter(letter, reply) {
        if (composer.visible && !saveCurrent())
            return;
        loadingDraft = true;
        senders = session.identities;
        draftId = reply ? "" : (letter.hash || "");
        var sender = reply ? letter.to : letter.from;
        fromField.currentIndex = senders.length ? 0 : -1;
        for (var n = 0; n < senders.length; ++n)
            if (senders[n].address === sender)
                fromField.currentIndex = n;
        toField.text = reply ? (letter.folder === "Channels" ? letter.to : letter.from) : (letter.to || "");
        subjectField.text = reply ? (/^Re:/i.test(letter.subject || "") ? letter.subject : "Re: " + (letter.subject || "")) : (letter.subject || "");
        bodyField.text = reply ? "" : (letter.body || "");
        broadcastField.checked = !reply && letter.kind === "broadcast";
        dirty = reply;
        loadingDraft = false;
        composer.open();
    }
    function changedDraft() {
        if (composer.visible && !loadingDraft) {
            dirty = true;
            autosave.restart();
        }
    }
    Timer {
        id: autosave
        interval: 800
        onTriggered: root.saveCurrent()
    }
    property color ink: "#182c3a"
    property color muted: "#758591"
    property color accent: "#197e76"
    font.family: "Helvetica Neue"
    font.pixelSize: 14
    onClosing: session.lock()
    Connections {
        target: session
        function onAboutToCloseMailbox() {
            root.saveCurrent();
            composer.close();
            root.selected = ({});
            root.history = [];
        }
        function onChanged() {
            if (root.selected.hash) {
                var found = false;
                for (var i = 0; i < session.messages.length; ++i)
                    if (session.messages[i].hash === root.selected.hash) {
                        root.selected = session.messages[i];
                        found = true;
                        break;
                    }
                if (!found)
                    root.selected = ({});
            }
        }
        function onLocked() {
            root.dirty = false;
            root.draftId = "";
            root.senders = [];
            root.history = [];
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
            textFormat: Text.PlainText
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
                enabled: session.unlocked
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
                text: "Close mailbox"
                enabled: session.mailboxOpen
                onTriggered: session.closeMailbox()
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
            title: "Network"
            Action {
                text: "Network enabled"
                checkable: true
                checked: session.networkEnabled
                onTriggered: session.setNetworkEnabled(checked)
            }
            Action {
                text: "Peer / proxy settings…"
                onTriggered: session.configureNode()
            }
            Action {
                text: "Restart node"
                enabled: session.networkEnabled
                onTriggered: session.restartNode()
            }
            Action {
                text: "Retention settings…"
                onTriggered: session.configureRetention()
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
                text: "Subscribe to broadcasts…"
                enabled: session.mailboxOpen
                onTriggered: session.subscribe()
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
                        textFormat: Text.PlainText
                        anchors.centerIn: parent
                        text: "y"
                        font.pixelSize: 27
                        font.bold: true
                        color: "white"
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Text {
                        textFormat: Text.PlainText
                        text: "ynotbit"
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
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
                    textFormat: Text.PlainText
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
                        onClicked: root.editLetter({}, false)
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: "MAILBOX"
                        font.pixelSize: 10
                        font.letterSpacing: 1.6
                        color: root.muted
                        Layout.topMargin: 25
                        Layout.bottomMargin: 6
                    }
                    Repeater {
                        model: ["Inbox", "Drafts", "Outbox", "Sent", "Channels", "Broadcasts", "Archive", "Trash", "Identities"]
                        delegate: Rectangle {
                            required property string modelData
                            Layout.fillWidth: true
                            height: 34
                            radius: 7
                            color: root.folder === modelData ? "#dcebe8" : "transparent"
                            Text {
                                textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
                        text: "Your keys. Your mailbox."
                        color: root.muted
                        font.pixelSize: 12
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: "Development build · 0.2"
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
                            textFormat: Text.PlainText
                            anchors.centerIn: parent
                            text: "✉"
                            font.pixelSize: 32
                            color: root.accent
                        }
                    }
                    Text {
                        textFormat: Text.PlainText
                        Layout.alignment: Qt.AlignHCenter
                        text: session.unlocked ? "Open your correspondence" : "A quiet place for your letters"
                        font.pixelSize: 27
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        textFormat: Text.PlainText
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
                                textFormat: Text.PlainText
                                text: "01  UNLOCK"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                textFormat: Text.PlainText
                                text: "Your vault"
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        ColumnLayout {
                            Text {
                                textFormat: Text.PlainText
                                text: "02  INSPECT"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                textFormat: Text.PlainText
                                text: "Retained objects"
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        ColumnLayout {
                            Text {
                                textFormat: Text.PlainText
                                text: "03  READ"
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: root.accent
                            }
                            Text {
                                textFormat: Text.PlainText
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
                        textFormat: Text.PlainText
                        text: "Identities & chans"
                        font.pixelSize: 25
                        font.weight: Font.DemiBold
                        color: root.ink
                    }
                    Text {
                        textFormat: Text.PlainText
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
                            height: 120
                            radius: 9
                            color: "white"
                            border.color: "#e0e7eb"
                            Column {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                Text {
                                    textFormat: Text.PlainText
                                    text: modelData.label + (modelData.chan ? " · Shared chan" : "")
                                    color: root.ink
                                    font.bold: true
                                }
                                TextEdit {
                                    textFormat: TextEdit.PlainText
                                    text: modelData.address
                                    readOnly: true
                                    selectByMouse: true
                                    color: root.muted
                                    font.pixelSize: 12
                                }
                                Row {
                                    spacing: 8
                                    Button {
                                        text: "Copy address"
                                        onClicked: session.copyAddress(modelData.address)
                                    }
                                    Button {
                                        text: "Rename"
                                        onClicked: session.renameIdentity(modelData.address)
                                    }
                                    Button {
                                        text: modelData.chan ? "Write to chan" : "Write"
                                        enabled: session.mailboxOpen
                                        onClicked: root.editLetter({
                                            to: modelData.address
                                        }, false)
                                    }
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
                                    textFormat: Text.PlainText
                                    text: root.folder
                                    font.pixelSize: 24
                                    font.weight: Font.DemiBold
                                    color: root.ink
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    text: session.document
                                    color: root.muted
                                    font.pixelSize: 11
                                }
                            }
                            TextField {
                                id: searchField
                                Layout.fillWidth: true
                                Layout.margins: 12
                                placeholderText: "Search this folder"
                            }
                            Button {
                                text: "Manage subscriptions"
                                visible: root.folder === "Broadcasts"
                                Layout.fillWidth: true
                                onClicked: subscriptionsDialog.open()
                            }
                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: session.messages
                                delegate: Rectangle {
                                    required property var modelData
                                    width: ListView.view.width
                                    height: modelData.folder === root.folder && (!searchField.text || (modelData.subject + " " + modelData.body + " " + modelData.from + " " + modelData.to).toLowerCase().includes(searchField.text.toLowerCase())) ? 104 : 0
                                    visible: height > 0
                                    color: root.selected.hash === modelData.hash ? "#eaf3f1" : "white"
                                    Column {
                                        anchors.fill: parent
                                        anchors.margins: 17
                                        spacing: 7
                                        Text {
                                            textFormat: Text.PlainText
                                            width: parent.width
                                            text: (modelData.unread ? "• " : "") + (modelData.subject || "Untitled letter")
                                            elide: Text.ElideRight
                                            font.weight: Font.DemiBold
                                            color: root.ink
                                        }
                                        Text {
                                            textFormat: Text.PlainText
                                            width: parent.width
                                            text: modelData.body.replace(/\n/g, " ")
                                            elide: Text.ElideRight
                                            color: root.muted
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            textFormat: Text.PlainText
                                            text: modelData.state ? modelData.state.replace(/_/g, " ") : modelData.received
                                            color: root.muted
                                            font.pixelSize: 10
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: {
                                            root.selected = modelData;
                                            session.readLetter(modelData.hash);
                                        }
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
                                textFormat: Text.PlainText
                                Layout.alignment: Qt.AlignHCenter
                                text: "No letter selected"
                                color: root.ink
                                font.pixelSize: 21
                            }
                            Text {
                                textFormat: Text.PlainText
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
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: root.selected.subject || "Untitled letter"
                                    font.pixelSize: 25
                                    font.weight: Font.DemiBold
                                    color: root.ink
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    text: root.selected.state ? root.selected.state.replace(/_/g, " ").toUpperCase() : root.selected.folder === "Drafts" ? "DRAFT · NOT SENT" : "DECRYPTED · SAVED LOCALLY"
                                    color: root.accent
                                    font.pixelSize: 10
                                    font.letterSpacing: 1.1
                                }
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 7
                                    Button {
                                        text: "Edit / Send"
                                        visible: root.selected.folder === "Drafts"
                                        onClicked: root.editLetter(root.selected, false)
                                    }
                                    Button {
                                        text: "Reply"
                                        visible: ["Inbox", "Channels", "Broadcasts", "Archive"].includes(root.selected.folder)
                                        onClicked: root.editLetter(root.selected, true)
                                    }
                                    Button {
                                        text: "Delivery history"
                                        visible: !!root.selected.state
                                        onClicked: {
                                            root.history = session.deliveryHistory(root.selected.hash);
                                            historyDialog.open();
                                        }
                                    }
                                    Button {
                                        text: "Retry"
                                        visible: ["failed", "expired", "cancelled"].includes(root.selected.state)
                                        onClicked: session.retryLetter(root.selected.hash)
                                    }
                                    Button {
                                        text: "Cancel delivery"
                                        visible: !!root.selected.state && !["acknowledged", "published", "cancelled", "failed", "expired"].includes(root.selected.state)
                                        onClicked: session.cancelLetter(root.selected.hash)
                                    }
                                    Button {
                                        text: "Archive"
                                        visible: ["Inbox", "Channels", "Broadcasts", "Sent"].includes(root.selected.folder)
                                        onClicked: session.moveLetter(root.selected.hash, "Archive")
                                    }
                                    Button {
                                        text: "Trash"
                                        visible: root.selected.folder !== "Trash" && (!root.selected.state || ["acknowledged", "published", "cancelled", "failed", "expired"].includes(root.selected.state))
                                        onClicked: session.moveLetter(root.selected.hash, "Trash")
                                    }
                                    Button {
                                        text: "Restore"
                                        visible: root.selected.folder === "Trash"
                                        onClicked: session.restoreLetter(root.selected.hash)
                                    }
                                    Button {
                                        text: "Delete permanently"
                                        visible: root.selected.folder === "Trash"
                                        onClicked: session.deleteLetter(root.selected.hash)
                                    }
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    visible: !!root.selected.deliveryError
                                    text: root.selected.deliveryError || ""
                                    color: "#82461c"
                                    wrapMode: Text.WordWrap
                                }
                                TextEdit {
                                    textFormat: TextEdit.PlainText
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
                                    textFormat: TextEdit.PlainText
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
                        textFormat: Text.PlainText
                        text: session.status
                        color: root.ink
                        font.pixelSize: 11
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: session.objectCount + " cached objects · " + (session.cacheBytes / 1048576).toFixed(1) + " MB"
                        color: root.muted
                        font.pixelSize: 11
                    }
                }
                Text {
                    textFormat: Text.PlainText
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
        objectName: "composer"
        title: root.draftId ? "Edit letter" : "Write a letter"
        modal: true
        width: Math.min(680, root.width - 60)
        height: Math.min(630, root.height - 70)
        anchors.centerIn: parent
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            if (root.dirty && session.mailboxOpen && !root.saveCurrent())
                Qt.callLater(function () {
                    composer.open();
                });
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            ComboBox {
                id: fromField
                objectName: "senderSelector"
                Layout.fillWidth: true
                model: root.senders
                textRole: "label"
                onActivated: root.changedDraft()
            }
            Text {
                textFormat: Text.PlainText
                visible: !root.senders.length
                text: "Create or import an identity to send this letter."
                color: root.muted
            }
            CheckBox {
                id: broadcastField
                text: "Broadcast to subscribers"
                onToggled: root.changedDraft()
            }
            TextField {
                id: toField
                objectName: "recipientField"
                Layout.fillWidth: true
                visible: !broadcastField.checked
                placeholderText: "Recipient · BM-address"
                onTextEdited: root.changedDraft()
            }
            TextField {
                id: subjectField
                objectName: "subjectField"
                Layout.fillWidth: true
                placeholderText: "Subject"
                onTextEdited: root.changedDraft()
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    id: bodyField
                    textFormat: TextEdit.PlainText
                    objectName: "bodyField"
                    placeholderText: "Take your time. Write something worth sending."
                    wrapMode: TextEdit.Wrap
                    onTextChanged: root.changedDraft()
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: session.error || (root.dirty ? "Unsaved changes" : root.draftId ? "Saved in your encrypted mailbox" : "Drafts are saved as you write")
                wrapMode: Text.WordWrap
                color: session.error ? "#82461c" : root.muted
                font.pixelSize: 12
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: "Send queues proof of work and network delivery. Locking pauses preparation; already submitted objects can continue relaying."
                wrapMode: Text.WordWrap
                color: root.muted
                font.pixelSize: 12
            }
            RowLayout {
                FlatButton {
                    text: "Save & close"
                    onClicked: {
                        root.dirty = true;
                        if (root.saveCurrent()) {
                            composer.close();
                            root.folder = "Drafts";
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                FlatButton {
                    objectName: "sendButton"
                    text: "Send letter"
                    enabled: root.senders.length > 0 && !!bodyField.text.trim() && (broadcastField.checked || !!toField.text.trim())
                    onClicked: {
                        root.dirty = true;
                        if (root.saveCurrent() && session.sendLetter(root.draftId)) {
                            root.dirty = false;
                            composer.close();
                            root.folder = "Outbox";
                            root.selected = ({});
                        }
                    }
                }
            }
        }
    }
    Dialog {
        id: historyDialog
        title: "Delivery history"
        modal: true
        width: 580
        height: 420
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            Column {
                width: parent.width
                spacing: 16
                Repeater {
                    model: root.history
                    Text {
                        textFormat: Text.PlainText
                        required property var modelData
                        width: parent.width
                        text: modelData.time + " · " + modelData.state.replace(/_/g, " ") + "\n" + modelData.detail
                        wrapMode: Text.WordWrap
                        color: root.ink
                    }
                }
            }
        }
    }
    Dialog {
        id: subscriptionsDialog
        title: "Broadcast subscriptions"
        modal: true
        width: 580
        height: 420
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            FlatButton {
                text: "Add subscription…"
                onClicked: session.subscribe()
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: session.subscriptions
                clip: true
                delegate: Column {
                    required property var modelData
                    width: ListView.view.width
                    spacing: 5
                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: modelData.label + "\n" + modelData.address
                        wrapMode: Text.WrapAnywhere
                        color: root.ink
                    }
                    Button {
                        text: "Unsubscribe"
                        onClicked: session.unsubscribe(modelData.address)
                    }
                }
            }
        }
    }
}
