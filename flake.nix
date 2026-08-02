{
  description = "Coax Native — Windows live-TV player built on libmpv";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      cross = pkgs.pkgsCross.mingwW64;
    in
    {
      # Cross-compiling shell: Linux tools on PATH, Windows libraries on the
      # link path. `cross.mkShell` wires the mingw-w64 compiler wrapper up with
      # the right NIX_LDFLAGS, which a bare `nix shell` does not do.
      devShells.${system}.default = cross.mkShell {
        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          p7zip
          curl
        ];

        # mcfgthreads is the threading runtime nixpkgs' mingw gcc links against;
        # without it on the link path every binary fails with -lmcfgthread.
        buildInputs = [
          cross.windows.mingw_w64
          cross.windows.mcfgthreads
        ];
      };
    };
}
