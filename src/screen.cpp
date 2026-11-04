#include "screen.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {

constexpr const char* RESET = "\033[0m";
constexpr const char* BOLD = "\033[1m";

constexpr int BLOCK_WIDTH = 60;
constexpr int BOX_WIDTH = BLOCK_WIDTH - 4; // entre les deux bords
constexpr int GRADIENT_STEPS = 6; // couleurs 1 à 6 de la palette de console

constexpr std::array<const char*, 6> LOGO{
    "██╗   ██╗██╗  ██╗███████╗███████╗██████╗ ",
    "██║   ██║██║ ██╔╝██╔════╝██╔════╝██╔══██╗",
    "██║   ██║█████╔╝ █████╗  █████╗  ██████╔╝",
    "╚██╗ ██╔╝██╔═██╗ ██╔══╝  ██╔══╝  ██╔═══╝ ",
    " ╚████╔╝ ██║  ██╗███████╗███████╗██║     ",
    "  ╚═══╝  ╚═╝  ╚═╝╚══════╝╚══════╝╚═╝     ",
};
constexpr int LOGO_WIDTH = 41;
constexpr int CLOCK_FIRST_Y = 7; // le dégradé continue sous le logo et la barre

// Même police que le logo. Toutes les lignes d'un chiffre ont la même largeur.
constexpr std::array<std::array<const char*, 6>, 10> DIGITS{{
    {" ██████╗ ", "██╔═████╗", "██║██╔██║", "████╔╝██║", "╚██████╔╝", " ╚═════╝ "},
    {" ██╗", "███║", "╚██║", " ██║", " ██║", " ╚═╝"},
    {"██████╗ ", "╚════██╗", " █████╔╝", "██╔═══╝ ", "███████╗", "╚══════╝"},
    {"██████╗ ", "╚════██╗", " █████╔╝", " ╚═══██╗", "██████╔╝", "╚═════╝ "},
    {"██╗  ██╗", "██║  ██║", "███████║", "╚════██║", "     ██║", "     ╚═╝"},
    {"███████╗", "██╔════╝", "███████╗", "╚════██║", "███████║", "╚══════╝"},
    {" ██████╗ ", "██╔════╝ ", "███████╗ ", "██╔═══██╗", "╚██████╔╝", " ╚═════╝ "},
    {"███████╗", "╚════██║", "    ██╔╝", "   ██╔╝ ", "   ██║  ", "   ╚═╝  "},
    {" █████╗ ", "██╔══██╗", "╚█████╔╝", "██╔══██╗", "╚█████╔╝", " ╚════╝ "},
    {" █████╗ ", "██╔══██╗", "╚██████║", " ╚═══██║", " █████╔╝", " ╚════╝ "},
}};
constexpr std::array<const char*, 6> COLON{"   ", "██╗", "╚═╝", "██╗", "╚═╝", "   "};

