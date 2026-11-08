// The two hand-written native modules: ZXing QR scanning (no Google services) and one location
// fix from Android's LocationManager (no Play services).
import { NativeModules, PermissionsAndroid, Platform } from "react-native";

export async function scanQr(prompt: string): Promise<string | null> {
  const m = NativeModules.CremaQrScan;
  if (!m) throw new Error("The scanner isn't available in this build");
  const r = await m.scan(prompt);
  return r ? String(r) : null;               // null: cancelled
}

export async function locationFix(timeoutMs = 15000): Promise<{ lat: number; lon: number; accuracy: number }> {
  if (Platform.OS !== "android") throw new Error("Location: Android only");
  const granted = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION, {
    title: "Your location",
    message: "Crema uses it to check you're at a café when you stamp, and to find the café nearest to you. It stays on this phone.",
    buttonPositive: "OK",
  });
  if (granted !== PermissionsAndroid.RESULTS.GRANTED) throw new Error("Location permission is needed to stamp");
  const m = NativeModules.CremaGeoFix;
  if (!m) throw new Error("Location isn't available in this build");
  const f = await m.fix(timeoutMs);
  return { lat: f.lat, lon: f.lon, accuracy: f.accuracy };
}
