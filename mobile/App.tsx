// Crema - the coffee passport. Join an event by scanning the organizer's QR, then scan each café's
// rotating code at the café. Everything syncs over Logos (Loam's shared node); the leaderboard is
// computed on this phone from signed stamps.
import React, { useEffect, useMemo, useState } from "react";
import { Alert, FlatList, Pressable, ScrollView, StyleSheet, Switch, Text, TextInput, View } from "react-native";
import { SafeAreaProvider, SafeAreaView } from "react-native-safe-area-context";
import { StatusBar } from "expo-status-bar";
import { WebView } from "react-native-webview";
import QRCode from "react-native-qrcode-svg";
import { useKeepAwake } from "expo-keep-awake";
import { crema, P } from "./src/lib/crema";
import { scanQr, locationFix } from "./src/lib/native";
import { SharedNodeStatus } from "./src/lib/loam-transport-pkg/src/SharedNodeStatus";

const C = { bg: "#1d1410", card: "#2a1d16", line: "#3a2a20", crema: "#d9a066", text: "#f3e7da", muted: "#b59c86", good: "#3d5a2f", bad: "#6b2a22" };
type Tab = "passport" | "map" | "board" | "me";

function useCrema() {
  const [, setN] = useState(0);
  useEffect(() => crema.on(() => setN((n) => n + 1)), []);
  return crema;
}

const short = (a: string) => (a ? a.slice(0, 6) + "…" + a.slice(-4) : "");

export default function App() {
  const c = useCrema();
  const [ready, setReady] = useState(false);
  const [tab, setTab] = useState<Tab>("passport");
  const [toast, setToast] = useState<{ text: string; good: boolean } | null>(null);
  const [cafeOn, setCafeOn] = useState(false);
  useEffect(() => { c.boot().finally(() => setReady(true)); }, []);
  useEffect(() => { if (!toast) return; const t = setTimeout(() => setToast(null), 5000); return () => clearTimeout(t); }, [toast]);

  async function scan() {
    try {
      const text = await scanQr("Scan a café's Crema code, or an event's join code");
      if (!text) return;
      setToast({ text: "Checking…", good: true });
      const r = await c.handleScan(text, () => locationFix());
      setToast({ text: r.message, good: r.ok });
      if (r.kind === "cafe" && r.ok) setCafeOn(true);
    } catch (e: any) { setToast({ text: String(e?.message || e), good: false }); }
  }

  if (cafeOn && c.cafe) return <CafeMode onExit={() => setCafeOn(false)} />;
  const g = c.game();
  const s = g ? g.state() : null;

  return (
    <SafeAreaProvider>
      <SafeAreaView style={st.root} edges={["top", "bottom"]}>
        <StatusBar style="light" />
        <View style={st.header}>
          <Text style={st.brand}>CREMA</Text>
          <Text style={st.event} numberOfLines={1}>{s?.eventName || ""}</Text>
        </View>
        {c.net.started ? <SharedNodeStatus appName="Crema" /> : null}
        {!ready ? <Text style={st.muted}>Starting…</Text> : !g ? <Welcome onScan={scan} /> : (
          <View style={{ flex: 1 }}>
            {tab === "passport" && <Passport />}
            {tab === "map" && <MapTab />}
            {tab === "board" && <Board />}
            {tab === "me" && <Me onCafe={() => setCafeOn(true)} />}
          </View>
        )}
        {toast ? <View style={[st.toast, { backgroundColor: toast.good ? C.good : C.bad }]}><Text style={st.toastText}>{toast.text}</Text></View> : null}
        {g ? (
          <View style={st.nav}>
            <NavBtn label="Passport" on={tab === "passport"} onPress={() => setTab("passport")} />
            <NavBtn label="Map" on={tab === "map"} onPress={() => setTab("map")} />
            <Pressable style={st.scan} onPress={scan}><Text style={st.scanText}>Scan</Text></Pressable>
            <NavBtn label="Board" on={tab === "board"} onPress={() => setTab("board")} />
            <NavBtn label="Me" on={tab === "me"} onPress={() => setTab("me")} />
          </View>
        ) : null}
      </SafeAreaView>
    </SafeAreaProvider>
  );
}

