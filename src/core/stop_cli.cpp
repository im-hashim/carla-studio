// Copyright (C) 2026 Abdul, Hashim.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include <algorithm>

#include <csignal>
#include <sys/types.h>

namespace {

QTextStream &out() { static QTextStream s(stdout); return s; }
static void log(const QString &line) { out() << "[stop] " << line << "\n"; out().flush(); }

static QString pidFile() {
    return QDir::homePath() + "/.config/carla-studio/carla-pids.txt";
}

static QList<qint64> readTrackedPids() {
    QList<qint64> pids;
    QFile f(pidFile());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return pids;
    for (const QString &line : QString::fromLocal8Bit(f.readAll()).split('\n', Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 p = line.trimmed().toLongLong(&ok);
        if (ok && p > 1) pids << p;
    }
    return pids;
}

static bool pid_alive(qint64 pid) {
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

static QList<qint64> pgrepPids(const QString &pattern) {
    QList<qint64> pids;
    QProcess p;
    p.start("pgrep", {"-f", pattern});
    p.waitForFinished(3000);
    for (const QString &tok : QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 pid = tok.trimmed().toLongLong(&ok);
        if (ok && pid > 1) pids << pid;
    }
    return pids;
}

static void killGroup(const QString &pattern, int sig) {
    for (qint64 pid : pgrepPids(pattern)) {
        ::kill(static_cast<pid_t>(pid), sig);
        log(QString("  signal %1 → PID %2 [%3]").arg(sig).arg(pid).arg(pattern));
    }
}

static void stopStack() {
    log("Stopping Docker CARLA containers...");
    QProcess::execute("/bin/sh", {"-c",
        "docker stop carla-mcity-server 2>/dev/null; docker rm carla-mcity-server 2>/dev/null; "
        "docker ps -q --filter ancestor=carlasim/carla | xargs -r docker stop 2>/dev/null || true"});

    log("Stopping TeraSim...");
    killGroup("'terasim|TeraSim'", SIGTERM);

    log("Stopping ROS2 / visualization...");
    killGroup("'ros2|rviz|carla_av_ros2|carla_sensor_ros2|carla_cosim_ros2|gnss_decoder_fallback|planning_simulator_fallback'", SIGTERM);
    QProcess::execute("/bin/sh", {"-c", "ros2 daemon stop 2>/dev/null || true"});

    log("Stopping SUMO / scenario runners...");
    killGroup("'sumo-gui|sumo |libsumo|traci|terasim_examples|_example\\.py'", SIGTERM);

    log("Removing Docker network...");
    QProcess::execute("/bin/sh", {"-c", "docker network rm carla-net 2>/dev/null || true"});

    log("Stack stopped.");
}

}

int carla_cli_stop_main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    bool full = false;
    QString remoteHost;
    QString remoteUser;
    QString remotePass;
    QString remoteBindAddress;
    for (const QString &a : args)
        if (a == "--full" || a == "-f") full = true;

    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == "--remote-host" && i + 1 < args.size())
            remoteHost = args[++i];
        else if (a == "--remote-user" && i + 1 < args.size())
            remoteUser = args[++i];
        else if (a == "--remote-pass" && i + 1 < args.size())
            remotePass = args[++i];
        else if (a == "--remote-bind-address" && i + 1 < args.size())
            remoteBindAddress = args[++i];
    }

    if (!remoteHost.isEmpty()) {
        if (remoteUser.isEmpty()) {
            log("--remote-user is required when --remote-host is set.");
            return 2;
        }

        QStringList remote;
        remote << "set -e";
        remote << "SELF=$$";
        remote << "PARENT=$PPID";
        remote << "PIDFILE=\"$HOME/.config/carla-studio/carla-pids.txt\"";
        remote << "PIDS=\"\"";
        remote << "if [ -f \"$PIDFILE\" ]; then";
        remote << "  while IFS= read -r line; do";
        remote << "    case \"$line\" in ''|*[!0-9]*) continue;; esac";
        remote << "    [ \"$line\" -eq \"$SELF\" ] && continue";
        remote << "    [ \"$line\" -eq \"$PARENT\" ] && continue";
        remote << "    [ \"$line\" -gt 1 ] && PIDS=\"$PIDS $line\"";
        remote << "  done < \"$PIDFILE\"";
        remote << "fi";
        remote << "for p in $(pgrep -f 'CarlaUE[45]-Linux|UnrealEditor.*CarlaUnreal|CarlaUnreal-Linux-Shipping|CarlaUnreal\\.sh|CarlaUE[45]\\.sh' 2>/dev/null || true); do";
        remote << "  [ \"$p\" -eq \"$SELF\" ] && continue";
        remote << "  [ \"$p\" -eq \"$PARENT\" ] && continue";
        remote << "  PIDS=\"$PIDS $p\"";
        remote << "done";
        remote << "if [ -z \"$PIDS\" ]; then";
        remote << "  echo '[stop] No tracked CARLA processes found - nothing to stop.'";
        remote << "else";
        remote << "  echo '[stop] Sending SIGTERM...'";
        remote << "  for p in $PIDS; do [ \"$p\" -eq \"$SELF\" ] && continue; [ \"$p\" -eq \"$PARENT\" ] && continue; kill -TERM \"$p\" 2>/dev/null && echo \"[stop]   SIGTERM -> PID $p\" || true; done";
        remote << "  for _ in $(seq 1 30); do";
        remote << "    alive=0";
        remote << "    for p in $PIDS; do [ \"$p\" -eq \"$SELF\" ] && continue; [ \"$p\" -eq \"$PARENT\" ] && continue; kill -0 \"$p\" 2>/dev/null && alive=1; done";
        remote << "    [ \"$alive\" -eq 0 ] && break";
        remote << "    sleep 2";
        remote << "  done";
        remote << "  for p in $PIDS; do [ \"$p\" -eq \"$SELF\" ] && continue; [ \"$p\" -eq \"$PARENT\" ] && continue; kill -0 \"$p\" 2>/dev/null && kill -KILL \"$p\" 2>/dev/null && echo \"[stop]   SIGKILL -> PID $p\" || true; done";
        remote << "  for p in $(pgrep -f 'CarlaUE4|CarlaUE5|CarlaUnreal' 2>/dev/null || true); do [ \"$p\" -eq \"$SELF\" ] && continue; [ \"$p\" -eq \"$PARENT\" ] && continue; kill -KILL \"$p\" 2>/dev/null && echo \"[stop]   SIGKILL (sweep) -> PID $p\" || true; done";
        remote << "  rm -f \"$PIDFILE\"";
        remote << "  echo '[stop] CARLA stopped.'";
        remote << "fi";
        if (full) {
            remote << "echo '[stop] Stopping full stack extras...'";
            remote << "docker stop carla-mcity-server 2>/dev/null || true";
            remote << "docker rm carla-mcity-server 2>/dev/null || true";
            remote << "docker ps -q --filter ancestor=carlasim/carla | xargs -r docker stop 2>/dev/null || true";
            remote << "pkill -TERM -f 'terasim|TeraSim' 2>/dev/null || true";
            remote << "pkill -TERM -f 'ros2|rviz|carla_av_ros2|carla_sensor_ros2|carla_cosim_ros2|gnss_decoder_fallback|planning_simulator_fallback' 2>/dev/null || true";
            remote << "ros2 daemon stop 2>/dev/null || true";
            remote << "pkill -TERM -f 'sumo-gui|sumo |libsumo|traci|terasim_examples|_example\\.py' 2>/dev/null || true";
            remote << "docker network rm carla-net 2>/dev/null || true";
            remote << "echo '[stop] Stack stopped.'";
        }

        QProcess p;
        QString program = "ssh";
        QStringList sshArgs;
        if (!remotePass.isEmpty()) {
            QProcess check;
            check.start("/bin/sh", {"-c", "command -v sshpass"});
            check.waitForFinished(2000);
            if (check.exitCode() != 0) {
                log("sshpass is required for --remote-pass but is not installed.");
                return 2;
            }
            program = "sshpass";
            sshArgs << "-p" << remotePass << "ssh";
        }
        if (!remoteBindAddress.trimmed().isEmpty())
            sshArgs << "-o" << QString("BindAddress=%1").arg(remoteBindAddress.trimmed());
        sshArgs << "-o" << "StrictHostKeyChecking=accept-new";
        sshArgs << QString("%1@%2").arg(remoteUser, remoteHost);
        sshArgs << remote.join("\n");

        p.setProcessChannelMode(QProcess::ForwardedChannels);
        p.start(program, sshArgs);
        if (!p.waitForStarted(5000)) {
            log("Failed to start SSH process.");
            return 1;
        }
        p.waitForFinished(120000);
        return p.exitCode();
    }

    QList<qint64> pids = readTrackedPids();
    for (qint64 p : pgrepPids("'CarlaUE[45]-Linux|UnrealEditor.*CarlaUnreal|CarlaUnreal-Linux-Shipping|CarlaUnreal\\.sh|CarlaUE[45]\\.sh'")) {
        if (!pids.contains(p)) pids << p;
    }

    if (pids.isEmpty()) {
        log("No tracked CARLA processes found — nothing to stop.");
    } else {
        log(QString("Sending SIGTERM to %1 process(es)...").arg(pids.size()));
        for (qint64 pid : pids) {
            if (pid_alive(pid)) {
                ::kill(static_cast<pid_t>(pid), SIGTERM);
                log(QString("  SIGTERM → PID %1").arg(pid));
            }
        }

        const int grace_steps = 30;
        const int step_ms = 2000;
        for (int i = 0; i < grace_steps; ++i) {
            QThread::msleep(static_cast<unsigned long>(step_ms));
            bool any_alive = false;
            for (qint64 pid : pids) {
                if (pid_alive(pid)) { any_alive = true; break; }
            }
            if (!any_alive) break;
        }

        for (qint64 pid : pids) {
            if (pid_alive(pid)) {
                ::kill(static_cast<pid_t>(pid), SIGKILL);
                log(QString("  SIGKILL → PID %1").arg(pid));
            }
        }

        for (qint64 pid : pgrepPids("'CarlaUE4|CarlaUE5|CarlaUnreal'")) {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
            log(QString("  SIGKILL (sweep) → PID %1").arg(pid));
        }

        QFile::remove(pidFile());
        log("CARLA stopped.");
    }

    if (full) stopStack();
    return 0;
}
