{
  description = "Crema Basecamp ui_qml module (pure QML view over crema_core), Basecamp 0.3";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/3a64943fe6827bab4c4686fb35c37d24d5df0710";   # tag 0.3.3 (pairs with Basecamp 0.3.2)
    crema_core.url = "path:../crema_core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
