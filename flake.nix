{
  description = "Shen-C independent reference-counted compiler and runtime";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { nixpkgs, ... }:
    let
      systems = [
        "aarch64-darwin" "x86_64-darwin"
        "aarch64-linux" "x86_64-linux"
      ];
      each = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      tools = pkgs: [
        pkgs.clang pkgs.cmake pkgs.ninja pkgs.gnumake pkgs.git pkgs.python3
      ];
      srcFor = pkgs: pkgs.lib.cleanSourceWith {
        src = ./.;
        filter = path: type:
          let base = baseNameOf path;
          in !(builtins.elem base [
            "bin" "obj" "build" "build-ci" "build-rc" "evidence" ".git"
          ]);
      };
      # Installed compilation needs a C toolchain, make, the complete KL kernel,
      # and the independent runtime sources. Dynamic execution also needs a compiler.
      installArtifacts = pkgs: binaries: sources: ''
        runHook preInstall
        mkdir -p "$out/bin" "$out/libexec/shen-c" \
          "$out/share/shen-c/src" "$out/share/shen-c/scripts"
        cp ${binaries}/shen-rc "$out/bin/shen-rc"
        cp ${binaries}/shen-rc "$out/bin/shen-c"
        cp ${binaries}/rc-build "$out/libexec/shen-c/rc-build"
        cp -R ${sources}/src/rc "$out/share/shen-c/src/rc"
        cp -R ${sources}/shen "$out/share/shen-c/shen"
        cp ${sources}/scripts/embed-tier-config.py "$out/share/shen-c/scripts/"
        cp ${sources}/scripts/release_manifest.py "$out/share/shen-c/scripts/"
        cp ${sources}/scripts/compile-shen-rc.sh "$out/bin/compile-shen-rc.sh"
        for executable in shen-rc shen-c; do
          wrapProgram "$out/bin/$executable" \
            --set-default SHEN_RC_NATIVE_CC ${pkgs.clang}/bin/clang \
            --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.clang ]}
        done
        makeWrapper "$out/libexec/shen-c/rc-build" "$out/bin/rc-build" \
          --set-default SHEN_C_HOME "$out/share/shen-c" \
          --set-default CC clang \
          --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.python3 pkgs.clang pkgs.gnumake ]}
        wrapProgram "$out/bin/compile-shen-rc.sh" \
          --set-default SHEN_C_HOME "$out/share/shen-c" \
          --set-default CC clang \
          --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.python3 pkgs.clang pkgs.gnumake ]}
        runHook postInstall
      '';
      installedSmoke = ''
        runHook preInstallCheck
        unset SHEN_C_HOME SHEN_RC_HOME SHEN_RC_CLI RC_BUILDER
        "$out/bin/shen-c" --version
        test "$("$out/bin/shen-rc" eval -e '(+ 20 22)')" = 42
        python3 -c 'import subprocess,sys; p=subprocess.run([sys.argv[1],"repl"],input=b"[314159 271828]",capture_output=True,timeout=120); assert p.returncode==0 and b"[314159 271828]" in p.stdout and not p.stderr' "$out/bin/shen-rc"
        rc_check_dir=$(mktemp -d)
        printf '(define nix-answer X -> (+ X 1))\n(nix-answer 41)\n' > "$rc_check_dir/input.shen"
        "$out/bin/compile-shen-rc.sh" "$rc_check_dir/input.shen" "$rc_check_dir/native"
        SHEN_RC_PRINT=1 "$rc_check_dir/native/app" > "$rc_check_dir/result"
        test "$(cat "$rc_check_dir/result")" = 42
        runHook postInstallCheck
      '';
      mkShenC = pkgs: cc: pkgs.stdenv.mkDerivation {
        pname = "shen-c";
        version = "0.3.0";
        src = srcFor pkgs;
        nativeBuildInputs = [ pkgs.gnumake pkgs.python3 pkgs.makeWrapper ]
          ++ pkgs.lib.optionals (cc == "clang") [ pkgs.clang ]
          ++ pkgs.lib.optionals (cc == "gcc") [ pkgs.gcc ];
        CC = cc;
        PKG_CONFIG = "/nonexistent/shen-rc-pkg-config";
        dontConfigure = true;
        enableParallelBuilding = true;
        buildPhase = ''
          runHook preBuild
          make -j$NIX_BUILD_CORES CC="$CC"
          runHook postBuild
        '';
        doCheck = true;
        checkPhase = ''
          runHook preCheck
          make CC="$CC" test
          test/rc/test_reader_capture.sh ./bin/shen-rc
          test "$(./bin/shen-c eval -e '(+ 20 22)')" = 42
          runHook postCheck
        '';
        installPhase = installArtifacts pkgs "bin" ".";
        doInstallCheck = true;
        installCheckPhase = installedSmoke;
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
          version = "0.3.0";
          src = srcFor pkgs;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.clang pkgs.gnumake pkgs.python3 pkgs.makeWrapper ];
          PKG_CONFIG = "/nonexistent/shen-rc-pkg-config";
          cmakeFlags = [
            "-DCMAKE_C_COMPILER=clang"
            "-DCMAKE_BUILD_TYPE=Release"
          ];
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ctest --output-on-failure
            ../test/rc/test_reader_capture.sh ./shen-rc
            test "$(./shen-rc eval -e '(+ 20 22)')" = 42
            runHook postCheck
          '';
          installPhase = installArtifacts pkgs "." "..";
          doInstallCheck = true;
          installCheckPhase = installedSmoke;
        };
      } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
        make-gcc = mkShenC pkgs "gcc";
      });

      devShells = each (pkgs: {
        default = pkgs.mkShell {
          packages = tools pkgs
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [ pkgs.gcc ];
          shellHook = ''
            echo "shen-c dev shell (C17, independent RC compiler and runtime)"
            echo "  make; make test; make certify"
          '';
        };

      });
    };
}
