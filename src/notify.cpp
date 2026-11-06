#include "notify.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <vector>

#include <limits.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr int MAX_ALERTS_PER_LOCK = 30; // Discord limite les webhooks, et un insistant ne doit pas noyer le salon
constexpr std::size_t MAX_RUNNING = 8;

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return "";
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::string config_directory(const std::string& home) {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (config != nullptr && config[0] == '/')
        return std::string(config) + "/vkeep";
    return home + "/.config/vkeep";
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            std::array<char, 8> code{};
            std::snprintf(code.data(), code.size(), "\\u%04x", c);
            out += code.data();
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

// Chaîne entre guillemets pour un fichier de configuration de curl, et pour un nom de fichier de -F.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}

std::string local_host() {
    std::array<char, HOST_NAME_MAX + 1> name{};
    if (gethostname(name.data(), name.size() - 1) != 0 || name[0] == '\0')
        return "machine";
    return name.data();
}

} // namespace

NotifyConfig load_notify_config(const std::string& home) {
    NotifyConfig config;
    config.path = config_directory(home) + "/config";
    std::ifstream file(config.path);
    if (!file)
        return config; // pas de fichier : pas d'alertes, c'est le cas normal

    struct stat info{};
    if (stat(config.path.c_str(), &info) == 0 && (info.st_mode & 077) != 0)
        config.warnings.push_back(config.path + " est lisible par d'autres comptes, qui pourraient écrire sur ton salon : chmod 600 " + config.path);

    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
        ++number;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            config.warnings.push_back(config.path + ", ligne " + std::to_string(number) + " : il manque « = »");
            continue;
        }
        const std::string key = trim(line.substr(0, equal));
        std::string value = trim(line.substr(equal + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key != "discord_webhook") {
            config.warnings.push_back(config.path + ", ligne " + std::to_string(number) + " : réglage inconnu « " + key + " »");
        } else if (value.rfind("https://", 0) != 0 || value.find_first_of(" \t") != std::string::npos) {
            config.warnings.push_back(config.path + ", ligne " + std::to_string(number) +
                                      " : discord_webhook doit être une adresse https://, alertes désactivées");
            config.webhook.clear();
        } else {
            config.webhook = value;
        }
    }
    return config;
}

Notifier::Notifier(std::string webhook, std::string error_log)
    : webhook_(std::move(webhook)), error_log_(std::move(error_log)), host_(local_host()) {}

void Notifier::send(const std::string& text, const std::string& photo, bool essential) {
    if (!enabled())
        return;
    try {
        poll();
        if (!essential) {
            if (sent_ >= MAX_ALERTS_PER_LOCK) {
                if (!suspended_) {
                    suspended_ = true;
                    const pid_t pid = spawn("⏸️ **" + host_ + "** : trop de tentatives, alertes suspendues jusqu'au déverrouillage", "", false);
                    if (pid > 0)
                        running_.push_back(pid);
                }
                return;
            }
            if (running_.size() >= MAX_RUNNING) {
                log_error("alerte abandonnée : trop d'envois en cours");
                return;
            }
            ++sent_;
        }
        if (const pid_t pid = spawn(text, photo, false); pid > 0)
            running_.push_back(pid);
    } catch (const std::exception&) {
        // Une alerte perdue vaut mieux qu'un verrou qui s'arrête.
    }
}

void Notifier::poll() {
    std::erase_if(running_, [this](pid_t pid) {
        int status = 0;
        // Jamais waitpid(-1) : PAM et la caméra attendent leurs propres processus.
        if (waitpid(pid, &status, WNOHANG) != pid)
            return false;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            log_error("échec de l'envoi (curl : code " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
        return true;
    });
}

int Notifier::send_and_wait(const std::string& text) {
    const pid_t pid = spawn(text, "", true);
    if (pid <= 0)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

pid_t Notifier::spawn(const std::string& text, const std::string& photo, bool to_terminal) {
    // L'adresse du webhook est un secret : elle passe par un tube, jamais par la ligne de commande
    // que tout le monde peut lire dans ps.
    std::string config = "silent\nshow-error\nfail\nmax-time = 20\nretry = 3\nretry-max-time = 90\noutput = /dev/null\n";
    config += "url = " + quoted(webhook_) + "\n";
    const std::string payload = "{\"content\":" + json_string(text) +
                                ",\"username\":\"vKeep\",\"allowed_mentions\":{\"parse\":[]}}";
    config += "form-string = " + quoted("payload_json=" + payload) + "\n";
    if (!photo.empty())
        config += "form = " + quoted("files[0]=@" + quoted(photo) + ";type=image/jpeg") + "\n";

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0)
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[0], STDIN_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!to_terminal)
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, error_log_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);

    // Session à part : curl finit son envoi même après la sortie du verrou ou la fermeture du terminal.
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int signum : {SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGHUP})
        sigaddset(&defaults, signum);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | (to_terminal ? 0 : POSIX_SPAWN_SETSID));

    // -q d'abord : un ~/.curlrc ne doit pas changer l'envoi.
    const char* const arguments[] = {"curl", "-q", "--config", "-", nullptr};
    pid_t pid = -1;
    const int status = posix_spawnp(&pid, "curl", &actions, &attributes, const_cast<char* const*>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipe_fds[0]);
    if (status != 0) {
        close(pipe_fds[1]);
        log_error(std::string("curl n'a pas pu être lancé : ") + std::strerror(status));
        return -1;
    }

    // Quelques centaines d'octets : le tube les prend d'un coup, même si curl tarde à lire.
    const char* data = config.data();
    std::size_t left = config.size();
    while (left > 0) {
        const ssize_t written = write(pipe_fds[1], data, left);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        data += written;
        left -= static_cast<std::size_t>(written);
    }
    close(pipe_fds[1]);
    return pid;
}

void Notifier::log_error(const std::string& message) const {
    const std::time_t timestamp = std::time(nullptr);
    std::tm now{};
    localtime_r(&timestamp, &now);
    std::array<char, 32> stamp{};
    std::strftime(stamp.data(), stamp.size(), "%Y-%m-%dT%H:%M:%S%z", &now);
    // Créé en 0600 comme celui où curl écrit : c'est le même fichier.
    const int fd = open(error_log_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    const std::string line = std::string(stamp.data()) + " vkeep: " + message + '\n';
    [[maybe_unused]] const ssize_t written = write(fd, line.data(), line.size());
    close(fd);
}
