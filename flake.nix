{
  description = "simple llama daemon";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "allam";
          version = "0.5.0";
          src = self;
          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ pkgs.httplib pkgs.nlohmann_json ];
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = [ pkgs.clang-tools pkgs.gdb ];
        };
      });
    };
}
