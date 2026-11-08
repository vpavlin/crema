/**
 * withGeoFix — one location fix per stamp from Android's LocationManager (native/geofix), so the
 * app needs no Google Play services (expo-location pulls in play-services-location).
 *
 * Copies the Kotlin into android/, registers the package (hand-written, not autolinkable) and
 * declares the location permissions. Re-applied on every `expo prebuild`.
 */
const { withDangerousMod, withMainApplication, withAndroidManifest } = require("@expo/config-plugins");
const fs = require("fs");
const path = require("path");

const PACKAGE_IMPORT = "xyz.vpavlin.crema.geo.GeoFixPackage";
const PERMS = ["android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION"];

function copyDir(src, dst) {
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    const s = path.join(src, e.name), d = path.join(dst, e.name);
    if (e.isDirectory()) copyDir(s, d); else fs.copyFileSync(s, d);
  }
}

const withFiles = (config) =>
  withDangerousMod(config, ["android", async (cfg) => {
    copyDir(path.join(cfg.modRequest.projectRoot, "native", "geofix", "android", "java"),
            path.join(cfg.modRequest.platformProjectRoot, "app/src/main/java"));
    return cfg;
  }]);

const withPackage = (config) =>
  withMainApplication(config, (cfg) => {
    let src = cfg.modResults.contents;
    if (!src.includes(PACKAGE_IMPORT)) {
      const before = src;
      src = src.replace(/PackageList\(this\)\.packages\.apply\s*\{/,
        `PackageList(this).packages.apply {\n          // one location fix per stamp (LocationManager, no Google services)\n          add(${PACKAGE_IMPORT}())`);
      if (src === before) throw new Error("withGeoFix: MainApplication package list not found");
      cfg.modResults.contents = src;
    }
    return cfg;
  });

const withPerms = (config) =>
  withAndroidManifest(config, (cfg) => {
    const m = cfg.modResults.manifest;
    m["uses-permission"] = m["uses-permission"] || [];
    for (const p of PERMS)
      if (!m["uses-permission"].some((u) => u.$["android:name"] === p)) m["uses-permission"].push({ $: { "android:name": p } });
    // a phone without GPS can still play with network location
    m["uses-feature"] = m["uses-feature"] || [];
    if (!m["uses-feature"].some((u) => u.$["android:name"] === "android.hardware.location.gps"))
      m["uses-feature"].push({ $: { "android:name": "android.hardware.location.gps", "android:required": "false" } });
    return cfg;
  });

module.exports = (config) => withPerms(withPackage(withFiles(config)));