constexpr std::array<const char*, 10> SPINNER{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
constexpr std::array<const char*, 4> CONSOLE_SPINNER{"|", "/", "-", "\\"};

constexpr std::array<const char*, 7> DAYS{"dimanche", "lundi",    "mardi", "mercredi",
                                          "jeudi",    "vendredi", "samedi"};
constexpr std::array<const char*, 12> MONTHS{"janvier", "février", "mars",     "avril",
                                             "mai",     "juin",    "juillet",  "août",
                                             "septembre", "octobre", "novembre", "décembre"};

struct Rgb {
    int r, g, b;
};

// Dégradé du logo (t de 0 à 100) : bleu → violet → corail.
Rgb gradient(int t) {
    t = std::clamp(t, 0, 100);
    if (t < 50)
        return {47 + (138 - 47) * t / 50, 111 + (99 - 111) * t / 50, 209 + (230 - 209) * t / 50};
    const int u = t - 50;
    return {138 + (229 - 138) * u / 50, 99 + (83 - 99) * u / 50, 230 + (75 - 230) * u / 50};
}

// Couleurs nommées : code 256 couleurs dans un terminal, case de la palette sur console.
struct Named {
    int code_256;
    int console_slot;
    Rgb rgb;
};
constexpr Named GREY{245, 8, {0x8A, 0x8A, 0x8A}};
constexpr Named RED{203, 9, {0xFF, 0x5F, 0x5F}};
constexpr Named GREEN{78, 10, {0x5F, 0xD7, 0x87}};
constexpr Named BLUE{75, 12, {0x5F, 0xAF, 0xFF}};
constexpr Named VIOLET{141, 13, {0xAF, 0x87, 0xFF}};

std::string paint(const ScreenState& state, const Named& color, const std::string& text) {
    const std::string code = state.console
                                 ? "\033[" + std::to_string(90 + color.console_slot - 8) + "m"
                                 : "\033[38;5;" + std::to_string(color.code_256) + "m";
    return code + text + RESET;
}

std::string gradient_code(const ScreenState& state, int t) {
    t = std::clamp(t, 0, 100);
    if (state.console)
        return "\033[" + std::to_string(31 + std::min(t * GRADIENT_STEPS / 100, GRADIENT_STEPS - 1)) + "m";
    const Rgb c = gradient(t);
    if (state.truecolor)
        return "\033[38;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
    return "\033[38;5;" + std::to_string(16 + 36 * (c.r * 5 / 255) + 6 * (c.g * 5 / 255) + c.b * 5 / 255) + "m";
}

bool is_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Largeur affichée : ignore les séquences de couleur et compte les caractères UTF-8.
int display_width(const std::string& text) {
    int width = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\033') {
            while (i < text.size() && text[i] != 'm')
                ++i;
            continue;
        }
        if (!is_continuation(text[i]))
            ++width;
    }
    return width;
}

// Tronque un texte sans couleur, en retirant les contrôles venus de l'extérieur.
std::string fit(const std::string& text, int max_width) {
    std::string out;
    int width = 0;
    for (const char byte : text) {
        const auto value = static_cast<unsigned char>(byte);
        const bool control = value < 0x20 || value == 0x7F;
        if (!is_continuation(byte)) {
            if (width == max_width)
                break;
            ++width;
        }
        out += control ? ' ' : byte;
    }
    return out;
}

std::string repeat(const char* glyph, int count) {
    std::string out;
    for (int i = 0; i < count; ++i)
        out += glyph;
    return out;
}

std::string spaces(int count) {
    return std::string(static_cast<std::size_t>(std::max(0, count)), ' ');
}

std::string center(const std::string& text, int width) {
    return spaces((width - display_width(text)) / 2) + text;
}

// Dégradé en diagonale, par blocs de 3 caractères, comme le logo de scripts/install.sh.
std::string logo_row(const ScreenState& state, const std::string& row, int y) {
    std::string out;
    int x = 0;
    for (std::size_t i = 0; i < row.size();) {
        std::size_t next = i + 1;
        while (next < row.size() && is_continuation(row[next]))
            ++next;
        if (x % 3 == 0)
            out += gradient_code(state, x * 100 / LOGO_WIDTH + y * 4);
        // Sur console, les traits doubles de l'ombre font brouillon : seuls les blocs restent.
        if (state.console && row.compare(i, next - i, "█") != 0)
            out += ' ';
        else
            out.append(row, i, next - i);
        i = next;
        ++x;
    }
    return out + RESET;
}