function NavBtn({ label, on, onPress }: { label: string; on: boolean; onPress: () => void }) {
  return <Pressable style={[st.navBtn, on && st.navOn]} onPress={onPress}><Text style={[st.navText, on && { color: C.text }]}>{label}</Text></Pressable>;
}

function Welcome({ onScan }: { onScan: () => void }) {
  return (
    <ScrollView contentContainerStyle={{ padding: 20, gap: 14 }}>
      <Text style={st.h1}>The coffee passport</Text>
      <Text style={st.body}>Visit the good coffee places in town and collect a stamp at each. Every café shows a code that changes every 30 seconds - scan it while you're there.</Text>
      <Text style={st.body}>To start, scan the event's join code from the organizer.</Text>
      <Pressable style={st.primary} onPress={onScan}><Text style={st.primaryText}>Scan the event's code</Text></Pressable>
      <Text style={st.muted}>No account, no server: your stamps are signed on this phone and shared over Logos with the other players.</Text>
    </ScrollView>
  );
}

function Passport() {
  const g = crema.game()!;
  const s = g.state();
  if (!s.catalog) return <View style={{ padding: 20 }}><Text style={st.body}>Waiting for the café list…</Text><Text style={st.muted}>It comes from the other players or the organizer over Logos - keep the app open online for a moment.</Text></View>;
  const cafes = s.catalog.cafes;
  return (
    <FlatList
      contentContainerStyle={{ padding: 16, paddingBottom: 120 }}
      ListHeaderComponent={
        <View style={{ marginBottom: 8 }}>
          <Text style={st.big}>{s.myVisited.size} / {cafes.length}</Text>
          <Text style={st.muted}>cafés stamped{s.share ? "" : " · you're not on the leaderboard"}</Text>
        </View>
      }
      data={cafes}
      keyExtractor={(x: any) => x.id}
      renderItem={({ item }: any) => {
        const done = s.myVisited.has(item.id);
        return (
          <View style={[st.row, done && { borderColor: C.crema }]}>
            <Text style={st.stamp}>{done ? "☕" : "○"}</Text>
            <View style={{ flex: 1 }}><Text style={st.rowTitle}>{item.name}</Text>{item.address ? <Text style={st.muted}>{item.address}</Text> : null}</View>
          </View>
        );
      }}
    />
  );
}

function MapTab() {
  const g = crema.game()!;
  const s = g.state();
  const html = useMemo(() => {
    const cafes = (s.catalog?.cafes || []).map((c: any) => ({ n: c.name, a: c.address || "", lat: c.latE6 / 1e6, lon: c.lonE6 / 1e6, v: s.myVisited.has(c.id) }));
    return `<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<style>html,body,#m{margin:0;height:100%;background:#1d1410}</style></head><body><div id="m"></div>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script><script>
var cafes=${JSON.stringify(cafes)};var m=L.map('m');
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'© OpenStreetMap'}).addTo(m);
var pts=[];cafes.forEach(function(c){var k=L.circleMarker([c.lat,c.lon],{radius:9,color:'#2a1d16',weight:2,fillColor:c.v?'#d9a066':'#6b4f3f',fillOpacity:.95}).addTo(m);
var e=document.createElement('div');var b=document.createElement('b');b.textContent=c.n;e.appendChild(b);e.appendChild(document.createElement('br'));e.appendChild(document.createTextNode(c.a));k.bindPopup(e);pts.push([c.lat,c.lon]);});
if(pts.length)m.fitBounds(pts,{padding:[30,30]});else m.setView([49.195,16.608],14);
</script></body></html>`;
  }, [s.catalog?.version, s.myVisited.size]);
  return <WebView originWhitelist={["*"]} source={{ html }} style={{ flex: 1, backgroundColor: C.bg }} />;
}

