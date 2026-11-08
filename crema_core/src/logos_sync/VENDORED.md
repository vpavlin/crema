Vendored from github.com/vpavlin/loam-sync @ fd13c02 (basecamp/logos_sync/): byte-for-byte copies of the
revision packages/loam-sync pins (the phone runs the TS side of the same commit). Do not edit here;
change upstream and re-vendor:
  cp packages/loam-sync/basecamp/logos_sync/*.hpp crema_core/src/logos_sync/
Vendored (not included from packages/) because the nix build sees crema_core/ only.
