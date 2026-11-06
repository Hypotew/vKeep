#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

// Ce que contient ~/.config/vkeep/config, et ce qui ne va pas dedans.
struct NotifyConfig {
    std::string path;
    std::string webhook; // vide : alertes désactivées
    std::vector<std::string> warnings;
};

NotifyConfig load_notify_config(const std::string& home);

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
    void send(const std::string& text, const std::string& photo = "", bool essential = false);
    void poll(); // ramasse les envois terminés

    // Pour --test-notify : envoie, attend la fin et laisse curl écrire ses erreurs sur le terminal.
    // Renvoie le code de sortie de curl, ou -1 s'il n'a pas pu être lancé.
    int send_and_wait(const std::string& text);

private:
    pid_t spawn(const std::string& text, const std::string& photo, bool to_terminal);
    void log_error(const std::string& message) const;

    std::string webhook_;
    std::string error_log_;
    std::string host_;
    std::vector<pid_t> running_;
    int sent_ = 0;
    bool suspended_ = false;
};
