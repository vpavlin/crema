// Entropy first: @noble draws from crypto.getRandomValues, which Hermes doesn't have.
import "react-native-get-random-values";
// Hermes has no Node Buffer; loam-sync's catch-up fingerprint hex-encodes with Buffer.
import { Buffer } from "buffer";
if (typeof (globalThis as any).Buffer === "undefined") (globalThis as any).Buffer = Buffer;
import { installGlobalHandler } from "./src/lib/crashlog";
installGlobalHandler();
import { registerRootComponent } from "expo";
import App from "./App";
registerRootComponent(App);
