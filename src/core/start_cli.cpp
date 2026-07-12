// Copyright (C) 2026 Abdul, Hashim.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// carla-studio-cli_start — headless CARLA simulator launcher
//
// Usage: carla-studio start [--map <MapName>] [--port <rpc-port>] [--directory <carla-root>]
//                          [--res WxH] [--quality low|medium|high|ultra]
//                          [--wait-seconds <n>] [--viewport]
//                          [--engine <unreal-engine-root>]
//                          [--remote-host <host> --remote-user <user>
//                           [--remote-pass <password>] [--remote-bind-address <ip>]]

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <QMap>
#include <algorithm>

namespace {

QTextStream &out() { static QTextStream s(stdout); return s; }
QTextStream &err() { static QTextStream s(stderr); return s; }

static void log(const QString &line) { out() << "[start] " << line << "\n"; out().flush(); }
static void logErr(const QString &line) { err() << "[start] ERROR: " << line << "\n"; err().flush(); }

static QString shell_escape(const QString &s) {
    QString out = s;
    out.replace("'", "'\\''");
    return "'" + out + "'";
}

static QString pidFile() {
    const QString cfg = QDir::homePath() + "/.config/carla-studio";
    QDir().mkpath(cfg);
    return cfg + "/carla-pids.txt";
}

static void writePid(qint64 pid) {
    QFile f(pidFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(&f) << pid << "\n";
    }
}

static QString findCarlaRoot(const QString &hint) {
    if (!hint.isEmpty() && QFileInfo(hint).isDir()) return hint;
    const QString env = QString::fromLocal8Bit(qgetenv("CARLA_ROOT")).trimmed();
    if (!env.isEmpty() && QFileInfo(env).isDir()) return env;
    return {};
}

static QString findShippingBinary(const QString &carla_root) {
    static const QStringList kBinaries = {
        // Prefer direct shipping executable inside CARLA 0.10 extracted layout.
        "CarlaUnreal/Binaries/Linux/CarlaUnreal-Linux-Shipping",
        "CarlaUnreal-Linux-Shipping",
        "CarlaUnreal.sh",
        "CarlaUE5.sh",
        "CarlaUE4.sh",
        "LinuxNoEditor/CarlaUE4.sh",
        // CARLA 0.10.0 source-tree dev build — last resort when no shipping
        // binary exists.  Launched with -RenderOffScreen so it runs headless.
        "Unreal/CarlaUnreal/Binaries/Linux/CarlaUnreal",
    };
    // Check flat layout first (0.9.x / most releases).
    for (const QString &rel : kBinaries) {
        const QString p = carla_root + "/" + rel;
        if (QFileInfo(p).isFile()) return p;
    }
    // Some releases (e.g. 0.10.0) extract into a single top-level subdirectory.
    QDirIterator it(carla_root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString sub = it.next();
        for (const QString &rel : kBinaries) {
            const QString p = sub + "/" + rel;
            if (QFileInfo(p).isFile()) return p;
        }
    }
    return {};
}

static QString findUproject(const QString &carla_root) {
    const QString direct = carla_root + "/Unreal/CarlaUnreal/CarlaUnreal.uproject";
    if (QFileInfo(direct).isFile()) return direct;

    QDirIterator it(carla_root,
                    QStringList() << "CarlaUnreal.uproject",
                    QDir::Files,
                    QDirIterator::Subdirectories);
    if (it.hasNext()) return it.next();
    return {};
}

static QString resolveUnrealEditorBinary(const QString &engineRootHint) {
    const QString hint = engineRootHint.trimmed();
    if (!hint.isEmpty()) {
        const QString p = hint + "/Engine/Binaries/Linux/UnrealEditor";
        if (QFileInfo(p).isFile()) return p;
    }

    const QString env = QString::fromLocal8Bit(qgetenv("CARLA_UNREAL_ENGINE_PATH")).trimmed();
    if (!env.isEmpty()) {
        const QString p = env + "/Engine/Binaries/Linux/UnrealEditor";
        if (QFileInfo(p).isFile()) return p;
    }

    // Search common install locations only via env vars; no hardcoded paths.
    // Users should set CARLA_UNREAL_ENGINE_PATH or UE5_ROOT.
    const QString ue5Root = QString::fromLocal8Bit(qgetenv("UE5_ROOT")).trimmed();
    if (!ue5Root.isEmpty()) {
        const QString p = ue5Root + "/Engine/Binaries/Linux/UnrealEditor";
        if (QFileInfo(p).isFile()) return p;
    }
    return {};
}

}

