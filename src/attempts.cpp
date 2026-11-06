#include "attempts.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <thread>

#include "notify.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr auto PHOTO_INTERVAL = std::chrono::seconds(5);
constexpr auto SWITCH_DEBOUNCE = std::chrono::seconds(2); // Alt-Fn maintenu se répète
constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(10);
constexpr auto FINISH_TIMEOUT = std::chrono::seconds(4);
constexpr auto FINISH_POLL = std::chrono::milliseconds(50);
constexpr int MAX_PHOTOS_PER_LOCK = 50; // borne l'espace disque si quelqu'un insiste
constexpr std::size_t MAX_SUMMARY_LINES = 5;

std::string format_now(const char* format) {
    const std::time_t timestamp = std::time(nullptr);
    std::tm now{};
    localtime_r(&timestamp, &now);
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), format, &now);
    return buffer.data();
}

const char* attempt_key(Attempt attempt) {
    return attempt == Attempt::WrongPassword ? "wrong-password" : "console-switch";
}

const char* attempt_label(Attempt attempt) {
    return attempt == Attempt::WrongPassword ? "mauvais mot de passe" : "changement de console";
}

// ffmpeg a son propre processus : le verrou n'attend jamais la caméra et ne dépend pas d'elle.
// Renvoie 0, ou le code d'erreur si ffmpeg n'a pas pu être lancé.
int spawn_ffmpeg(const std::string& camera, const std::string& output, const std::string& errors, pid_t& pid) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errors.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);

    // Les signaux ignorés par le verrou le seraient aussi par ffmpeg : on doit pouvoir l'arrêter.
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int signum : {SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGHUP})
        sigaddset(&defaults, signum);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    // -ss 1 : la webcam a besoin d'un instant pour régler son exposition.
    const char* const arguments[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                                     "-f", "v4l2", "-i", camera.c_str(), "-ss", "1",
                                     "-frames:v", "1", "-update", "1", "-y", output.c_str(), nullptr};
    const int status = posix_spawnp(&pid, "ffmpeg", &actions, &attributes,
                                    const_cast<char* const*>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return status;
}

} // namespace

std::string state_directory(const std::string& home) {
    const char* state = std::getenv("XDG_STATE_HOME");
    if (state != nullptr && state[0] == '/')
        return std::string(state) + "/vkeep";
    return home + "/.local/state/vkeep";
}

AttemptLog::AttemptLog(const std::string& home, bool photos_wanted, std::string camera, Notifier& notifier)
    : directory_(state_directory(home)), photos_wanted_(photos_wanted), camera_(std::move(camera)),
      notifier_(notifier) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    std::filesystem::permissions(directory_, std::filesystem::perms::owner_all, error);
}

AttemptLog::~AttemptLog() {
    stop_capture();
}

void AttemptLog::record(Attempt attempt) {
    try {
        const auto now = Clock::now();
        if (attempt == Attempt::ConsoleSwitch) {
            if (last_switch_ && now - *last_switch_ < SWITCH_DEBOUNCE) {
                last_switch_ = now;
                return;
            }
            last_switch_ = now;
        }

        poll();
        std::string problem;
        const std::string photo = start_photo(format_now("%Y%m%d-%H%M%S"), problem);
        entries_.push_back({attempt, format_now("%H:%M:%S"), photo});

        std::ofstream journal(directory_ + "/attempts.log", std::ios::app);
        journal << format_now("%Y-%m-%dT%H:%M:%S%z") << ' ' << attempt_key(attempt);
        journal << ' ' << (photo.empty() ? "no-photo:" + problem : photo) << '\n';

        Alert alert;
        const std::string title = attempt == Attempt::WrongPassword ? "Mauvais mot de passe" : "Changement de console refusé";
        alert.headline = "⚠️ " + title + " sur **" + notifier_.host() + "**";
        alert.title = "⚠️ " + title;
        alert.color = alert_color::CORAL;
        alert.fields = {{"Machine", notifier_.host()}, {"Tentative", "n° " + std::to_string(entries_.size())}};
        if (!photo.empty()) {
            alert.photo = photo; // part avec la photo, une fois prise
            pending_alert_ = std::move(alert);
        } else {
            // Trop tôt ou capture en cours : la photo précédente montre déjà la personne.
            if (photos_wanted_ && problem != "too-soon" && problem != "capture-running")
                alert.fields.emplace_back("Photo", problem == "no-camera" ? "aucune caméra" : "impossible");
            notifier_.send(alert);
        }
    } catch (const std::exception&) {
        // Une tentative non notée vaut mieux qu'un verrou qui s'arrête.
    }
}

