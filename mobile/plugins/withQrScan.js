/**
 * withQrScan — QR scanning via ZXing (com.journeyapps:zxing-android-embedded, Apache-2.0).
 *
 * Replaces expo-camera: on Android its barcode scanner is Google ML Kit +
 * play-services-code-scanner, which bundle Play services and Firebase components. ZXing
 * decodes on the device with no Google dependency (the scanner most F-Droid apps use).
 *
 * Copies native/qrscan into android/, registers the package, adds the gradle dependency and
 * keeps ZXing's capture screen in portrait (the app is portrait-only). The CAMERA permission
 * comes from the library's manifest.
 */
const { withDangerousMod, withMainApplication, withAppBuildGradle, withAndroidManifest } = require("@expo/config-plugins");
const fs = require("fs");
const path = require("path");

const PACKAGE_IMPORT = "xyz.vpavlin.crema.qrscan.QrScanPackage";
const DEP = `implementation("com.journeyapps:zxing-android-embedded:4.3.0")`;

function copyDir(src, dst) {
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    const s = path.join(src, e.name), d = path.join(dst, e.name);
    if (e.isDirectory()) copyDir(s, d); else fs.copyFileSync(s, d);
  }
}

const withFiles = (config) =>
  withDangerousMod(config, ["android", async (cfg) => {
    copyDir(path.join(cfg.modRequest.projectRoot, "native", "qrscan", "android", "java"),
            path.join(cfg.modRequest.platformProjectRoot, "app/src/main/java"));
    return cfg;
  }]);

const withPackage = (config) =>
  withMainApplication(config, (cfg) => {
    let src = cfg.modResults.contents;
    if (!src.includes(PACKAGE_IMPORT)) {
      const before = src;
      src = src.replace(/PackageList\(this\)\.packages\.apply\s*\{/,
        `PackageList(this).packages.apply {\n          // QR scanning (ZXing, no Google services) — manual package, not autolinkable.\n          add(${PACKAGE_IMPORT}())`);
      if (src === before) throw new Error("withQrScan: MainApplication package list not found");
      cfg.modResults.contents = src;
    }
    return cfg;
  });

const withDep = (config) =>
  withAppBuildGradle(config, (cfg) => {
    if (!cfg.modResults.contents.includes("zxing-android-embedded")) {
      const before = cfg.modResults.contents;
      cfg.modResults.contents = before.replace(/implementation\("com\.facebook\.react:react-android"\)/,
        `implementation("com.facebook.react:react-android")\n\n    // QR scanning without Google services\n    ${DEP}`);
      if (cfg.modResults.contents === before) throw new Error("withQrScan: app/build.gradle dependency anchor not found");
    }
    return cfg;
  });

const withPortraitCapture = (config) =>
  withAndroidManifest(config, (cfg) => {
    const m = cfg.modResults.manifest;
    m.$["xmlns:tools"] = m.$["xmlns:tools"] || "http://schemas.android.com/tools";
    const app = m.application[0];
    app.activity = app.activity || [];
    const name = "com.journeyapps.barcodescanner.CaptureActivity";
    if (!app.activity.some((a) => a.$["android:name"] === name))
      app.activity.push({ $: { "android:name": name, "android:screenOrientation": "portrait", "tools:replace": "screenOrientation" } });
    return cfg;
  });

module.exports = (config) => withPortraitCapture(withDep(withPackage(withFiles(config))));
