# native/logosdelivery — the JNI bridge (supplied, not built here)

This directory holds the prebuilt Logos delivery node + the hand-written JNI
bridge that the Expo config plugin (`plugins/withLogosDelivery.js`) re-copies into
`android/` on every `expo prebuild` (CNG wipes `android/`, so nothing hand-added
there survives). It is **arm64-only** — there is no public x86_64
`liblogosdelivery.so`, so an x86_64 emulator has no node. **Test sync on a real
arm64 phone.**

Expected layout (populate from the Logos delivery build — mirrors KYM's
`mobile/native/logosdelivery/`):

```
arm64-v8a/
  libc++_shared.so        # load 1st: liblogosdelivery references __gxx_personality_v0
  librln.so               # load 2nd
  liblogosdelivery.so     # load 3rd: the node (arm64 only)
  liblogosjni.so          # load 4th: our JNI shim, built from jni/
jni/
  logos_messaging_ffi.c   # wraps the stable high-level API + a few kernel symbols
  Android.mk              # liblogosdelivery = PREBUILT_SHARED_LIBRARY; shim = BUILD_SHARED_LIBRARY
android/java/co/logos/qaku/
  LogosMessagingModule.kt # RN NativeModule "LogosMessaging": setup/subscribeContentTopic/
                          #   channelCreate/channelSend/storeQuery/getNodeInfo; lazy lib load
                          #   in load order behind a @Synchronized @Volatile flag; one JS event
                          #   "logosMessage" for ALL receives (relay + channel)
  LogosMessagingPackage.kt
```

## The non-obvious rules (see logos-mobile-app + logos-reliable-channels)

- **JNI wraps the stable high-level API:** `logosdelivery_create_node/start_node/
  stop_node/destroy/subscribe/send/get_node_info/channel_create/channel_send/
  channel_close/set_event_callback` + kernel `waku_store_query`. `waku_stop`/
  `waku_destroy` are **not exported** — call `logosdelivery_stop_node`/`_destroy`.
- **Cross-thread JVM attach:** the node calls the event callback from its own
  worker threads. Cache a global ref + method id in `JNI_OnLoad`; `AttachCurrentThread`
  once per thread with a `pthread_key` detach. **Attach unconditionally — never
  inside `assert()`** (NDEBUG strips it → NULL `env` → release-only SIGSEGV).
- **Relay config only** (`{mode:"Core",preset:"logos.dev",relay:true,entryNodes}`)
  — light-client fields make `waku_new` reject the config.
- **Content-topic subscribe** (auto-shards) — a raw pubsub subscribe lands on a
  non-existent shard and receives nothing.
- **Android DNS deadlock:** patch `online_monitor.nim` to assume-online at 0
  peers, and bake it into the arm64 `.so` (the node's raw-UDP DNS check fails on
  Android's sandboxed stack → node thinks it's offline forever).

The JS side of channels (subscribe-before-channelCreate, double-base64, counters,
store-pull) is already implemented in `src/lib/delivery.ts`.
