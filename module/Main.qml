import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
// The host requires these imports for custom colours to apply; the view styles with its own palette.
import Logos.Theme
import Logos.Controls

// Crema - the organizer's desk for a coffee passport event. Pure QML over crema_core:
//   snapshot() -> {ok, status, me, current, events:[{eventId,name,organizer}], event:{eventId, name, organizer,
//                  catalog, leaderboard:[{author,name,cafes,stamps,visited,reachedAt}], logSize,
//                  draft?, publishedVersion?, unpublished?}}
//   createEvent(name, city, "YYYY-MM-DD", days)  importCafes(eventId, listJson)  addCafe(eventId, cafeJson)
//   removeCafe(eventId, cafeId)  publishCatalog(eventId)  joinQr(eventId) -> {link, n, cells}
//   cafeSetupQr(eventId, cafeId) -> {link, name, n, cells}  cafeStamp(eventId, cafeId) -> {text, n, cells, secondsLeft}
//   joinEvent(link)  selectEvent(eventId)
// Every call is async (callModuleAsync); every string from the network is shown as PlainText.
Item {
    id: root
    anchors.fill: parent

    // ── palette: the phone app's (mobile/App.tsx) ──
    readonly property color cBg: "#1d1410"
    readonly property color cCard: "#2a1d16"
    readonly property color cRaised: "#33241b"
    readonly property color cLine: "#3a2a20"
    readonly property color cCrema: "#d9a066"
    readonly property color cText: "#f3e7da"
    readonly property color cMuted: "#b59c86"
    readonly property color cGood: "#7fb069"
    readonly property color cBad: "#e07a5f"

    // ── state ──
    property var st: ({})
    property string tab: "board"              // studio | board | cafe
    property string toastMsg: ""
    property string dialog: ""                 // "" | new | join | joinqr | setup | import | add
    property var qr: null                      // the dialog's QR {n, cells, link, name}
    property string cafeId: ""                 // café mode: which café
    property var stamp: null                   // café mode: {text, n, cells, secondsLeft}
    property double stampAt: 0

    readonly property var ev: st.event || null
    readonly property bool organizer: !!(ev && ev.organizer)
    readonly property var catalog: ev && ev.catalog ? ev.catalog : null
    readonly property var draftCafes: ev && ev.draft && ev.draft.cafes ? ev.draft.cafes : []
    readonly property var board: ev && ev.leaderboard ? ev.leaderboard : []
    readonly property int totalCafes: catalog && catalog.cafes ? catalog.cafes.length : 0
    readonly property bool connected: st.status === "Connected"

    // ── core plumbing: async only (a blocking callModule freezes the view for up to 20 s) ──
    function callVia(m, a, cb) {
        var done = function (r) { try { cb(r === undefined || r === null ? "" : String(r)); } catch (e) { console.warn("crema callback", m, e); } };
        if (typeof logos === "undefined" || !logos) { Qt.callLater(function () { done(""); }); return; }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync("crema_core", m, a || [], done, 20000); }
            catch (e) { Qt.callLater(function () { done(""); }); }
            return;
        }
        Qt.callLater(function () { var r = ""; try { r = logos.callModule("crema_core", m, a || []); } catch (e) {} done(r); });
    }
    function parse(raw) {
        var s = String(raw || "").trim();
        for (var i = 0; i < 2 && s.charAt(0) === "\""; i++) { try { s = String(JSON.parse(s)).trim(); } catch (e) { return null; } }
        if (s.charAt(0) !== "{") return null;
        try { return JSON.parse(s); } catch (e) { return null; }
    }
    function apply(o) {
        if (!o || !o.ok || o.status === undefined) return;
        root.st = o;
        if (root.tab === "studio" && !root.organizer) root.tab = "board";
    }
    property bool refreshBusy: false
    property double refreshSince: 0
    function refresh() {
        if (root.refreshBusy && Date.now() - root.refreshSince < 45000) return;
        root.refreshBusy = true; root.refreshSince = Date.now();
        callVia("snapshot", [], function (raw) { root.refreshBusy = false; apply(parse(raw)); });
    }
    property var inFlight: ({})
    function act(m, a, okMsg, cb) {
        if (root.inFlight[m]) return;
        var f = Object.assign({}, root.inFlight); f[m] = true; root.inFlight = f;
        callVia(m, a, function (raw) {
            var g = Object.assign({}, root.inFlight); delete g[m]; root.inFlight = g;
            var r = parse(raw);
            refresh();
            if (r && r.ok) { if (okMsg) toast(okMsg); if (cb) cb(r); return; }
            toast(r && r.error ? r.error : (raw === "" ? "The Crema core did not answer - is crema_core loaded?" : "Request failed"));
        });
    }
    function toast(m) { root.toastMsg = String(m); toastTimer.restart(); }
    function copyText(t, what) { clip.text = String(t || ""); clip.selectAll(); clip.copy(); clip.deselect(); toast((what || "Text") + " copied"); }
    function shortAddr(a) { a = String(a || ""); return a.length > 12 ? a.slice(0, 6) + "…" + a.slice(-4) : a; }
    function cafeName(id) {
        var l = root.catalog && root.catalog.cafes ? root.catalog.cafes : root.draftCafes;
        for (var i = 0; i < l.length; i++) if (l[i].id === id) return l[i].name;
        return id;
    }
    function fmtDay(ms) { return ms ? Qt.formatDate(new Date(Number(ms)), "d MMM yyyy") : ""; }
    function deg(e6) { return (Number(e6) / 1e6).toFixed(5); }

    // café mode: fetch the code now, then again as each 30 s step turns over
    function loadStamp() {
        if (!root.ev || !root.cafeId) return;
        callVia("cafeStamp", [root.ev.eventId, root.cafeId], function (raw) {
            var r = parse(raw);
            if (r && r.ok) { root.stamp = r; root.stampAt = Date.now(); }
        });
    }
    readonly property int secondsLeft: stamp ? Math.max(0, Math.ceil(stamp.secondsLeft - (nowTick - stampAt) / 1000)) : 0
    property double nowTick: Date.now()
    Timer {
        interval: 250; repeat: true; running: root.tab === "cafe" && !!root.cafeId
        onTriggered: { root.nowTick = Date.now(); if (!root.stamp || root.secondsLeft <= 0) root.loadStamp(); }
    }
    function openCafeMode(id) { root.cafeId = id; root.stamp = null; root.tab = "cafe"; loadStamp(); }

    function showQr(method, args) {
        root.qr = null;
        callVia(method, args, function (raw) {
            var r = parse(raw);
            if (r && r.ok) { root.qr = r; } else { root.dialog = ""; toast(r && r.error ? r.error : "No QR"); }
        });
    }

    Timer { interval: 3000; running: true; repeat: true; onTriggered: root.refresh() }
    Timer { id: toastTimer; interval: 3500; onTriggered: root.toastMsg = "" }
    Component.onCompleted: root.refresh()
    Connections {
        target: (typeof logos !== "undefined") ? logos : null
        ignoreUnknownSignals: true
        function onModuleEventReceived(module, event, data) {
            if (module === "crema_core" && event === "stateChanged") root.apply(root.parse(data));
        }
    }
    TextEdit { id: clip; visible: false }
    FileDialog {
        id: mapDialog
        title: "Choose the event map (.pmtiles)"
        nameFilters: ["PMTiles maps (*.pmtiles)"]
        onAccepted: root.act("importMap", [root.ev.eventId, String(selectedFile)], "Map imported - uploading to Logos Storage")
    }
    readonly property var draftMap: ev && ev.draft && ev.draft.map ? ev.draft.map : null
    readonly property var making: ev && ev.mapMaking ? ev.mapMaking : null
    function mapBadge() {
        if (root.making) return root.making.step === "failed" ? "Couldn't make it" : (root.making.step === "drawing" ? "Drawing…" : "Downloading…");
        if (!root.draftMap) return "";
        return root.draftMap.state === "ready" ? "On Logos Storage" : "Uploading " + (root.draftMap.piecesDone || 0) + "/" + (root.draftMap.piecesTotal || "?");
    }
    function makingText() {
        var k = root.making;
        if (k.step === "drawing") return "Drawing the tiles" + (k.tilesTotal ? ": " + k.tilesDone + " of " + k.tilesTotal : "…") + " (" + root.mb(k.bytes) + " of OpenStreetMap data)";
        return "Downloading OpenStreetMap data for the cafés' area" + (k.bytes ? ": " + root.mb(k.bytes) : "…") + " - usually under a minute";
    }
    function mb(b) { return (Number(b || 0) / 1048576).toFixed(1) + " MB"; }

    // ── building blocks ──
    component CButton: Rectangle {
        id: b
        property string label: ""
        property bool primary: false
        property bool danger: false
        property bool active: true
        signal clicked()
        implicitWidth: bt.implicitWidth + 28
        implicitHeight: 34
        radius: 8
        opacity: active ? 1 : 0.45
        color: primary ? root.cCrema : (ma.containsMouse && active ? root.cRaised : root.cCard)
        border.color: primary ? root.cCrema : (danger ? root.cBad : root.cLine)
        Text { id: bt; anchors.centerIn: parent; text: b.label; textFormat: Text.PlainText; font.pixelSize: 13; font.bold: b.primary
               color: b.primary ? root.cBg : (b.danger ? root.cBad : root.cText) }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: if (b.active) b.clicked() }
    }
    component CField: Rectangle {
        property alias text: inp.text
        property alias placeholder: ph.text
        implicitHeight: 36
        radius: 8; color: root.cBg; border.color: inp.activeFocus ? root.cCrema : root.cLine
        TextInput { id: inp; anchors.fill: parent; anchors.margins: 9; color: root.cText; font.pixelSize: 14; clip: true; selectByMouse: true }
        Text { id: ph; anchors.fill: inp; color: root.cMuted; font.pixelSize: 14; visible: !inp.text && !inp.activeFocus; textFormat: Text.PlainText }
    }
    component Label2: Text { color: root.cMuted; font.pixelSize: 12; textFormat: Text.PlainText }
    component QrBox: Rectangle {
        id: qb
        property var code: null
        function repaint() { cv.requestPaint(); }
        color: "white"; radius: 12
        onCodeChanged: cv.requestPaint()
        onWidthChanged: cv.requestPaint()
        Canvas {
            id: cv
            anchors.fill: parent; anchors.margins: Math.max(10, parent.width * 0.05)
            onPaint: {
                var ctx = getContext("2d"); ctx.reset();
                ctx.fillStyle = "white"; ctx.fillRect(0, 0, width, height);
                var q = qb.code; if (!q || !q.cells) return;
                var n = q.n, side = Math.min(width, height), s = Math.floor(side / n), off = Math.floor((side - s * n) / 2);
                ctx.fillStyle = "#1d1410";
                for (var y = 0; y < n; y++) for (var x = 0; x < n; x++) if (q.cells[y * n + x]) ctx.fillRect(off + x * s, off + y * s, s, s);
            }
        }
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── header ──
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 60; color: root.cCard
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.cLine }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 18; spacing: 12
                Image { source: "icon.png"; sourceSize.width: 30; sourceSize.height: 30; Layout.preferredWidth: 30; Layout.preferredHeight: 30 }
                Text { text: "Crema"; color: root.cCrema; font.pixelSize: 20; font.bold: true; textFormat: Text.PlainText }
                Rectangle {
                    implicitWidth: stT.implicitWidth + 20; implicitHeight: 24; radius: 12
                    color: root.connected ? "#24331d" : root.cRaised
                    Text { id: stT; anchors.centerIn: parent; font.pixelSize: 11; textFormat: Text.PlainText
                           color: root.connected ? root.cGood : root.cMuted; text: String(root.st.status || "Starting…") }
                }
                ComboBox {
                    id: evPick
                    visible: (root.st.events || []).length > 0
                    Layout.preferredWidth: 260
                    model: (root.st.events || []).map(function (e) { return (e.organizer ? "★ " : "") + e.name; })
                    currentIndex: { var l = root.st.events || []; for (var i = 0; i < l.length; i++) if (l[i].eventId === root.st.current) return i; return -1; }
                    onActivated: function (i) { var e = (root.st.events || [])[i]; if (e) root.act("selectEvent", [e.eventId], ""); }
                    implicitHeight: 34
                    background: Rectangle { radius: 8; color: root.cBg; border.color: evPick.hovered ? root.cCrema : root.cLine }
                    contentItem: Text { leftPadding: 12; rightPadding: 30; text: evPick.displayText; color: root.cText; font.pixelSize: 14
                                        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; textFormat: Text.PlainText }
                    indicator: Text { x: evPick.width - width - 12; anchors.verticalCenter: parent.verticalCenter; text: "▾"; color: root.cCrema; font.pixelSize: 14 }
                }
                Item { Layout.fillWidth: true }
                CButton { label: "Join event"; onClicked: { joinLink.text = ""; root.dialog = "join"; } }
                CButton { label: "New event"; primary: true; onClicked: root.dialog = "new" }
            }
        }

        // ── tabs ──
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 44; color: root.cBg; visible: !!root.ev
            Row {
                anchors.left: parent.left; anchors.leftMargin: 18; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                Repeater {
                    model: root.organizer ? [["studio", "Studio"], ["board", "Leaderboard"], ["cafe", "Café mode"]] : [["board", "Leaderboard"]]
                    delegate: Rectangle {
                        required property var modelData
                        width: tl.implicitWidth + 26; height: 30; radius: 15
                        color: root.tab === modelData[0] ? root.cCrema : "transparent"
                        border.color: root.tab === modelData[0] ? root.cCrema : root.cLine
                        Text { id: tl; anchors.centerIn: parent; text: parent.modelData[1]; textFormat: Text.PlainText; font.pixelSize: 13
                               color: root.tab === parent.modelData[0] ? root.cBg : root.cText }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.tab = parent.modelData[0] }
                    }
                }
            }
        }

        // ── no event yet ──
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true; visible: !root.ev
            ColumnLayout {
                anchors.centerIn: parent; spacing: 14; width: Math.min(parent.width - 40, 520)
                Text { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; text: "A coffee passport for your city"; color: root.cText; font.pixelSize: 24; font.bold: true; textFormat: Text.PlainText }
                Text { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: root.cMuted; font.pixelSize: 14; textFormat: Text.PlainText
                       text: "Create an event, add the cafés, and publish the signed café list. Players join with the event QR on the Crema phone app and collect a stamp at each café by scanning its rotating code. Everything syncs over Logos Delivery - no server." }
                RowLayout { Layout.alignment: Qt.AlignHCenter; spacing: 10
                    CButton { label: "New event"; primary: true; onClicked: root.dialog = "new" }
                    CButton { label: "Follow an event"; onClicked: { joinLink.text = ""; root.dialog = "join"; } }
                }
            }
        }

        // ── studio (organizer) ──
        ScrollView {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: !!root.ev && root.tab === "studio" && root.organizer
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: parent.width; spacing: 14
                Item { implicitHeight: 4 }
                // event card
                Rectangle {
                    Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18
                    implicitHeight: evCol.implicitHeight + 28; radius: 12; color: root.cCard; border.color: root.cLine
                    ColumnLayout {
                        id: evCol; anchors.fill: parent; anchors.margins: 14; spacing: 8
                        RowLayout {
                            Layout.fillWidth: true; spacing: 10
                            ColumnLayout { Layout.fillWidth: true; spacing: 2
                                Text { text: root.ev ? root.ev.name : ""; color: root.cText; font.pixelSize: 20; font.bold: true; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label2 { text: root.ev && root.ev.draft ? (root.ev.draft.city + " · " + root.fmtDay(root.ev.draft.start) + " – " + root.fmtDay(root.ev.draft.end)) : "" }
                            }
                            CButton { label: "Join QR"; onClicked: { root.dialog = "joinqr"; root.showQr("joinQr", [root.ev.eventId]); } }
                            CButton {
                                primary: true; active: root.draftCafes.length > 0 && !root.inFlight["publishCatalog"]
                                label: root.ev && root.ev.publishedVersion ? (root.ev.unpublished ? "Publish v" + (root.ev.publishedVersion + 1) : "Published v" + root.ev.publishedVersion) : "Publish"
                                onClicked: root.act("publishCatalog", [root.ev.eventId], "Café list signed and sent")
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true; visible: !!(root.ev && root.ev.unpublished); implicitHeight: 34; radius: 8; color: "#3a2a12"
                            Text { anchors.fill: parent; anchors.margins: 9; color: root.cCrema; font.pixelSize: 13; textFormat: Text.PlainText; elide: Text.ElideRight
                                   text: root.ev && root.ev.publishedVersion ? "You have changes the players don't see yet - publish to send them." : "Not published yet: players see no cafés until you publish." }
                        }
                        Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap
                                 text: "Players: " + root.board.length + " on the leaderboard · " + (root.ev ? root.ev.logSize : 0) + " events in the log · this desktop: " + root.shortAddr(root.st.me) }
                    }
                }
                // the event map (on Logos Storage; docs/adr/0002)
                Rectangle {
                    Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18
                    implicitHeight: mapCol.implicitHeight + 24; radius: 12; color: root.cCard; border.color: root.cLine
                    ColumnLayout {
                        id: mapCol; anchors.fill: parent; anchors.margins: 12; spacing: 6
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            Text { text: "Event map"; color: root.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText }
                            Rectangle {
                                visible: !!root.draftMap || !!root.making; implicitWidth: ms.implicitWidth + 16; implicitHeight: 22; radius: 11
                                color: root.making && root.making.step === "failed" ? "#3a1c16" : (root.draftMap && root.draftMap.state === "ready" && !root.making ? "#24331d" : "#3a2a12")
                                Text { id: ms; anchors.centerIn: parent; font.pixelSize: 11; textFormat: Text.PlainText
                                       color: root.making && root.making.step === "failed" ? root.cBad : (root.draftMap && root.draftMap.state === "ready" && !root.making ? root.cGood : root.cCrema)
                                       text: root.mapBadge() }
                            }
                            Item { Layout.fillWidth: true }
                            CButton { label: root.draftMap ? "Make again" : "Make map"; primary: !root.draftMap
                                      active: !(root.making && root.making.step !== "failed") && root.draftCafes.length > 0
                                      onClicked: root.act("makeMap", [root.ev.eventId, "500"], "Making the map - downloading OpenStreetMap data for the cafés' area") }
                            CButton { label: "Import file"; onClicked: mapDialog.open() }
                            CButton { label: "Remove"; danger: true; visible: !!root.draftMap; onClicked: root.act("removeMap", [root.ev.eventId], "Map removed (publish to apply)") }
                        }
                        Label2 {
                            Layout.fillWidth: true; wrapMode: Text.WordWrap
                            text: root.making && root.making.step !== "failed" ? root.makingText()
                                  : !root.draftMap
                                  ? "Phones draw the map from tiles made here - no map servers. Make map downloads OpenStreetMap data for the cafés' area (500 m around them), draws our own vector tiles and puts them on Logos Storage in small pieces, so phones show a coarse map within seconds and sharpen it as more arrives. Or import a .pmtiles file."
                                  : root.mb(root.draftMap.size) + " · zoom " + (root.draftMap.minzoom !== undefined ? root.draftMap.minzoom + "-" : "") + root.draftMap.maxzoom
                                    + (root.draftMap.piecesTotal ? " · " + root.draftMap.piecesDone + "/" + root.draftMap.piecesTotal + " pieces on Storage" : "")
                                    + (root.draftMap.state === "ready" ? ((root.ev.catalog && root.ev.catalog.map && root.ev.catalog.map.sha256 === root.draftMap.sha256) ? " · published" : " · publish to send it to the players") : "")
                        }
                        Rectangle {
                            visible: !!(root.draftMap && root.draftMap.state === "uploading" && root.draftMap.piecesTotal)
                            Layout.fillWidth: true; implicitHeight: 6; radius: 3; color: root.cLine
                            Rectangle { height: parent.height; radius: 3; color: root.cCrema
                                        width: root.draftMap && root.draftMap.piecesTotal ? parent.width * root.draftMap.piecesDone / root.draftMap.piecesTotal : 0 }
                        }
                        Text { visible: !!((root.draftMap && root.draftMap.error) || (root.making && root.making.error)); Layout.fillWidth: true; wrapMode: Text.WordWrap; color: root.cBad; font.pixelSize: 12; textFormat: Text.PlainText
                               text: root.making && root.making.error ? root.making.error : (root.draftMap && root.draftMap.error ? root.draftMap.error : "") }
                    }
                }
                // cafés
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18; spacing: 8
                    Text { text: "Cafés (" + root.draftCafes.length + ")"; color: root.cText; font.pixelSize: 16; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true }
                    CButton { label: "Import list"; onClicked: { importText.text = ""; root.dialog = "import"; } }
                    CButton { label: "Add café"; onClicked: { addId.text = ""; addName.text = ""; addLat.text = ""; addLon.text = ""; addAddr.text = ""; root.dialog = "add"; } }
                }
                Label2 { Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.fillWidth: true; wrapMode: Text.WordWrap; visible: root.draftCafes.length > 0
                         text: "Each café has its own key. Give the café its SETUP code in person (it turns their phone or tablet into the stamp display) - anyone holding it can stamp for that café." }
                Repeater {
                    model: root.draftCafes
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18
                        implicitHeight: 54; radius: 10; color: root.cCard; border.color: root.cLine
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 10; spacing: 8
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                Text { text: modelData.name; color: root.cText; font.pixelSize: 14; font.bold: true; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label2 { Layout.fillWidth: true; elide: Text.ElideRight
                                         text: modelData.id + " · " + root.deg(modelData.latE6) + ", " + root.deg(modelData.lonE6) + " · " + (modelData.radiusM || 75) + " m" + (modelData.address ? " · " + modelData.address : "") }
                            }
                            CButton { label: "Setup code"; onClicked: { root.dialog = "setup"; root.showQr("cafeSetupQr", [root.ev.eventId, modelData.id]); } }
                            CButton { label: "Café mode"; active: !!root.catalog; onClicked: root.openCafeMode(modelData.id) }
                            CButton { label: "Remove"; danger: true; onClicked: root.act("removeCafe", [root.ev.eventId, modelData.id], modelData.name + " removed (publish to apply)") }
                        }
                    }
                }
                Item { implicitHeight: 20 }
            }
        }

        // ── leaderboard (big-screen friendly) ──
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: !!root.ev && root.tab === "board"
            readonly property real k: Math.max(1, Math.min(width / 900, 1.8))
            id: boardPane
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 18 * boardPane.k; spacing: 10 * boardPane.k
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout { Layout.fillWidth: true; spacing: 0
                        Text { text: root.catalog ? root.catalog.name : (root.ev ? root.ev.name : ""); color: root.cText; font.pixelSize: 26 * boardPane.k; font.bold: true; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label2 { font.pixelSize: 13 * boardPane.k
                                 text: root.catalog ? (root.totalCafes + " cafés · " + root.fmtDay(root.catalog.start) + " – " + root.fmtDay(root.catalog.end)) : "Waiting for the organizer's café list…" }
                    }
                    Text { text: root.board.length + (root.board.length === 1 ? " player" : " players"); color: root.cCrema; font.pixelSize: 18 * boardPane.k; textFormat: Text.PlainText }
                }
                Text { visible: root.board.length === 0; Layout.topMargin: 30; Layout.alignment: Qt.AlignHCenter; color: root.cMuted; font.pixelSize: 16 * boardPane.k; textFormat: Text.PlainText
                       text: "No stamps yet. Scan the join QR with the Crema app and go get a coffee." }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8 * boardPane.k
                    model: root.board
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: ListView.view.width; height: 64 * boardPane.k; radius: 12
                        color: index === 0 ? "#3a2a12" : root.cCard; border.color: index < 3 ? root.cCrema : root.cLine
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 16 * boardPane.k; anchors.rightMargin: 16 * boardPane.k; spacing: 14 * boardPane.k
                            Rectangle {
                                Layout.preferredWidth: 36 * boardPane.k; Layout.preferredHeight: 36 * boardPane.k; radius: width / 2
                                color: index === 0 ? "#d9a066" : (index === 1 ? "#b8b0a6" : (index === 2 ? "#a86f45" : "transparent"))
                                border.color: index < 3 ? "transparent" : root.cLine
                                Text { anchors.centerIn: parent; text: String(index + 1); font.pixelSize: 16 * boardPane.k; font.bold: true; textFormat: Text.PlainText
                                       color: index < 3 ? root.cBg : root.cMuted }
                            }
                            ColumnLayout { Layout.fillWidth: true; spacing: 2
                                Text { text: modelData.name ? modelData.name : root.shortAddr(modelData.author); color: root.cText; font.pixelSize: 18 * boardPane.k; font.bold: true; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label2 { font.pixelSize: 12 * boardPane.k; elide: Text.ElideRight; Layout.fillWidth: true
                                         text: (modelData.visited || []).map(function (v) { return root.cafeName(v); }).join(" · ") }
                            }
                            // progress through the city
                            Rectangle {
                                Layout.preferredWidth: 160 * boardPane.k; implicitHeight: 8 * boardPane.k; radius: height / 2; color: root.cLine
                                visible: root.totalCafes > 0
                                Rectangle { height: parent.height; radius: parent.radius; color: root.cCrema; width: parent.width * Math.min(1, modelData.cafes / Math.max(1, root.totalCafes)) }
                            }
                            Text { text: modelData.cafes + (root.totalCafes ? " / " + root.totalCafes : ""); color: root.cCrema; font.pixelSize: 22 * boardPane.k; font.bold: true; textFormat: Text.PlainText
                                   Layout.preferredWidth: 90 * boardPane.k; horizontalAlignment: Text.AlignRight }
                        }
                    }
                }
            }
        }

        // ── café mode: this screen is the café's stamp display ──
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: !!root.ev && root.tab === "cafe"
            ColumnLayout {
                anchors.centerIn: parent; spacing: 14; visible: !!root.cafeId
                Text { Layout.alignment: Qt.AlignHCenter; text: root.cafeName(root.cafeId); color: root.cText; font.pixelSize: 26; font.bold: true; textFormat: Text.PlainText }
                Label2 { Layout.alignment: Qt.AlignHCenter; font.pixelSize: 14; text: "Scan with the Crema app to collect your stamp" }
                QrBox {
                    id: stampBox
                    Layout.alignment: Qt.AlignHCenter
                    readonly property real side: Math.max(200, Math.min(root.width - 80, root.height - 330))
                    Layout.preferredWidth: side; Layout.preferredHeight: side
                    code: root.stamp
                }
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: stampBox.side; implicitHeight: 6; radius: 3; color: root.cLine
                    Rectangle { height: parent.height; radius: 3; color: root.cCrema; width: parent.width * root.secondsLeft / 30 }
                }
                Label2 { Layout.alignment: Qt.AlignHCenter; text: root.stamp ? "New code in " + root.secondsLeft + " s" : "…" }
                CButton { Layout.alignment: Qt.AlignHCenter; label: "Choose another café"; onClicked: { root.cafeId = ""; root.stamp = null; } }
            }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 18; spacing: 8; visible: !root.cafeId
                Text { text: "Which café is this screen?"; color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText }
                Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap
                         text: "Café mode shows the café's rotating stamp code (a new one every 30 s). Players' phones also check they're within the café's radius, so a photo of the code won't work from home. Cafés usually run this on their own phone or tablet from the setup code instead." }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    Repeater {
                        model: root.catalog && root.catalog.cafes ? root.catalog.cafes : []
                        delegate: CButton { required property var modelData; label: modelData.name; onClicked: root.openCafeMode(modelData.id) }
                    }
                }
            }
        }
    }

    // ── dialogs ──
    Rectangle {
        anchors.fill: parent; color: "#b0000000"; visible: root.dialog !== ""
        MouseArea { anchors.fill: parent; onClicked: root.dialog = "" }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 40, 520); height: dlg.implicitHeight + 36
            radius: 14; color: root.cCard; border.color: root.cLine
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                id: dlg; anchors.fill: parent; anchors.margins: 18; spacing: 10

                // new event
                ColumnLayout {
                    visible: root.dialog === "new"; Layout.fillWidth: true; spacing: 8
                    Text { text: "New event"; color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText }
                    Label2 { text: "Name" }
                    CField { id: newName; Layout.fillWidth: true; placeholder: "Brno Coffee Week" }
                    Label2 { text: "City" }
                    CField { id: newCity; Layout.fillWidth: true; placeholder: "Brno" }
                    RowLayout { Layout.fillWidth: true; spacing: 10
                        ColumnLayout { Layout.fillWidth: true; Label2 { text: "First day (YYYY-MM-DD)" }
                    CField { id: newStart; Layout.fillWidth: true; text: Qt.formatDate(new Date(), "yyyy-MM-dd") } }
                        ColumnLayout { Layout.preferredWidth: 110; Label2 { text: "Days" }
                    CField { id: newDays; Layout.fillWidth: true; text: "7" } }
                    }
                    Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: "This desktop becomes the organizer: only it can sign the café list. Keep it (and its data folder) safe." }
                    RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                        CButton { label: "Cancel"; onClicked: root.dialog = "" }
                        CButton { label: "Create"; primary: true; onClicked: root.act("createEvent", [newName.text.trim(), newCity.text.trim(), newStart.text.trim(), newDays.text.trim()], "Event created",
                                                                                   function () { root.dialog = ""; root.tab = "studio"; }) }
                    }
                }

                // join (follow) an event
                ColumnLayout {
                    visible: root.dialog === "join"; Layout.fillWidth: true; spacing: 8
                    Text { text: "Follow an event"; color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText }
                    Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: "Paste the event's join link (crema:join?…). This desktop shows its leaderboard - handy for a big screen at the venue." }
                    CField { id: joinLink; Layout.fillWidth: true; placeholder: "crema:join?e=…" }
                    RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                        CButton { label: "Cancel"; onClicked: root.dialog = "" }
                        CButton { label: "Follow"; primary: true; onClicked: root.act("joinEvent", [joinLink.text.trim()], "Following the event", function () { root.dialog = ""; root.tab = "board"; }) }
                    }
                }

                // a QR: the event's join code or a café's setup code
                ColumnLayout {
                    visible: root.dialog === "joinqr" || root.dialog === "setup"; Layout.fillWidth: true; spacing: 10
                    Text { color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight
                           text: root.dialog === "setup" ? "Setup code · " + (root.qr && root.qr.name ? root.qr.name : "") : "Join " + (root.ev ? root.ev.name : "") }
                    Rectangle {
                        Layout.fillWidth: true; visible: root.dialog === "setup"; implicitHeight: warn.implicitHeight + 16; radius: 8; color: "#3a1c16"
                        Text { id: warn; anchors.fill: parent; anchors.margins: 8; wrapMode: Text.WordWrap; color: root.cBad; font.pixelSize: 13; textFormat: Text.PlainText
                               text: "This code holds the café's secret key. Show it only to the café, in person - never post or send it. Scanned with the Crema phone app's Scan button, it turns their phone or tablet into the café's stamp display." }
                    }
                    Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap; visible: root.dialog === "joinqr"
                             text: "Players scan this in the Crema app to join. Print it, post it, put it on the event page - it lets people play and read the leaderboard, nothing more." }
                    QrBox { id: dialogQr; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 280; Layout.preferredHeight: 280; code: root.qr }
                    RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                        CButton { label: "Copy link"; visible: root.dialog === "joinqr"; active: !!root.qr; onClicked: root.copyText(root.qr.link, "Join link") }
                        CButton { label: "Done"; primary: true; onClicked: { root.dialog = ""; root.qr = null; } }
                    }
                }

                // import a list
                ColumnLayout {
                    visible: root.dialog === "import"; Layout.fillWidth: true; spacing: 8
                    Text { text: "Import cafés"; color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText }
                    Label2 { Layout.fillWidth: true; wrapMode: Text.WordWrap
                             text: "Paste a JSON list - [{\"id\":\"kafec\",\"name\":\"Kafec\",\"latE6\":49192990,\"lonE6\":16611550}, …] - or a whole seed file such as data/brno-cafes.json. Cafés with an id you already have are replaced; new ones get a key." }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 200; radius: 8; color: root.cBg; border.color: root.cLine
                        ScrollView { anchors.fill: parent; anchors.margins: 6
                            TextArea { id: importText; color: root.cText; font.family: "monospace"; font.pixelSize: 12; wrapMode: TextEdit.WrapAnywhere; background: null } }
                    }
                    RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                        CButton { label: "Cancel"; onClicked: root.dialog = "" }
                        CButton { label: "Import"; primary: true; onClicked: root.act("importCafes", [root.ev.eventId, importText.text], "", function (r) {
                                      root.dialog = ""; root.toast(r.added + " added, " + r.replaced + " updated - publish to send them"); }) }
                    }
                }

                // add one café
                ColumnLayout {
                    visible: root.dialog === "add"; Layout.fillWidth: true; spacing: 8
                    Text { text: "Add a café"; color: root.cText; font.pixelSize: 18; font.bold: true; textFormat: Text.PlainText }
                    RowLayout { Layout.fillWidth: true; spacing: 10
                        ColumnLayout { Layout.preferredWidth: 150; Label2 { text: "Id (a-z, 0-9, -)" }
                    CField { id: addId; Layout.fillWidth: true; placeholder: "kafec" } }
                        ColumnLayout { Layout.fillWidth: true; Label2 { text: "Name" }
                    CField { id: addName; Layout.fillWidth: true; placeholder: "Kafec" } }
                    }
                    RowLayout { Layout.fillWidth: true; spacing: 10
                        ColumnLayout { Layout.fillWidth: true; Label2 { text: "Latitude" }
                    CField { id: addLat; Layout.fillWidth: true; placeholder: "49.19299" } }
                        ColumnLayout { Layout.fillWidth: true; Label2 { text: "Longitude" }
                    CField { id: addLon; Layout.fillWidth: true; placeholder: "16.61155" } }
                    }
                    Label2 { text: "Address (optional)" }
                    CField { id: addAddr; Layout.fillWidth: true }
                    RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                        CButton { label: "Cancel"; onClicked: root.dialog = "" }
                        CButton { label: "Add"; primary: true; onClicked: {
                            var la = parseFloat(addLat.text.replace(",", ".")), lo = parseFloat(addLon.text.replace(",", "."));
                            if (isNaN(la) || isNaN(lo)) { root.toast("Latitude and longitude as decimal degrees"); return; }
                            var c = { id: addId.text.trim().toLowerCase(), name: addName.text.trim(), latE6: Math.round(la * 1e6), lonE6: Math.round(lo * 1e6) };
                            if (addAddr.text.trim()) c.address = addAddr.text.trim();
                            root.act("addCafe", [root.ev.eventId, JSON.stringify(c)], c.name + " added - publish to send it", function () { root.dialog = ""; });
                        } }
                    }
                }
            }
        }
    }

    // ── toast ──
    Rectangle {
        anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: 24
        visible: root.toastMsg !== ""; radius: 10; color: root.cRaised; border.color: root.cCrema
        width: Math.min(parent.width - 40, tt.implicitWidth + 32); height: tt.implicitHeight + 20
        Text { id: tt; anchors.centerIn: parent; width: Math.min(root.width - 72, implicitWidth); wrapMode: Text.WordWrap; text: root.toastMsg; color: root.cText; font.pixelSize: 13; textFormat: Text.PlainText }
    }
}
