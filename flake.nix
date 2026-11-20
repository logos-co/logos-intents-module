{
  description = "Logos Intents Module - routes user-mediated requests between modules";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        tests = {
          dir = ./tests;
        };
      };

      # contracts/intents.lidl and contracts/intents_presenter.lidl split the
      # module's generated contract between apps and frontends. Together they
      # must list exactly its methods and events, or a binding fails at runtime.
      contractsCheck = system:
        let pkgs = logos-module-builder.inputs.nixpkgs.legacyPackages.${system};
        in pkgs.runCommand "intents-contracts-match" { } ''
          members() { grep -hE '^\s*(method|event) ' "$@" | sed -E 's/^\s+//' | sort; }
          members ${module.packages.${system}.lidl}/intents_module.lidl > generated
          members ${./contracts/intents.lidl} ${./contracts/intents_presenter.lidl} > split
          if ! diff -u generated split; then
            echo "contracts/*.lidl no longer match the generated intents_module contract" >&2
            exit 1
          fi
          touch $out
        '';
    in module // {
      checks = builtins.mapAttrs
        (system: checks: checks // { contracts = contractsCheck system; })
        (module.checks or {});
    };
}