int carla_cli_start_main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    QString mapName;
    int rpc_port = 2000;
    QString directory;
    QString engineRoot;
    bool showProgress = false;
    int waitSeconds = -1;    // --wait-seconds <n>: override progress wait timeout
    QString remoteHost;
    QString remoteUser;
    QString remotePass;
    QString remoteBindAddress;
    int resX = 0, resY = 0;   // --res WxH
    QString quality;           // --quality low|medium|high|ultra

    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == "--progress") { showProgress = true; }
        else if (a == "--viewport") { /* no-op: viewport is always enabled */ }
        else if ((a == "--map" || a == "-m") && i + 1 < args.size())
            mapName = args[++i];
        else if ((a == "--port" || a == "-p") && i + 1 < args.size()) {
            bool ok = false;
            const int p = args[++i].toInt(&ok);
            if (!ok || p <= 0 || p > 65535) {
                logErr("Invalid --port value. Expected 1..65535.");
                return 2;
            }
            rpc_port = p;
        }
        else if (a == "--wait-seconds" && i + 1 < args.size()) {
            bool ok = false;
            const int s = args[++i].toInt(&ok);
            if (!ok || s <= 0) {
                logErr("Invalid --wait-seconds value. Expected a positive integer.");
                return 2;
            }
            waitSeconds = s;
        }
        else if ((a == "--directory" || a == "-d") && i + 1 < args.size())
            directory = args[++i];
        else if (a == "--engine" && i + 1 < args.size())
            engineRoot = args[++i];
        else if (a == "--remote-host" && i + 1 < args.size())
            remoteHost = args[++i];
        else if (a == "--remote-user" && i + 1 < args.size())
            remoteUser = args[++i];
        else if (a == "--remote-pass" && i + 1 < args.size())
            remotePass = args[++i];
        else if (a == "--remote-bind-address" && i + 1 < args.size())
            remoteBindAddress = args[++i];
        else if (a == "--res" && i + 1 < args.size()) {
            const QString v = args[++i].trimmed();
            int sep = static_cast<int>(v.indexOf('x'));
            if (sep < 0) sep = static_cast<int>(v.indexOf('X'));
            if (sep > 0 && sep < v.size() - 1) {
                bool ox = false, oy = false;
                resX = v.left(sep).toInt(&ox);
                resY = v.mid(sep + 1).toInt(&oy);
                if (!ox || !oy || resX <= 0 || resY <= 0) {
                    logErr("Invalid --res value. Expected WxH e.g. 3840x2160.");
                    return 2;
                }
            } else {
                logErr("Invalid --res value. Expected WxH e.g. 3840x2160.");
                return 2;
            }
        }
        else if (a == "--quality" && i + 1 < args.size()) {
            quality = args[++i].toLower();
            static const QStringList kQ = {"low","medium","high","ultra"};
            if (!kQ.contains(quality)) {
                logErr("Invalid --quality value. Expected: low, medium, high, ultra."); return 2;
            }
        }
    }

    const bool useRemote = !remoteHost.isEmpty();

    if (useRemote) {
        const QString remoteRoot = directory.trimmed();
        const QString remoteEngineRoot = engineRoot.trimmed();
        QStringList script;
        script << "set -e";
        script << "CARLA_ROOT_IN=" + shell_escape(remoteRoot);
        script << "ENGINE_ROOT_IN=" + shell_escape(remoteEngineRoot);
        script << "if [ -n \"$CARLA_ROOT_IN\" ] && [ ! -d \"$CARLA_ROOT_IN\" ]; then";
        script << "  echo \"[start] ERROR: remote directory does not exist: $CARLA_ROOT_IN\"";
        script << "  exit 1";
        script << "fi";
        // Explicit CLI/UI directory must override any remote profile CARLA_ROOT.
        script << "CARLA_ROOT=\"\"";
        script << "if [ -n \"$CARLA_ROOT_IN\" ]; then";
        script << "  CARLA_ROOT=\"$CARLA_ROOT_IN\"";
        script << "elif [ -n \"${CARLA_ROOT:-}\" ] && [ -d \"${CARLA_ROOT}\" ]; then";
        script << "  CARLA_ROOT=\"${CARLA_ROOT}\"";
        script << "fi";
        // Auto-detect common CARLA locations on remote hosts when no root was supplied.
        script << "if [ -z \"$CARLA_ROOT\" ]; then";
        script << "  for d in /simulators/carla-0-10-0 /simulators/carla/CARLA /simulators/carla /simulators/carla_src; do";
        script << "    [ -d \"$d\" ] || continue";
        script << "    if [ -f \"$d/CarlaUnreal/Binaries/Linux/CarlaUnreal-Linux-Shipping\" ] || [ -f \"$d/CarlaUnreal.sh\" ] || [ -f \"$d/CarlaUE5.sh\" ] || [ -f \"$d/CarlaUE4.sh\" ] || [ -f \"$d/Unreal/CarlaUnreal/CarlaUnreal.uproject\" ]; then";
        script << "      CARLA_ROOT=\"$d\"";
        script << "      break";
        script << "    fi";
        script << "  done";
        script << "fi";
        script << "if [ -z \"$CARLA_ROOT\" ]; then";
        script << "  echo \"[start] ERROR: set --directory or CARLA_ROOT on remote host (also tried common /simulators paths)\"";
        script << "  exit 1";
        script << "fi";
        script << "export CARLA_ROOT";
        // Ensure a display is available for UE5/Vulkan rendering.
        // In non-login SSH sessions $DISPLAY is usually unset; auto-detect from
        // /tmp/.X11-unix (picks :1, :0, etc.) and only fall back to :0 if none found.
        script << "if [ -z \"${DISPLAY:-}\" ]; then";
        script << "  _xsock=$(ls /tmp/.X11-unix/ 2>/dev/null | sed 's/^X/:/' | sort -V | head -1)";
        script << "  export DISPLAY=${_xsock:-:0}";
        script << "fi";
        script << "find_bin() {";
        script << "  root=\"$1\"";
        // Candidates ordered by preference: shipping first, dev binary last.
        script << "  for rel in CarlaUnreal/Binaries/Linux/CarlaUnreal-Linux-Shipping CarlaUnreal-Linux-Shipping CarlaUnreal.sh CarlaUE5.sh CarlaUE4.sh LinuxNoEditor/CarlaUE4.sh Unreal/CarlaUnreal/Binaries/Linux/CarlaUnreal; do";
        script << "    if [ -f \"$root/$rel\" ]; then echo \"$root/$rel\"; return 0; fi";
        script << "  done";
        script << "  for d in \"$root\"/*; do";
        script << "    [ -d \"$d\" ] || continue";
        script << "    for rel in CarlaUnreal/Binaries/Linux/CarlaUnreal-Linux-Shipping CarlaUnreal-Linux-Shipping CarlaUnreal.sh CarlaUE5.sh CarlaUE4.sh LinuxNoEditor/CarlaUE4.sh Unreal/CarlaUnreal/Binaries/Linux/CarlaUnreal; do";
        script << "      if [ -f \"$d/$rel\" ]; then echo \"$d/$rel\"; return 0; fi";
        script << "    done";
        script << "  done";
        script << "  return 1";
        script << "}";
        script << "BIN=$(find_bin \"$CARLA_ROOT\") || true";
        script << "UPROJECT=\"$CARLA_ROOT/Unreal/CarlaUnreal/CarlaUnreal.uproject\"";
        script << "ENGINE_EDITOR=\"\"";
        script << "ENGINE_BIN_DIR=\"\"";
        // Check explicit --engine arg and env vars first, then scan well-known paths.
        script << "if [ -n \"$ENGINE_ROOT_IN\" ] && [ -f \"$ENGINE_ROOT_IN/Engine/Binaries/Linux/UnrealEditor\" ]; then";
        script << "  ENGINE_EDITOR=\"$ENGINE_ROOT_IN/Engine/Binaries/Linux/UnrealEditor\"";
        script << "  ENGINE_BIN_DIR=\"$ENGINE_ROOT_IN/Engine/Binaries/Linux\"";
        script << "elif [ -n \"${CARLA_UNREAL_ENGINE_PATH:-}\" ] && [ -f \"$CARLA_UNREAL_ENGINE_PATH/Engine/Binaries/Linux/UnrealEditor\" ]; then";
        script << "  ENGINE_EDITOR=\"$CARLA_UNREAL_ENGINE_PATH/Engine/Binaries/Linux/UnrealEditor\"";
        script << "  ENGINE_BIN_DIR=\"$CARLA_UNREAL_ENGINE_PATH/Engine/Binaries/Linux\"";
        script << "elif [ -n \"${UE5_ROOT:-}\" ] && [ -f \"$UE5_ROOT/Engine/Binaries/Linux/UnrealEditor\" ]; then";
        script << "  ENGINE_EDITOR=\"$UE5_ROOT/Engine/Binaries/Linux/UnrealEditor\"";
        script << "  ENGINE_BIN_DIR=\"$UE5_ROOT/Engine/Binaries/Linux\"";
        script << "elif [ -n \"${UE4_ROOT:-}\" ] && [ -f \"$UE4_ROOT/Engine/Binaries/Linux/UnrealEditor\" ]; then";
        script << "  ENGINE_EDITOR=\"$UE4_ROOT/Engine/Binaries/Linux/UnrealEditor\"";
        script << "  ENGINE_BIN_DIR=\"$UE4_ROOT/Engine/Binaries/Linux\"";
        script << "else";
        // Scan common installation prefixes when no env var points to the engine.
        script << "  for _edir in /engines/ue5/UnrealEngine /engines/ue4/UnrealEngine /opt/UnrealEngine /usr/local/UnrealEngine; do";
        script << "    if [ -f \"$_edir/Engine/Binaries/Linux/UnrealEditor\" ]; then";
        script << "      ENGINE_EDITOR=\"$_edir/Engine/Binaries/Linux/UnrealEditor\"";
        script << "      ENGINE_BIN_DIR=\"$_edir/Engine/Binaries/Linux\"";
        script << "      break";
        script << "    fi";
        script << "  done";
        script << "fi";
        script << "mkdir -p \"$HOME/.config/carla-studio\"";
        script << "LOG=\"/tmp/carla_studio_launch.log\"";
        script << QString("RPC=%1").arg(rpc_port);
        script << "STREAM=$((RPC + 1))";
        // Default to Town01_Opt: it loads in ~24 s vs ~50 s for Town10HD_Opt.
        // The user can override with --map.
        const QString effectiveMap = mapName.isEmpty() ? QStringLiteral("Town01_Opt") : mapName;
        script << QString("MAP=%1").arg(shell_escape(effectiveMap));
        // Resolution shell var
        if (resX > 0 && resY > 0)
            script << QString("RES_ARGS=\"-ResX=%1 -ResY=%2\"").arg(resX).arg(resY);
        else
            script << "RES_ARGS=\"\"" ;
        // Quality ExecCmds shell var
        if (quality == "low")
            script << "EXEC_CMDS=\"r.DynamicGlobalIlluminationMethod 0,r.Nanite 0,sg.ShadowQuality 1,sg.TextureQuality 1,sg.EffectsQuality 1,sg.PostProcessQuality 1\"";
        else if (quality == "medium")
            script << "EXEC_CMDS=\"r.DynamicGlobalIlluminationMethod 0,r.Nanite 1,sg.ShadowQuality 3,sg.TextureQuality 3,sg.EffectsQuality 3,sg.PostProcessQuality 3\"";
        else if (quality == "high")
            script << "EXEC_CMDS=\"r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,r.Nanite 1,sg.ShadowQuality 4,sg.TextureQuality 4,sg.EffectsQuality 4,sg.PostProcessQuality 4\"";
        else if (quality == "ultra")
            script << "EXEC_CMDS=\"r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,r.Nanite 1,sg.ShadowQuality 5,sg.TextureQuality 5,sg.EffectsQuality 5,sg.PostProcessQuality 5,r.Shadow.MaxResolution 4096,r.Shadow.MaxCSMResolution 4096\"";
        else
            script << "EXEC_CMDS=\"\"";
        script << "if nc -z -w2 127.0.0.1 \"$RPC\" >/dev/null 2>&1; then";
        script << "  EXISTING_PID=$(ss -ltnp 2>/dev/null | grep -E \":${RPC}\\b\" | sed -n 's/.*pid=\\([0-9][0-9]*\\).*/\\1/p' | head -n1)";
        script << "  if [ -n \"$EXISTING_PID\" ]; then";
        script << "    echo \"$EXISTING_PID\" > \"$HOME/.config/carla-studio/carla-pids.txt\"";
        script << "    echo \"[start] CARLA already running on port ${RPC} (PID ${EXISTING_PID}); not launching duplicate.\"";
        script << "  else";
        script << "    echo \"[start] CARLA already running on port ${RPC}; not launching duplicate.\"";
        script << "  fi";
        script << "  exit 0";
        script << "fi";
        // Prefer UnrealEditor -game for source builds (dev binary needs cooked content).
        // Only use the raw binary when it looks like a shipping build (no uproject).
        script << "USE_EDITOR=0";
        script << "if [ -f \"$UPROJECT\" ] && [ -n \"$ENGINE_EDITOR\" ]; then";
        script << "  USE_EDITOR=1";
        script << "fi";
        script << "BIN_DIR=$([ -n \"$BIN\" ] && dirname \"$BIN\" || echo '')";
        // Locate EOS SDK and other UE5 engine libs that the dev binary links against.
        script << "EOS_LIB_DIR=\"\"";
        script << "for d in \"$BIN_DIR\" /engines/ue5/UnrealEngine/Engine/Binaries/Linux /opt/UnrealEngine/Engine/Binaries/Linux; do";
        script << "  if [ -f \"$d/libEOSSDK-Linux-Shipping.so\" ]; then EOS_LIB_DIR=\"$d\"; break; fi";
        script << "done";
        script << "if [ -z \"$EOS_LIB_DIR\" ]; then";
        script << "  EOS_LIB_DIR=$(find /engines /simulators /opt /usr/local -maxdepth 8 -name \"libEOSSDK-Linux-Shipping.so\" 2>/dev/null | head -1 | xargs -r dirname 2>/dev/null || true)";
        script << "fi";
        script << "if [ \"$USE_EDITOR\" -eq 0 ] && [ -n \"$BIN\" ]; then";
        script << "  LAUNCH_ARGS=(-carla-rpc-port=\"${RPC}\" -carla-streaming-port=\"${STREAM}\")";
        script << "  if [ -n \"$MAP\" ]; then LAUNCH_ARGS+=(-map=\"${MAP}\"); fi";
        script << "  if [ -n \"$RES_ARGS\" ]; then LAUNCH_ARGS+=($RES_ARGS); fi";
        script << "  if [ -n \"$EXEC_CMDS\" ]; then LAUNCH_ARGS+=(-ExecCmds \"$EXEC_CMDS\"); fi";
        script << "  cd \"$BIN_DIR\"";
        script << "  export LD_LIBRARY_PATH=\"$BIN_DIR${ENGINE_BIN_DIR:+:$ENGINE_BIN_DIR}${EOS_LIB_DIR:+:$EOS_LIB_DIR}:${LD_LIBRARY_PATH:-}\"";
        script << "  nohup \"$BIN\" \"${LAUNCH_ARGS[@]}\" > \"$LOG\" 2>&1 &";
        script << "elif [ -f \"$UPROJECT\" ] && [ -n \"$ENGINE_EDITOR\" ]; then";
        script << "  LAUNCH_ARGS=(\"$UPROJECT\" -game -vulkan -windowed -unattended -nosplash -nosound)";
        script << "  LAUNCH_ARGS+=(-carla-rpc-port=\"${RPC}\" -carla-streaming-port=\"${STREAM}\" -log -map=\"${MAP}\")";
        script << "  if [ -n \"$RES_ARGS\" ]; then LAUNCH_ARGS+=($RES_ARGS); fi";
        script << "  if [ -n \"$EXEC_CMDS\" ]; then LAUNCH_ARGS+=(-ExecCmds \"$EXEC_CMDS\"); fi";
        script << "  cd \"$(dirname \"$UPROJECT\")\"";
        script << "  if [ -n \"$ENGINE_BIN_DIR\" ]; then export LD_LIBRARY_PATH=\"$ENGINE_BIN_DIR:${LD_LIBRARY_PATH:-}\"; fi";
        script << "  nohup \"$ENGINE_EDITOR\" \"${LAUNCH_ARGS[@]}\" > \"$LOG\" 2>&1 &";
        if (waitSeconds > 0)
            script << QString("  WAIT_MAX=%1").arg(waitSeconds);
        else
            script << "  WAIT_MAX=300";
        script << "else";
        script << "  echo \"[start] ERROR: no CARLA launcher found in $CARLA_ROOT\"";
        script << "  echo \"[start] looked for shipping scripts and source uproject + UnrealEditor\"";
        script << "  exit 1";
        script << "fi";
        script << "PID=$!";
        script << "echo \"$PID\" > \"$HOME/.config/carla-studio/carla-pids.txt\"";
        script << "echo \"[start] CARLA started (PID ${PID})\"";
        script << "echo \"[start] RPC port: ${RPC}\"";
        if (showProgress) {
            script << "echo \"[start] Waiting for CARLA on port ${RPC}...\"";
            script << "START_T=$(date +%s)";
            script << "WARNED=0";
            script << "while true; do";
            script << "  if ! kill -0 \"$PID\" 2>/dev/null; then";
            script << "    ELAPSED=$(( $(date +%s) - START_T ))";
            script << "    echo \"\"";
            script << "    echo \"[start] CARLA process exited after ${ELAPSED}s without opening port ${RPC} — check ${LOG}\"";
            script << "    exit 1";
            script << "  fi";
            script << "  nc -z -w2 127.0.0.1 \"$RPC\" >/dev/null 2>&1 && break";
            script << "  ELAPSED=$(( $(date +%s) - START_T ))";
            script << "  printf '\\r[start] waiting... (%ss)   ' \"$ELAPSED\"";
            script << "  if [ \"$WARNED\" -eq 0 ] && [ \"$ELAPSED\" -ge 300 ]; then";
            script << "    echo \"\"";
            script << "    echo \"[start] still waiting after ${ELAPSED}s (process alive) — check ${LOG} for progress\"";
            script << "    WARNED=1";
            script << "  fi";
            script << "  sleep 3";
            script << "done";
            script << "ELAPSED=$(( $(date +%s) - START_T ))";
            script << "echo \"\"";
            script << "echo \"[start] CARLA is ready on port ${RPC} (took ${ELAPSED}s)\"";
        }

        const QString remoteScript = script.join("\n");

        QProcess p;
        QString program = "ssh";
        QStringList sshArgs;
        if (!remotePass.isEmpty()) {
            QProcess check;
            check.start("/bin/sh", {"-c", "command -v sshpass"});
            check.waitForFinished(2000);
            if (check.exitCode() != 0) {
                logErr("sshpass is required for --remote-pass but is not installed.");
                return 2;
            }
            program = "sshpass";
            sshArgs << "-p" << remotePass << "ssh";
        }
        if (!remoteBindAddress.trimmed().isEmpty())
            sshArgs << "-o" << QString("BindAddress=%1").arg(remoteBindAddress.trimmed());
        sshArgs << "-o" << "StrictHostKeyChecking=accept-new";
        sshArgs << "-o" << "ConnectTimeout=5";
        if (remoteUser.isEmpty())
            sshArgs << remoteHost;
        else
            sshArgs << QString("%1@%2").arg(remoteUser, remoteHost);
        sshArgs << remoteScript;

        log(QString("Launching CARLA on remote host %1%2")
            .arg(remoteHost, remoteUser.isEmpty() ? QString() : QString(" as %1").arg(remoteUser)));
        p.setProcessChannelMode(QProcess::ForwardedChannels);
        p.start(program, sshArgs);
        if (!p.waitForStarted(5000)) {
            logErr("Failed to start SSH process.");
            return 1;
        }
        const int remoteWaitMax = (waitSeconds > 0) ? waitSeconds : 300;
        const int sshWaitMs = showProgress ? (remoteWaitMax + 45) * 1000 : 25000;
        if (!p.waitForFinished(sshWaitMs)) {
            logErr("Timed out waiting for remote launcher script.");
            p.kill();
            p.waitForFinished(2000);
            return 1;
        }
        if (p.exitStatus() != QProcess::NormalExit) {
            logErr("Remote launcher SSH process terminated unexpectedly.");
            return 1;
        }
        return p.exitCode();
    }

    const QString carla_root = findCarlaRoot(directory);
    if (carla_root.isEmpty()) {
        logErr("Cannot find CARLA root. Set CARLA_ROOT or use --directory <path>.");
        return 1;
    }

    const QString binary = findShippingBinary(carla_root);
    const QString uproject = findUproject(carla_root);
    const QString editor = resolveUnrealEditorBinary(engineRoot);

    QString launchProgram;
    QStringList launchArgs;
    QString workingDir;

    const bool hasSourceTree = QFileInfo(carla_root + "/Unreal/CarlaUnreal/Source").isDir()
                            || QFileInfo(carla_root + "/Unreal/CarlaUE5/Source").isDir()
                            || QFileInfo(carla_root + "/Unreal/CarlaUE4/Source").isDir();
    const bool preferEditor = hasSourceTree && !uproject.isEmpty() && !editor.isEmpty();

    if (!binary.isEmpty() && !preferEditor) {
        launchProgram = binary;
        launchArgs << QString("-carla-rpc-port=%1").arg(rpc_port)
                   << QString("-carla-streaming-port=%1").arg(rpc_port + 1);
        // Default to Town01_Opt (~24 s boot) when no map specified.
        const QString effectiveMap = mapName.isEmpty() ? QStringLiteral("Town01_Opt") : mapName;
        launchArgs << QString("-map=%1").arg(effectiveMap);
        workingDir = QFileInfo(binary).absolutePath();
    } else if (!uproject.isEmpty() && !editor.isEmpty()) {
        launchProgram = editor;
        launchArgs << uproject << "-game" << "-vulkan";
        launchArgs << "-windowed";
        launchArgs << "-unattended"
                   << "-nosplash"
                   << "-nosound"
                   << QString("-carla-rpc-port=%1").arg(rpc_port)
                   << QString("-carla-streaming-port=%1").arg(rpc_port + 1)
                   << "-log";
        // Default to Town01_Opt (~24 s boot) when no map specified.
        const QString effectiveMap = mapName.isEmpty() ? QStringLiteral("Town01_Opt") : mapName;
        launchArgs << QString("-map=%1").arg(effectiveMap);
        workingDir = QFileInfo(uproject).absolutePath();
    } else {
        logErr("No CARLA launcher found in: " + carla_root);
        logErr("Expected shipping scripts or source-tree Unreal/CarlaUnreal/CarlaUnreal.uproject + UnrealEditor.");
        return 1;
    }

    // Append resolution args
    if (resX > 0 && resY > 0) {
        launchArgs << QString("-ResX=%1").arg(resX) << QString("-ResY=%1").arg(resY);
    }
    // Append quality ExecCmds
    if (!quality.isEmpty()) {
        static const QMap<QString,QString> kExec = {
            {"low",    "r.DynamicGlobalIlluminationMethod 0,r.Nanite 0,sg.ShadowQuality 1,sg.TextureQuality 1,sg.EffectsQuality 1,sg.PostProcessQuality 1"},
            {"medium", "r.DynamicGlobalIlluminationMethod 0,r.Nanite 1,sg.ShadowQuality 3,sg.TextureQuality 3,sg.EffectsQuality 3,sg.PostProcessQuality 3"},
            {"high",   "r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,r.Nanite 1,sg.ShadowQuality 4,sg.TextureQuality 4,sg.EffectsQuality 4,sg.PostProcessQuality 4"},
            {"ultra",  "r.DynamicGlobalIlluminationMethod 1,r.ReflectionMethod 1,r.Nanite 1,sg.ShadowQuality 5,sg.TextureQuality 5,sg.EffectsQuality 5,sg.PostProcessQuality 5,r.Shadow.MaxResolution 4096,r.Shadow.MaxCSMResolution 4096"},
        };
        const QString cmds = kExec.value(quality);
        if (!cmds.isEmpty()) launchArgs << QString("-ExecCmds=%1").arg(cmds);
    }
    log(QString("Launching: %1 %2").arg(launchProgram).arg(launchArgs.join(' ')));
    if (!mapName.isEmpty()) log("Map: " + mapName);

    qint64 pid = 0;
    if (!QProcess::startDetached(launchProgram, launchArgs, workingDir, &pid) || pid <= 0) {
        logErr("Failed to launch CARLA.");
        logErr("Check launcher path and permissions: " + launchProgram);
        return 1;
    }

    writePid(pid);
    log(QString("CARLA started (PID %1).").arg(pid));
    log("RPC port: " + QString::number(rpc_port));

    if (showProgress) {
        const bool launchedViaEditor = launchProgram.endsWith("/UnrealEditor");
        const int kMaxWait = (waitSeconds > 0) ? waitSeconds : (launchedViaEditor ? 300 : 120);
        log(QString("Waiting for CARLA on port %1 (max %2 s)...").arg(rpc_port).arg(kMaxWait));
        bool ready = false;
        bool exited = false;
        for (int s = 3; s <= kMaxWait; s += 3) {
            if (!QFileInfo(QString("/proc/%1").arg(pid)).exists()) {
                exited = true;
                out() << "\n";
                log("Port " + QString::number(rpc_port) + " not reachable after "
                    + QString::number(s) + " s - process exited, check /tmp/carla_studio_launch.log");
                break;
            }
            QProcess nc;
            nc.start("/bin/sh", QStringList() << "-c"
                     << QString("nc -z -w2 127.0.0.1 %1").arg(rpc_port));
            nc.waitForFinished(5000);
            if (nc.exitCode() == 0) { ready = true; break; }
            out() << QString("\r[start] waiting... (%1/%2 s)   ").arg(s).arg(kMaxWait);
            out().flush();
        }
        out() << "\n";
        if (ready)
            log("CARLA is ready on port " + QString::number(rpc_port));
        else if (exited)
            return 1;
        else if (QFileInfo(QString("/proc/%1").arg(pid)).exists())
            log("CARLA still initializing after " + QString::number(kMaxWait)
                + " s (process alive). Keep waiting; check /tmp/carla_studio_launch.log");
        else
            log("Port " + QString::number(rpc_port) + " not reachable after "
                + QString::number(kMaxWait) + " s — process exited, check /tmp/carla_studio_launch.log");
    }

    return 0;
}
