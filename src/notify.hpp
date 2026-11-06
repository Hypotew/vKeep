#pragma once

#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

// Ce que contient ~/.config/vkeep/config, et ce qui ne va pas dedans.
struct NotifyConfig {
    std::string path;
    std::string webhook; // vide : alertes désactivées
    std::vector<std::string> warnings;
};

NotifyConfig load_notify_config(const std::string& home);

// Couleurs des embeds, celles du logo.
namespace alert_color {
constexpr int BLUE = 0x2f6fd1;   // verrouillage
constexpr int VIOLET = 0x8a63e6; // test
constexpr int CORAL = 0xe5534b;  // tentative
constexpr int GREEN = 0x3fb950;  // déverrouillage
constexpr int AMBER = 0xd29922;  // verrou interrompu, alertes suspendues
} // namespace alert_color

// Un message Discord : une ligne pour la notification du téléphone, puis un embed.
struct Alert {
    std::string headline;
    std::string title;
    std::string description;
    int color = alert_color::BLUE;
    std::vector<std::pair<std::string, std::string>> fields; // affichés côte à côte
    std::string photo;                                       // affichée en grand dans l'embed
};

// Envoie les alertes sur un salon Discord par son webhook. curl tourne dans son propre processus :
// le verrou n'attend jamais le réseau, et un envoi raté ne laisse qu'une ligne dans le journal d'erreurs.
class Notifier {
public:
    Notifier(std::string webhook, std::string error_log);
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    bool enabled() const { return !webhook_.empty(); }
    const std::string& host() const { return host_; }

    // essential : part même quand la limite d'alertes est atteinte (verrouillage, déverrouillage).
    void send(const Alert& alert, bool essential = false);
    void poll(); // ramasse les envois terminés

    // Pour --test-notify : envoie, attend la fin et laisse curl écrire ses erreurs sur le terminal.
    // Renvoie le code de sortie de curl, ou -1 s'il n'a pas pu être lancé.
    int send_and_wait(const Alert& alert);

private:
    pid_t spawn(const Alert& alert, bool to_terminal);
    void log_error(const std::string& message) const;

    std::string webhook_;
    std::string error_log_;
    std::string host_;
    std::vector<pid_t> running_;
    int sent_ = 0;
    bool suspended_ = false;
};
