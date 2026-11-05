#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

enum class Attempt { WrongPassword, ConsoleSwitch };

// Garde la trace des tentatives et, si les photos sont demandées, en prend une de chacune.
// Rien ici ne doit pouvoir gêner le verrou : toute erreur laisse simplement moins de traces.
class AttemptLog {
public:
    AttemptLog(const std::string& home, bool photos_wanted, std::string camera);
    ~AttemptLog();
    AttemptLog(const AttemptLog&) = delete;
    AttemptLog& operator=(const AttemptLog&) = delete;

    void record(Attempt attempt);
    void poll(); // ramasse la capture terminée, tue celle qui reste bloquée
    void finish(); // laisse à la capture en cours un court délai pour finir
    std::string summary() const; // texte à afficher après le déverrouillage

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Attempt attempt;
        std::string time;
        std::string photo;
    };

    std::string start_photo(const std::string& stamp, std::string& problem);
    void stop_capture();

    std::string directory_;
    bool photos_wanted_;
    std::string camera_; // vide si aucune caméra n'a été trouvée
    std::vector<Entry> entries_;
    pid_t capture_ = -1;
    Clock::time_point capture_started_{};
    std::optional<Clock::time_point> last_photo_;
    std::optional<Clock::time_point> last_switch_;
    int photos_ = 0;
};
