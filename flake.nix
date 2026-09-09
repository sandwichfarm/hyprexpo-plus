{
  inputs = {
    hyprland = {
      type = "github";
      owner = "hyprwm";
      repo = "Hyprland";
      rev = "34eb03bd8da01024596c367fba66485a8c9b8ca7";
    };

    nixpkgs.follows = "hyprland/nixpkgs";
  };

  outputs = {
    self,
    nixpkgs,
    hyprland,
    ...
  }: let
    withPkgsFor = fn:
      nixpkgs.lib.genAttrs (builtins.attrNames hyprland.packages) (system:
        fn system (import nixpkgs {
          inherit system;
          overlays = [
            hyprland.overlays.hyprland-packages
            self.overlays.default
          ];
        }));
  in {
    packages = withPkgsFor (system: pkgs: rec {
      inherit (pkgs.hyprlandPlugins) hyprexpo;
      inherit (pkgs) hyprland;

      default = hyprexpo;
    });

    overlays = {
      default = self.overlays.hyprexpo;

      hyprexpo = final: prev:
        (nixpkgs.lib.optionalAttrs (prev ? glaze-hyprland) {
          # Released Hyprland 0.56.1/2 require glaze 7.x; the 0.56.2 lock has glaze 8.
          glaze-hyprland = prev.glaze-hyprland.overrideAttrs (old:
            nixpkgs.lib.optionalAttrs (
              prev ? hyprland
              && nixpkgs.lib.versionAtLeast prev.hyprland.version "0.56.1"
              && nixpkgs.lib.versionOlder prev.hyprland.version "0.56.3"
              && nixpkgs.lib.versionAtLeast old.version "8"
            ) {
              version = "7.2.0";
              src = final.fetchFromGitHub {
                owner = "stephenberry";
                repo = "glaze";
                tag = "v7.2.0";
                hash = "sha256-f3NVRi3SXKo42hn0WCw7JsOK3EkdOVJIcuzhPorKjFY=";
              };
            });
        })
        // {
          hyprlandPlugins =
            (prev.hyprlandPlugins or {})
            // {
              hyprexpo = final.callPackage ./default.nix {};
            };
        };
    };

    devShells = withPkgsFor (system: pkgs: {
      default = pkgs.mkShell.override {inherit (pkgs.hyprland) stdenv;} {
        shellHook = ''
          meson setup build --reconfigure
          sed -e 's/c++23/c++2b/g' ./build/compile_commands.json > ./compile_commands.json
        '';
        name = "hyprexpo-shell";
        nativeBuildInputs = with pkgs; [meson pkg-config ninja];
        buildInputs = [pkgs.hyprland];
        inputsFrom = [
          pkgs.hyprland
          pkgs.hyprlandPlugins.hyprexpo
        ];
      };
    });

    formatter = withPkgsFor (_: pkgs: pkgs.alejandra);
  };
}
