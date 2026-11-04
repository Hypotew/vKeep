#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

#include "tty.hpp"

enum class StatusKind { None, Info, Error, Success };

struct ScreenState {
    std::string user;
    std::string tty;
    bool all_consoles = false;
    bool console = false;   // console Linux : 16 couleurs, police limitée
    bool truecolor = false; // ignoré sur console
    std::tm locked_at{};

    std::string prompt;
    bool checking = false;
    unsigned spinner_frame = 0;
    std::size_t hidden_cells = 0; // nombre de points affichés pour une saisie masquée
    bool echo = false;
    std::string echoed; // saisie visible, seulement si PAM demande de l'afficher

    StatusKind status_kind = StatusKind::None;
    std::string status;
};

// Construit les lignes de l'écran, déjà positionnées, sans jamais recevoir le mot de passe.
std::vector<std::string> render_rows(const ScreenState& state, TtySize size, const std::tm& now);

// Séquence qui charge les couleurs du verrou dans la palette de la console Linux.
std::string console_palette();
