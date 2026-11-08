{
  description = "Crema coffee passport CORE module (organizer studio + leaderboard, synced over loam_core); headless AND the desktop view's backend (Basecamp 0.3).";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/3a64943fe6827bab4c4686fb35c37d24d5df0710";   # tag 0.3.3 (pairs with Basecamp 0.3.2)
    loam_core.url = "github:vpavlin/loam-basecamp/5db9069d7b953b5876210576393b1dc9ffc19fdf?dir=core";
    # the event map lives on Logos Storage; the host (Basecamp 0.3.2 / logosctl) owns the node at runtime
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.2";
  };

  # mkLogosModule: a headless core; the plugin glue is generated from src/crema_core_impl.h
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
