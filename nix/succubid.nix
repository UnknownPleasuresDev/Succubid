{ inputs, withSystem, ... }: {

  systems = [ "x86_64-linux" "aarch64-linux" "armv6l-linux" "armv7l-linux" "x86_64-darwin" "aarch64-darwin" ];
  perSystem = { pkgs, self', ... }: {
    packages.default = self'.packages.succubid;

    packages.succubid-gui = pkgs.writeShellScriptBin "succubid-gui" ''
      exec ${pkgs.lib.getExe pkgs.kitty} \
        ${pkgs.lib.getExe self'.packages.succubid} \
        "$@"
    '';

    packages.succubid-gui-unwrapped = pkgs.writeShellScriptBin "succubid-gui-unwrapped" ''
      exec ${pkgs.lib.getExe pkgs.kitty} \
        ${pkgs.lib.getBin self'.packages.succubid}/bin/unwrapped-succubid \
        "$@"
    '';

    packages.mpv = pkgs.symlinkJoin {
      name = "succubid-mpv";
      paths = [ pkgs.mpv ];
      nativeBuildInputs = [ pkgs.makeWrapper ];
      postBuild = ''
        wrapProgram ${pkgs.lib.getExe pkgs.mpv} --add-flags "--input-ipc-server=''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/mpv.sock"
      '';
    };

    packages.succubid = pkgs.stdenv.mkDerivation (finalAttrs:
      let
        install-systemd = pkgs.writeShellApplication {
            name = "install-succubid";
            text = ''
              SERVICE_DIR="$HOME/.config/systemd/user"
              OVERRIDE_DIR="$SERVICE_DIR/succubid.service.d"

              mkdir -p "$SERVICE_DIR" "$OVERRIDE_DIR"

              cat > "$SERVICE_DIR/succubid.service" <<EOF
              [Unit]
              Description=Succubid
              After=default.target

              [Service]
              Type=simple
              ExecStart={{pkg}}
              Restart=on-failure
              RestartSec=5

              [Install]
              WantedBy=default.target
              EOF

              printf "Enter your Handy connection key: "
              read -r CONNECTION_KEY

              cat > "$OVERRIDE_DIR/10-env.conf" <<EOF
              [Service]
              Environment=SUCCUBID_HANDY_CONNECTION_KEY=$CONNECTION_KEY
              EOF

              systemctl --user daemon-reload
              systemctl --user enable --now succubid.service

              echo
              echo "Succubid has been installed as a user service."
            '';
          };
      in
      {
        pname = "succubid";
        version = "0-unstable-2026-08-05";

        src = ../.;
        nativeBuildInputs = [
          pkgs.curl
          pkgs.makeWrapper
          pkgs.xxd
          pkgs.httplib
          pkgs.nlohmann_json
        ];
        configurePhase = "mkdir -p $out/bin/";
        installPhase = ''
          cp ./succubid $out/bin/unwrapped-succubid
          install -Dm644 LICENSE $out/share/licenses/succubid/LICENSE
          install -Dm644 succubid.1 $out/share/man/man1/succubid.1
          makeWrapper $out/bin/unwrapped-succubid $out/bin/succubid --add-flags "-g" --add-flags "-s" --add-flags "''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/mpv.sock"
          cat ${pkgs.lib.getExe install-systemd} | sed  "s,{{pkg}},$out/bin/succubid," > $out/bin/install-succubid; chmod +x $out/bin/install-succubid
        '';

        meta = {
          description = "A Unix Daemon for syncing NSFW videos with The Handy";
          homepage = "https://github.com/UnknownPleasuresDev/Succubid";
          license = pkgs.lib.licenses.agpl3Only;
          maintainers = [ "UnknownPleasuresDev" ];
          mainProgram = "succubid";
          platforms = pkgs.lib.platforms.all;
        };
      });
    };

  flake.homeModules.default = { config, lib, pkgs, ... }:
    let
      cfg = config.services.succubid;
      succubid = withSystem pkgs.stdenv.hostPlatform.system (
        { self', ... }: self'.packages.succubid
      );
      succubid-gui-unwrapped = withSystem pkgs.stdenv.hostPlatform.system (
        { self', ... }: self'.packages.succubid-gui-unwrapped
      );
    in {
      options.services.succubid = {
        enable = lib.mkEnableOption "succubid service";

        gui = lib.mkOption {
          type = lib.types.bool;
          default = false;
          defaultText = "false";
          description = "Toggle the MPV funscript switcher (triggers when there's multiple versions)";
        };

        connectionKey = lib.mkOption {
          type = lib.types.str;
          description = "The Handy connection key.";
        };

        mpvSocket = lib.mkOption {
          type = lib.types.path;
          default = "/run/user/$(id -u)/mpv.sock";
          defaultText = "/run/user/$(id -u)/mpv.sock";
          description = "Path to the MPV IPC socket.";
        };
      };

      config = {
        home.packages = [ succubid succubid-gui-unwrapped ];

        home.sessionVariables = {
          SUCCUBID_HANDY_CONNECTION_KEY = "${cfg.connectionKey}";
          SUCCUBID_MPV_SOCKET_PATH = "${cfg.mpvSocket}";
          SUCCUBID_USE_GUI = (toString cfg.gui);
        };

        xdg.desktopEntries.succubid-gui = {
          name = "succubid-gui";
          comment = "Run succubid in a window";
          exec = "${pkgs.lib.getExe succubid-gui-unwrapped}";
          terminal = false;
          type = "Application";
        };

        programs.mpv.config.input-ipc-server = toString cfg.mpvSocket;

        systemd.user.services.succubid = if cfg.enable then {
          Unit = {
            Description = "Succubid";
            After = [ "default.target" ];
          };
          Service = {
            Type = "simple";
            ExecStart = "${lib.getBin succubid}/bin/unwrapped-succubid -k ${lib.escapeShellArg cfg.connectionKey} -s ${lib.escapeShellArg (toString cfg.mpvSocket)}";
            Restart = "on-failure";
            RestartSec = 5;
          };
          Install.WantedBy = [ "default.target" ];
        } else {} ;
      };
    };

}