function Board() {
  const g = crema.game()!;
  const s = g.state();
  const total = s.catalog?.cafes.length || 0;
  return (
    <FlatList
      contentContainerStyle={{ padding: 16, paddingBottom: 120 }}
      ListHeaderComponent={<Text style={[st.muted, { marginBottom: 8 }]}>Distinct cafés stamped; ties go to whoever got there first. Computed on this phone from everyone's signed stamps.</Text>}
      ListEmptyComponent={<Text style={st.body}>No stamps yet. Be the first ☕</Text>}
      data={s.leaderboard}
      keyExtractor={(x: any) => x.author}
      renderItem={({ item, index }: any) => (
        <View style={[st.row, item.author === s.author && { borderColor: C.crema }]}>
          <Text style={st.rank}>{index + 1}</Text>
          <View style={{ flex: 1 }}><Text style={st.rowTitle}>{item.name || short(item.author)}{item.author === s.author ? "  (you)" : ""}</Text></View>
          <Text style={st.score}>{item.cafes}{total ? " / " + total : ""}</Text>
        </View>
      )}
    />
  );
}

function Me({ onCafe }: { onCafe: () => void }) {
  const c = useCrema();
  const g = c.game()!;
  const s = g.state();
  const [name, setName] = useState(s.names.get(s.author) || "");
  return (
    <ScrollView contentContainerStyle={{ padding: 16, gap: 14, paddingBottom: 120 }}>
      <Text style={st.label}>Name on the leaderboard</Text>
      <TextInput style={st.input} value={name} onChangeText={setName} onEndEditing={() => c.setName(name)} maxLength={40} placeholder="optional" placeholderTextColor={C.muted} />
      <View style={st.switchRow}>
        <View style={{ flex: 1 }}><Text style={st.body}>Show me on the leaderboard</Text><Text style={st.muted}>Off: your stamps stay on this phone. Turning it on later shares them.</Text></View>
        <Switch value={s.share} onValueChange={(v) => {
          if (!v) { Alert.alert("Leave the leaderboard?", "Stamps you already shared stay with the other players; new ones stay on this phone.", [{ text: "Cancel" }, { text: "Leave", onPress: () => c.setShare(false) }]); }
          else c.setShare(true);
        }} />
      </View>
      <View style={st.switchRow}>
        <View style={{ flex: 1 }}><Text style={st.body}>Use Loam's shared node</Text><Text style={st.muted}>One Logos node for all apps on this phone (needs the Loam app). Applies at the next start.</Text></View>
        <Switch value={c.net.shared} onValueChange={(v) => c.setSharedNode(v)} />
      </View>
      <Text style={st.muted}>Network: {c.net.status}{c.net.error ? " - " + c.net.error : ""}</Text>
      {c.games.size > 1 ? (
        <View style={{ gap: 6 }}>
          <Text style={st.label}>Events</Text>
          {[...c.games.values()].map((x) => (
            <Pressable key={x.join.eventId} style={[st.row, x.join.eventId === c.current && { borderColor: C.crema }]} onPress={() => c.select(x.join.eventId)}>
              <Text style={st.rowTitle}>{x.state().eventName}</Text>
            </Pressable>
          ))}
        </View>
      ) : null}
      {c.cafe ? (
        <Pressable style={st.ghost} onPress={onCafe}><Text style={st.body}>Show {c.cafe.name}'s stamp code</Text></Pressable>
      ) : <Text style={st.muted}>Running a café in the event? Scan the café setup code from the organizer to turn this device into your stamp display.</Text>}
      <Text style={st.muted}>You: {s.author}</Text>
    </ScrollView>
  );
}

function CafeMode({ onExit }: { onExit: () => void }) {
  useKeepAwake();
  const c = useCrema();
  const [now, setNow] = useState(Date.now());
  useEffect(() => { const t = setInterval(() => setNow(Date.now()), 1000); return () => clearInterval(t); }, []);
  const text = c.cafeStamp(now);
  const left = P.STEP_S - Math.floor(now / 1000) % P.STEP_S;
  return (
    <SafeAreaProvider>
      <SafeAreaView style={[st.root, { alignItems: "center", justifyContent: "center", gap: 18, padding: 24 }]}>
        <StatusBar hidden />
        <Text style={st.brand}>CREMA</Text>
        <Text style={st.h1}>{c.cafe?.name}</Text>
        <Text style={st.muted}>{c.cafe?.eventName}</Text>
        <View style={{ backgroundColor: "#fff", padding: 18, borderRadius: 24 }}>{text ? <QRCode value={text} size={280} /> : null}</View>
        <Text style={st.body}>Scan with Crema to collect this café's stamp · new code in {left}s</Text>
        <Pressable onLongPress={() => Alert.alert("Leave café mode?", "The code stops showing. (The café key stays on this device.)", [{ text: "Stay" }, { text: "Leave", onPress: onExit }])}>
          <Text style={st.muted}>hold to exit</Text>
        </Pressable>
      </SafeAreaView>
    </SafeAreaProvider>
  );
}

const st = StyleSheet.create({
  root: { flex: 1, backgroundColor: C.bg },
  header: { paddingHorizontal: 16, paddingTop: 8, paddingBottom: 6, flexDirection: "row", alignItems: "baseline", justifyContent: "space-between", gap: 12 },
  brand: { color: C.crema, fontWeight: "700", letterSpacing: 5 },
  event: { color: C.muted, flexShrink: 1 },
  h1: { color: C.text, fontSize: 26, fontWeight: "800", textAlign: "center" },
  big: { color: C.text, fontSize: 44, fontWeight: "800" },
  body: { color: C.text, fontSize: 15, lineHeight: 21 },
  muted: { color: C.muted, fontSize: 13, lineHeight: 18 },
  label: { color: C.muted, fontSize: 13, textTransform: "uppercase", letterSpacing: 1 },
  row: { flexDirection: "row", alignItems: "center", gap: 12, backgroundColor: C.card, borderRadius: 12, padding: 12, marginVertical: 4, borderWidth: 1, borderColor: "transparent" },
  rowTitle: { color: C.text, fontSize: 16, fontWeight: "600" },
  stamp: { fontSize: 24, width: 32, textAlign: "center", color: C.muted },
  rank: { color: C.crema, fontSize: 18, fontWeight: "800", width: 28 },
  score: { color: C.text, fontSize: 18, fontWeight: "700" },
  nav: { flexDirection: "row", alignItems: "center", backgroundColor: C.card, paddingHorizontal: 6, paddingVertical: 6, gap: 4 },
  navBtn: { flex: 1, alignItems: "center", paddingVertical: 10, borderRadius: 10 },
  navOn: { backgroundColor: C.line },
  navText: { color: C.muted, fontSize: 14 },
  scan: { backgroundColor: C.crema, borderRadius: 14, paddingVertical: 12, paddingHorizontal: 16 },
  scanText: { color: C.bg, fontWeight: "800", fontSize: 15 },
  primary: { backgroundColor: C.crema, borderRadius: 12, padding: 14, alignItems: "center" },
  primaryText: { color: C.bg, fontWeight: "800", fontSize: 16 },
  ghost: { borderWidth: 1, borderColor: C.muted, borderRadius: 12, padding: 12, alignItems: "center" },
  input: { backgroundColor: C.card, color: C.text, borderRadius: 10, padding: 12, borderWidth: 1, borderColor: C.line },
  switchRow: { flexDirection: "row", alignItems: "center", gap: 12 },
  toast: { position: "absolute", left: 16, right: 16, bottom: 90, padding: 14, borderRadius: 12 },
  toastText: { color: C.text, fontSize: 15 },
});