// Les lignes de l'heure, centrées sous le logo pour que le dégradé reste continu.
std::vector<std::string> big_clock_rows(const std::tm& now) {
    const std::array<int, 4> digits{now.tm_hour / 10, now.tm_hour % 10, now.tm_min / 10, now.tm_min % 10};
    std::vector<std::string> rows;
    for (std::size_t y = 0; y < COLON.size(); ++y) {
        std::string row;
        for (std::size_t d = 0; d < digits.size(); ++d) {
            if (d == 2)
                row += COLON[y];
            row += DIGITS[static_cast<std::size_t>(digits[d])][y];
        }
        rows.push_back(std::move(row));
    }
    const int padding = (LOGO_WIDTH - display_width(rows.front())) / 2;
    for (std::string& row : rows)
        row = spaces(padding) + row;
    return rows;
}

std::string two_digits(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof buffer, "%02d", value);
    return buffer;
}

std::string clock_text(const std::tm& time) {
    return two_digits(time.tm_hour) + ":" + two_digits(time.tm_min);
}

std::string date_text(const std::tm& now) {
    return std::string(DAYS[static_cast<std::size_t>(now.tm_wday)]) + " " +
           std::to_string(now.tm_mday) + " " + MONTHS[static_cast<std::size_t>(now.tm_mon)];
}

std::string title_text(const ScreenState& state) {
    return state.all_consoles ? "Toutes les consoles sont verrouillées"
                              : "Cette console est verrouillée";
}

std::string field_line(const ScreenState& state, int width) {
    const std::string marker = state.console ? ">" : "›";
    const int room = width - 3; // marqueur, espace, curseur
    std::string content;
    if (state.echo) {
        std::string visible = state.echoed;
        while (display_width(visible) > room) {
            std::size_t cut = 1;
            while (cut < visible.size() && is_continuation(visible[cut]))
                ++cut;
            visible.erase(0, cut);
        }
        content = fit(visible, room);
    } else {
        content = repeat("•", std::min(static_cast<int>(state.hidden_cells), room));
    }
    std::string line = paint(state, BLUE, marker) + " " + BOLD + content + RESET;
    if (!state.checking)
        line += paint(state, VIOLET, "_");
    return line;
}

std::string status_line(const ScreenState& state, int width) {
    const int room = std::max(0, width - 4);
    if (state.checking) {
        const char* frame = state.console
                                ? CONSOLE_SPINNER[state.spinner_frame % CONSOLE_SPINNER.size()]
                                : SPINNER[state.spinner_frame % SPINNER.size()];
        return "  " + paint(state, VIOLET, frame) + " Vérification...";
    }
    switch (state.status_kind) {
    case StatusKind::Error:
        return "  " + paint(state, RED, (state.console ? "x " : "✘ ") + fit(state.status, room));
    case StatusKind::Info:
        return "  " + paint(state, BLUE, state.console ? ">" : "›") + " " + fit(state.status, room);
    case StatusKind::Success:
        return "  " + paint(state, GREEN, state.console ? "√" : "✔") + " " + fit(state.status, room);
    case StatusKind::None:
        break;
    }
    return "";
}

