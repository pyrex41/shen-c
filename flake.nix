{
  description = "Shen-C development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { nixpkgs, ... }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      each = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      tools = pkgs: [
        pkgs.clang
        pkgs.cmake
        pkgs.ninja
        pkgs.gnumake
        pkgs.pkg-config
        pkgs.boehmgc
        pkgs.git
      ];
      srcFor = pkgs: pkgs.lib.cleanSourceWith {
        src = ./.;
        filter = path: type:
          let base = baseNameOf path;
          in !(builtins.elem base [
            "bin" "obj" "build" "build-ci" "evidence" ".git"
          ]);
      };
      rejectHomebrew = ''
        pkg-config --modversion bdw-gc
        cflags=$(pkg-config --cflags --libs bdw-gc)
        echo "bdw-gc flags: $cflags"
        case "$cflags" in
          *Homebrew*|*opt/homebrew*)
            echo "Homebrew bdw-gc is forbidden" >&2
            exit 1
            ;;
        esac
      '';
      mkShenC = pkgs: cc: pkgs.stdenv.mkDerivation {
        pname = "shen-c";
        version = "0.2.3";
        src = srcFor pkgs;
        nativeBuildInputs = [
          pkgs.pkg-config
          pkgs.cmake
          pkgs.ninja
          pkgs.gnumake
        ] ++ pkgs.lib.optionals (cc == "clang") [ pkgs.clang ]
          ++ pkgs.lib.optionals (cc == "gcc") [ pkgs.gcc ];
        buildInputs = [ pkgs.boehmgc ];
        CC = cc;
        dontConfigure = true;
        enableParallelBuilding = true;
        buildPhase = ''
          runHook preBuild
          ${rejectHomebrew}
          make -j$NIX_BUILD_CORES CC=$CC
          runHook postBuild
        '';
        doCheck = true;
        checkPhase = ''
          runHook preCheck
          make CC=$CC test
          ./bin/shen-c --version
          runHook postCheck
        '';
        installPhase = ''
          runHook preInstall
          mkdir -p $out/bin $out/libexec $out/share/shen-c
          cp bin/shen-c $out/libexec/shen-c
          cp -R shen $out/share/shen-c/shen
          cat > $out/bin/shen-c <<EOF
          #!/bin/sh
          export SHEN_C_HOME="''${SHEN_C_HOME:-$out/share/shen-c}"
          exec $out/libexec/shen-c "\$@"
          EOF
          chmod +x $out/bin/shen-c
          runHook postInstall
        '';
      };
    in {
      packages = each (pkgs: rec {
        toolchain = pkgs.buildEnv {
          name = "shen-c-toolchain";
          paths = tools pkgs;
        };
        shen-c = mkShenC pkgs "clang";
        default = shen-c;
      });

      checks = each (pkgs: {
        make-clang = mkShenC pkgs "clang";
        cmake-ninja = pkgs.stdenv.mkDerivation {
          pname = "shen-c-cmake";
          version = "0.2.3";
          src = srcFor pkgs;
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.cmake
            pkgs.ninja
            pkgs.clang
          ];
          buildInputs = [ pkgs.boehmgc ];
          cmakeFlags = [
            "-DCMAKE_C_COMPILER=clang"
            "-DCMAKE_BUILD_TYPE=Release"
          ];
          preConfigure = rejectHomebrew;
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ../bin/test_foundation
            ../bin/test_abi
            ../bin/shen-c --version
            runHook postCheck
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp ../bin/shen-c $out/bin/shen-c
          '';
        };
      } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
        make-gcc = mkShenC pkgs "gcc";
      });

      devShells = each (pkgs: {
        default = pkgs.mkShell {
          packages = tools pkgs
            ++ nixpkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.gcc ];
          nativeBuildInputs = [ pkgs.pkg-config pkgs.cmake pkgs.ninja pkgs.gnumake ];
          buildInputs = [ pkgs.boehmgc ];
          shellHook = ''
            echo "shen-c dev shell (C17, bdw-gc via pkg-config)"
            echo "  cc:        ''${CC:-clang}"
            echo "  cmake:     $(cmake --version | head -n1)"
            echo "  ninja:     $(ninja --version)"
            echo "  pkg-config bdw-gc: $(pkg-config --modversion bdw-gc)"
          '';
        };
      });
    };
}
