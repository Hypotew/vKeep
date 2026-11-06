#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

class Notifier;

enum class Attempt { WrongPassword, ConsoleSwitch };

// Dossier des photos et des journaux : $XDG_STATE_HOME/vkeep, ou ~/.local/state/vkeep.
std::string state_directory(const std::string& home);

// Garde la trace des tentatives et, si les photos sont demandées, en prend une de chacune.
// Chaque tentative part aussi en alerte, avec sa photo une fois prise.
// Rien ici ne doit pouvoir gêner le verrou : toute erreur laisse simplement moins de traces.
class AttemptLog {
public:
    AttemptLog(const std::string& home, bool photos_wanted, std::string camera, Notifier& notifier);
    ~AttemptLog();
    AttemptLog(const AttemptLog&) = delete;
    AttemptLog& operator=(const AttemptLog&) = delete;

    void record(Attempt attempt);
    void poll(); // ramasse la capture terminée, tue celle qui reste bloquée
    void finish(); // laisse à la capture en cours un court délai pour finir
    std::string summary() const; // texte à afficher après le déverrouillage
    std::size_t count() const { return entries_.size(); }

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        Attempt attempt;
        std::string time;
        std::string photo;
    };

    std::string start_photo(const std::string& stamp, std::string& problem);
    void stop_capture();
    void capture_done(bool succeeded); // envoie l'alerte qui attendait la photo

    std::string directory_;
    bool photos_wanted_;
    std::string camera_; // vide si aucune caméra n'a été trouvée
    Notifier& notifier_;
    std::string pending_alert_; // alerte de la capture en cours
    std::string pending_photo_;
    std::vector<Entry> entries_;
    pid_t capture_ = -1;
    Clock::time_point capture_started_{};
    std::optional<Clock::time_point> last_photo_;
    std::optional<Clock::time_point> last_switch_;
    int photos_ = 0;
};