std::vector<std::string> full_layout(const ScreenState& state, const std::tm& now) {
    const char* top_left = state.console ? "┌" : "╭";
    const char* top_right = state.console ? "┐" : "╮";
    const char* bottom_left = state.console ? "└" : "╰";
    const char* bottom_right = state.console ? "┘" : "╯";
    const std::string side = paint(state, GREY, "│");
    const auto box_row = [&](const std::string& text) {
        return "  " + side + " " + text + spaces(BOX_WIDTH - 1 - display_width(text)) + side;
    };

    // Sur console, la dernière ligne du logo et de l'heure ne contient que de l'ombre : elle est vide.
    const std::size_t art_rows = state.console ? LOGO.size() - 1 : LOGO.size();

    std::vector<std::string> lines;
    for (std::size_t y = 0; y < art_rows; ++y)
        lines.push_back(center(logo_row(state, LOGO[y], static_cast<int>(y)), BLOCK_WIDTH));
    // Pas de gras sur console : il changerait la case de palette, donc la couleur.
    lines.push_back(center((state.console ? "" : BOLD) + gradient_code(state, 50) + "── & ──" + RESET, BLOCK_WIDTH));
    const std::vector<std::string> clock_rows = big_clock_rows(now);
    for (std::size_t y = 0; y < art_rows; ++y)
        lines.push_back(spaces((BLOCK_WIDTH - LOGO_WIDTH) / 2) +
                        logo_row(state, clock_rows[y], CLOCK_FIRST_Y + static_cast<int>(y)));
    lines.emplace_back();
    lines.push_back(center(paint(state, GREY, title_text(state)), BLOCK_WIDTH));
    lines.push_back(center(paint(state, GREY, fit(state.user + " · " + state.tty + " · " + date_text(now) +
                                                      " · verrouillé à " + clock_text(state.locked_at),
                                                  BLOCK_WIDTH)),
                           BLOCK_WIDTH));
    lines.emplace_back();
    lines.push_back("  " + paint(state, GREY, top_left + repeat("─", BOX_WIDTH) + top_right));
    lines.push_back(box_row(BOLD + fit(state.prompt, BOX_WIDTH - 2) + RESET));
    lines.push_back("  " + paint(state, GREY, "├" + repeat("─", BOX_WIDTH) + "┤"));
    lines.push_back(box_row(field_line(state, BOX_WIDTH - 2)));
    lines.push_back("  " + paint(state, GREY, bottom_left + repeat("─", BOX_WIDTH) + bottom_right));
    lines.push_back(status_line(state, BLOCK_WIDTH));
    lines.push_back("    " + paint(state, GREY, "Entrée : valider · Échap : effacer"));
    return lines;
}

std::vector<std::string> compact_layout(const ScreenState& state, TtySize size, const std::tm& now) {
    const int width = size.cols;
    std::vector<std::string> lines;
    lines.push_back(BOLD + paint(state, BLUE, fit("vkeep · " + clock_text(now), width)));
    lines.push_back(paint(state, GREY, fit(title_text(state), width)));
    lines.push_back(paint(state, GREY, fit(state.user + " · " + state.tty, width)));
    lines.push_back(BOLD + fit(state.prompt, width) + RESET);
    if (width >= 4)
        lines.push_back(field_line(state, std::min(width, BOX_WIDTH)));
    lines.push_back(status_line(state, width));

    // Sur un très petit terminal, le champ de saisie passe avant le reste.
    while (static_cast<int>(lines.size()) > size.rows && lines.size() > 1)
        lines.erase(lines.begin());
    return lines;
}

std::string palette_entry(int slot, Rgb color) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "\033]P%X%02X%02X%02X", static_cast<unsigned>(slot),
                  static_cast<unsigned>(color.r), static_cast<unsigned>(color.g),
                  static_cast<unsigned>(color.b));
    return buffer;
}

} // namespace

std::vector<std::string> render_rows(const ScreenState& state, TtySize size, const std::tm& now) {
    std::vector<std::string> lines = full_layout(state, now);
    int margin = 0;
    if (size.cols >= BLOCK_WIDTH && size.rows >= static_cast<int>(lines.size())) {
        margin = (size.cols - BLOCK_WIDTH) / 2;
    } else {
        lines = compact_layout(state, size, now);
    }

    const int top = std::max(0, (size.rows - static_cast<int>(lines.size())) / 2);
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string row = std::to_string(top + static_cast<int>(i) + 1);
        rows.push_back("\033[" + row + ";1H\033[2K\033[" + row + ";" + std::to_string(margin + 1) + "H" +
                       lines[i]);
    }
    return rows;
}

std::string console_palette() {
    std::string out;
    for (int step = 0; step < GRADIENT_STEPS; ++step)
        out += palette_entry(step + 1, gradient((2 * step + 1) * 100 / (2 * GRADIENT_STEPS)));
    for (const Named* color : {&GREY, &RED, &GREEN, &BLUE, &VIOLET})
        out += palette_entry(color->console_slot, color->rgb);
    return out;
}
