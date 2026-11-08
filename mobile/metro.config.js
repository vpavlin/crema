// Metro: bundle the Crema SDK (../packages: protocol, engine, and the loam-sync submodule) so the
// phone runs the exact code the tests and the Basecamp core are checked against.
//  - watchFolders: the SDK root (not the repo root - that confuses the crawler)
//  - nodeModulesPaths: resolve their bare imports (@noble/*) from mobile/node_modules
const { getDefaultConfig } = require("expo/metro-config");
const path = require("path");
const projectRoot = __dirname;
const config = getDefaultConfig(projectRoot);
config.watchFolders = [path.resolve(projectRoot, "..", "packages")];
config.resolver.sourceExts = Array.from(new Set([...config.resolver.sourceExts, "mjs"]));
config.resolver.nodeModulesPaths = [path.join(projectRoot, "node_modules")];
config.resolver.useWatchman = false;
module.exports = config;
