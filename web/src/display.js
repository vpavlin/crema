// Crema café display: shows the café's rotating stamp QR. Runs on any old phone or tablet at the
// counter (until the e-ink device exists). No network needed - only a correct clock.
//
// Provisioning: the organizer opens display.html#c=<cafeId>&k=<secret hex>&n=<café name>&e=<event name>
// once; the page keeps it in this browser and removes it from the address bar.
import qrcode from "qrcode-generator";
import { makeStamp, STEP_S } from "../../protocol/src/index.js";

const KEY = "crema.display.v1";
const $ = (id) => document.getElementById(id);

function readProvisioning() {
  const h = new URLSearchParams(location.hash.slice(1));
  if (h.get("c") && h.get("k")) {
    const cfg = { cafeId: h.get("c"), secret: h.get("k"), name: h.get("n") || h.get("c"), event: h.get("e") || "" };
    localStorage.setItem(KEY, JSON.stringify(cfg));
    history.replaceState(null, "", location.pathname);   // the secret leaves the address bar
    return cfg;
  }
  try { return JSON.parse(localStorage.getItem(KEY) || "null"); } catch { return null; }
}

function render(cfg) {
  const now = Date.now();
  let text;
  try { text = makeStamp(cfg.cafeId, cfg.secret, now); } catch (e) { $("err").textContent = e.message; return; }
  const qr = qrcode(0, "M");
  qr.addData(text);
  qr.make();
  $("qr").innerHTML = qr.createSvgTag({ cellSize: 8, margin: 2, scalable: true });
  const msLeft = STEP_S * 1000 - (now % (STEP_S * 1000));
  $("bar").style.transition = "none";
  $("bar").style.width = (100 * msLeft) / (STEP_S * 1000) + "%";
  requestAnimationFrame(() => { $("bar").style.transition = `width ${msLeft}ms linear`; $("bar").style.width = "0%"; });
  $("clock").textContent = new Date(now).toLocaleTimeString();
  setTimeout(() => render(cfg), msLeft + 50);
}

async function keepAwake() {
  try { if ("wakeLock" in navigator) await navigator.wakeLock.request("screen"); } catch {}
}

const cfg = readProvisioning();
if (!cfg) {
  $("name").textContent = "Not set up";
  $("hint").textContent = "Open the setup link from the organizer on this device.";
} else {
  $("name").textContent = cfg.name;
  $("event").textContent = cfg.event;
  render(cfg);
  keepAwake();
  document.addEventListener("visibilitychange", () => document.visibilityState === "visible" && keepAwake());
  $("reset").onclick = () => { if (confirm("Remove this café's key from this device?")) { localStorage.removeItem(KEY); location.reload(); } };
}