std::string AttemptLog::start_photo(const std::string& stamp, std::string& problem) {
    const auto now = Clock::now();
    if (!photos_wanted_)
        problem = "not-requested";
    else if (camera_.empty())
        problem = "no-camera";
    else if (capture_ != -1)
        problem = "capture-running";
    else if (photos_ >= MAX_PHOTOS_PER_LOCK)
        problem = "too-many-photos";
    else if (last_photo_ && now - *last_photo_ < PHOTO_INTERVAL)
        problem = "too-soon";
    if (!problem.empty())
        return "";

    const std::string photo = directory_ + "/" + stamp + ".jpg";
    const int status = spawn_ffmpeg(camera_, photo, directory_ + "/ffmpeg-errors.log", capture_);
    if (status != 0) {
        capture_ = -1;
        problem = std::string("spawn-failed:") + std::strerror(status);
        return "";
    }
    capture_started_ = now;
    last_photo_ = now;
    ++photos_;
    return photo;
}

void AttemptLog::poll() {
    if (capture_ == -1)
        return;
    // Jamais waitpid(-1) : PAM attend ses propres processus.
    int status = 0;
    if (waitpid(capture_, &status, WNOHANG) == capture_) {
        capture_ = -1;
        capture_done(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } else if (Clock::now() - capture_started_ > CAPTURE_TIMEOUT) {
        stop_capture();
    }
    notifier_.poll();
}

void AttemptLog::finish() {
    const auto deadline = Clock::now() + FINISH_TIMEOUT;
    while (capture_ != -1 && Clock::now() < deadline) {
        std::this_thread::sleep_for(FINISH_POLL);
        poll();
    }
    stop_capture();
}

void AttemptLog::stop_capture() {
    if (capture_ == -1)
        return;
    kill(capture_, SIGKILL);
    waitpid(capture_, nullptr, 0);
    capture_ = -1;
    capture_done(false);
}

void AttemptLog::capture_done(bool succeeded) {
    if (!pending_alert_)
        return;
    Alert alert = std::move(*pending_alert_);
    pending_alert_.reset();
    std::error_code error;
    const bool usable = succeeded && std::filesystem::file_size(alert.photo, error) > 0 && !error;
    if (!usable) {
        alert.photo.clear();
        alert.fields.emplace_back("Photo", "impossible");
    }
    notifier_.send(alert);
}

std::string AttemptLog::summary() const {
    if (entries_.empty())
        return "";

    std::string text = "\033[91m!\033[0m " + std::to_string(entries_.size()) +
                       (entries_.size() == 1 ? " tentative" : " tentatives") + " pendant le verrouillage\n";
    const std::size_t first = entries_.size() > MAX_SUMMARY_LINES ? entries_.size() - MAX_SUMMARY_LINES : 0;
    std::error_code error;
    for (std::size_t i = first; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        text += "  " + entry.time + "  " + attempt_label(entry.attempt);
        if (!entry.photo.empty() && std::filesystem::exists(entry.photo, error))
            text += "  \033[90m" + entry.photo + "\033[0m";
        else if (photos_wanted_)
            text += "  \033[90m(pas de photo)\033[0m";
        text += '\n';
    }
    text += "  \033[90mJournal : " + directory_ + "/attempts.log\033[0m\n";
    return text;
}
