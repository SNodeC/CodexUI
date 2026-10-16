// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/LaunchPlan.h"

#include <QProcessEnvironment>

namespace codexui::manager {
    namespace {
        bool tls(const QString& t) {
            return t.contains("tls") || t.startsWith("wss");
        }
        QString frontendInstance(const QString& prefix, const QString& transport) {
            return transport == "unix" && prefix == "codex-bridge" ? prefix : prefix + '-' + transport;
        }
        void endpointArguments(QStringList& args, const Endpoint& e, const QString& instance, bool server) {
            args << instance;
            if (server && tls(e.transport)) {
                args << "tls" << "--cert" << e.certificate << "--cert-key" << e.privateKey;
            }
            if (!server && tls(e.transport)) {
                args << "tls" << "--ca-cert-dir-use-default=true";
                if (!e.caCertificate.isEmpty()) {
                    args << "--ca-cert" << e.caCertificate;
                }
                if (!e.serverName.isEmpty()) {
                    args << "--sni" << e.serverName;
                }
            }
            args << (server ? "local" : "remote");
            if (e.transport == "unix") {
                args << "--sun-path" << e.socketPath;
            } else if (e.transport.startsWith("rfcomm")) {
                args << "--host" << (server ? e.bindHost : e.host) << "--channel" << QString::number(e.channel);
            } else {
                args << "--host" << (server ? e.bindHost : e.host) << "--port" << QString::number(e.port);
            }
        }
        Command frontendCommand(const Profile& p, const QString& program, const QString& prefix, bool probe) {
            Command c{program, {"--config-file", "/dev/null"}, {}, {}};
            c.arguments << (probe ? QStringLiteral("codex-client") : prefix);
            if (probe) {
                c.arguments << "--json";
            }
            c.arguments << "--bridge-websocket-endpoint" << p.frontend.webSocketPath;
            if (p.frontend.transport != "unix") {
                c.arguments << prefix + "-unix" << "--disabled=true";
            }
            const auto instance = frontendInstance(prefix, p.frontend.transport);
            c.arguments << instance << "--disabled=false";
            if (probe) {
                c.arguments << "socket" << "--retry=true" << "--retry-tries=120" << "--retry-limit=1" << "--retry-timeout=0.25"
                            << "--reconnect=true";
            }
            endpointArguments(c.arguments, p.frontend, instance, false);
            return c;
        }
    } // namespace
    QString Command::shell() const {
        QStringList parts;
        if (!environment.isEmpty()) {
            parts << "env";
            for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
                parts << quote(it.key() + '=' + it.value());
            }
        }
        parts << quote(program);
        for (const auto& arg : arguments) {
            parts << quote(arg);
        }
        return (workingDirectory.isEmpty() ? QString{} : "cd " + quote(workingDirectory) + " && ") + parts.join(' ');
    }
    Command onHost(const Host& host, Command c) {
        if (!host.sshTarget.isEmpty()) {
            QStringList args{"-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=yes"};
            if (!host.identityFile.isEmpty()) {
                args << "-i" << host.identityFile;
            }
            args << "--" << host.sshTarget << c.shell();
            c = {"ssh", args, {}, {}};
        }
        return c;
    }
    Command Component::configurationCheck() const {
        Command check = command;
        if (check.program == "flock") {
            check.program = check.arguments.value(3);
            check.arguments = check.arguments.mid(4);
        }
        if (name == "App-server") {
            check.arguments = {"app-server", "--help"};
        } else {
            check.arguments.prepend("--show-config");
        }
        if (name == "CodexUI") {
            check.environment.insert("QT_QPA_PLATFORM", "offscreen");
        }
        return onHost(host, check);
    }
    bool Component::acceptsConfiguration(int code, const QString& output) const {
        return name == "App-server"
                   ? code == 0 && output.contains("--listen")
                   : (code == 0 || code == 2) && output.contains("snodec.config.comment-meta") && !output.contains("Error]");
    }
    Command serviceStart(const Component& c) {
        QStringList args{"--user",
                         "--quiet",
                         "--collect",
                         "--service-type=exec",
                         "--unit=" + c.unit,
                         "--description=CodexManager " + c.unit,
                         "--property=KillMode=mixed",
                         "--property=TimeoutStopSec=15s"};
        if (!c.command.workingDirectory.isEmpty()) {
            args << "--working-directory=" + c.command.workingDirectory;
        }
        for (auto i = c.command.environment.cbegin(); i != c.command.environment.cend(); ++i) {
            args << "--setenv=" + i.key() + '=' + i.value();
        }
        args << "--" << c.command.program;
        args << c.command.arguments;
        return onHost(c.host, {"systemd-run", args, {}, {}});
    }
    Command serviceCommand(const Component& c, const QString& action) {
        QStringList args{"--user", action};
        if (action == "show") {
            args << "--property=LoadState" << "--property=ActiveState" << "--property=MainPID" << "--property=Description";
        }
        args << "--" << c.unit;
        return onHost(c.host, {"systemctl", args, {}, {}});
    }
    Command serviceLogs(const Component& c) {
        return onHost(c.host, {"journalctl", {"--user", "--unit=" + c.unit, "--lines=100", "--no-pager", "--output=short-iso"}, {}, {}});
    }
    LaunchPlan LaunchPlan::forProfile(const Profile& p) {
        LaunchPlan plan;
        if (p.provider.transport != "stdio") {
            Command app{p.appServerExecutable, {"app-server", "--listen"}, p.appServer.environment, p.appServer.workingDirectory};
            app.environment.insert("CODEX_HOME", p.codexHome);
            if (p.provider.transport == "unix") {
                app.arguments << "unix://" + p.provider.socketPath;
            } else {
                app.arguments << "ws://" + (p.provider.transport.endsWith("ipv6") ? '[' + p.provider.bindHost + ']' : p.provider.bindHost) +
                                     ':' + QString::number(p.provider.port);
            }
            plan.backends.push_back({"App-server", p.appServer, p.unit("appserver"), app});
        }
        Command bridge{p.bridgeExecutable,
                       {"--config-file",
                        "/dev/null",
                        "--daemonize=false",
                        "codex",
                        "--app-server-transport",
                        p.provider.transport,
                        "--bridge-websocket-endpoint",
                        p.frontend.webSocketPath},
                       p.bridge.environment,
                       p.bridge.workingDirectory};
        if (p.provider.transport == "stdio") {
            bridge.arguments << "--app-server-executable" << p.appServerExecutable << "--codex-home" << p.codexHome;
        } else {
            QString instance = "codex-bridge-app-server-" + p.provider.transport;
            bridge.arguments << instance << "socket" << "--retry=true" << "--retry-tries=120" << "--retry-limit=1" << "--retry-timeout=0.25"
                             << "--reconnect=true";
            endpointArguments(bridge.arguments, p.provider, instance, false);
        }
        if (p.frontend.transport != "unix") {
            bridge.arguments << "codex-bridge" << "--disabled=true";
        }
        const auto instance = frontendInstance("codex-bridge", p.frontend.transport);
        bridge.arguments << instance << "--disabled=false";
        endpointArguments(bridge.arguments, p.frontend, instance, true);
        plan.backends.push_back({"Bridge", p.bridge, p.unit("bridge"), bridge});
        // One launch authority per effective home, even across different manager stores.
        auto& owner = p.provider.transport == "stdio" ? plan.backends.last() : plan.backends.first();
        const auto original = owner.command;
        owner.command.program = "flock";
        owner.command.arguments = {"--nonblock", "--", p.codexHome + "/.codexmanager-runtime.lock", original.program};
        owner.command.arguments << original.arguments;
        plan.probe = frontendCommand(p, p.probeExecutable, "codex-bridge-client", true);
        auto ui = frontendCommand(p, p.uiExecutable, "codex-ui", false);
        ui.environment = {{"CODEXUI_PROFILE_ID", p.id},
                          {"CODEXUI_PROFILE_NAME", p.name},
                          {"CODEXUI_PROFILE_ACCENT", accentColor(p.accent)},
                          {"CODEXUI_MANAGER_SOCKET", p.runtimeSocket()},
                          {"CODEXUI_SETTINGS_DIRECTORY", p.settingsDirectory()}};
        for (const auto& key : {"DISPLAY",
                                "WAYLAND_DISPLAY",
                                "XDG_RUNTIME_DIR",
                                "DBUS_SESSION_BUS_ADDRESS",
                                "XAUTHORITY",
                                "QT_QPA_PLATFORM",
                                "QT_SCALE_FACTOR"}) {
            if (qEnvironmentVariableIsSet(key)) {
                ui.environment.insert(QString::fromLatin1(key), qEnvironmentVariable(key));
            }
        }
        plan.ui = {"CodexUI", Host{}, p.unit("ui"), ui};
        return plan;
    }
    QString LaunchPlan::instructions(const Profile& p) const {
        QString result = "PROFILE: " + p.name + "\n\nCommands below use a POSIX shell. Paths belong to the named host.\n";
        result += "Managed components require systemd user services. Remote management additionally requires noninteractive SSH and a "
                  "verified host key.\n";
        result += "For unattended remote services, ensure the remote user service manager remains running after logout (ask the host "
                  "administrator about loginctl enable-linger).\n";
        result += "Use Account / Sign in in CodexManager after connecting the backend. Device-code login works for local and remote "
                  "app-servers. Browser login requires the callback on the app-server host. Credentials remain with that app-server. "
                  "Fallback commands below run on the app-server host with the selected CODEX_HOME.\n\n";
        Host appHost = p.provider.transport == "stdio" ? p.bridge : p.appServer;
        result += "APP-SERVER HOME — " + appHost.label() + "\n";
        result += Command{"mkdir", {"-p", "-m", "700", p.codexHome}, {}, {}}.shell() + "\n";
        for (const auto& args :
             {QStringList{"login"}, QStringList{"login", "--device-auth"}, QStringList{"login", "status"}, QStringList{"logout"}}) {
            result += Command{p.appServerExecutable, args, {{"CODEX_HOME", p.codexHome}}, {}}.shell() + "\n";
        }
        result += "Choose one login method; status checks the account, logout signs out.\n\n";
        if (p.provider.transport == "stdio") {
            result += "APP-SERVER: owned and started by the bridge over stdio. Do not start a second app-server for this profile.\n\n";
        }
        for (const auto& c : backends) {
            result += c.name.toUpper() + " — " + c.host.label() + " — " + ownershipLabel(c.host.ownership) + "\n";
            result += "Manual foreground launch on that host:\n" + c.command.shell() + "\nStop a foreground launch with Ctrl+C.\n";
            if (c.host.ownership != Ownership::External) {
                result += "Manager launch (run on this machine):\n" + serviceStart(c).shell() + "\n";
                result += "Verify owned service:\n" + serviceCommand(c, "show").shell() + "\n";
                result += "Stop owned service:\n" + serviceCommand(c, "stop").shell() + "\n";
                result += "Read owned service logs:\n" + serviceLogs(c).shell() + "\n";
            }
            result += '\n';
        }
        result += "VERIFY BRIDGE PROTOCOL — this machine\n" + probe.shell() + "\n";
        result += "Expected: client.connection connected, then bridge.provider state ready. Run release if this diagnostic client received "
                  "control; quit exits it. A reachable endpoint alone is not readiness.\n\n";
        result += "OPEN CODEXUI — this machine, after provider readiness\n" + ui.command.shell() + "\n\n";
        result += "Network listeners require suitable firewall and access controls. A profile is not an operating-system security "
                  "boundary. Use configured TLS for supported frontend network connections.\n";
        return result;
    }
} // namespace codexui::manager
