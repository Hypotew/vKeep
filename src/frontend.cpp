#include "frontend.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <system_error>

#include <pthread.h>
#include <sys/mman.h>

namespace {

constexpr const char* ENTER_SCREEN = "\033[?1049h\033[?25l";
constexpr const char* LEAVE_SCREEN = "\033[0m\033[?25h\033[?1049l";
constexpr const char* RESET_CONSOLE_PALETTE = "\033]R";
constexpr int TICK_MS = 1000;
constexpr auto SPINNER_PERIOD = std::chrono::milliseconds(100);
constexpr auto SUCCESS_PAUSE = std::chrono::milliseconds(400);

volatile sig_atomic_t hangup_requested = 0;
volatile sig_atomic_t redraw_requested = 0;

void on_hangup(int) {
    hangup_requested = 1;
}

void on_resize(int) {
    redraw_requested = 1;
}

void set_handler(int signum, void (*handler)(int)) {
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    sigaction(signum, &action, nullptr);
}

std::string localized_prompt(const char* prompt) {
    std::string text = prompt != nullptr ? prompt : "";
    while (!text.empty() && (text.back() == ' ' || text.back() == ':'))
        text.pop_back();
    return text.empty() || text == "Password" ? "Mot de passe" : text;
}

} // namespace

Frontend::Frontend(Tty& tty, const std::string& user, bool all_consoles) : tty_(tty) {
    for (const int signum : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGTERM, SIGPIPE})
        set_handler(signum, SIG_IGN);
    set_handler(SIGHUP, on_hangup);
    set_handler(SIGWINCH, on_resize);

    // Au mieux : éviter que le mot de passe parte dans le swap.
    mlock(secret_.data(), secret_.size());

    state_.user = user;
    const std::string& path = tty.path();
    state_.tty = path.starts_with("/dev/") ? path.substr(5) : path;
    state_.all_consoles = all_consoles;
    state_.console = tty.is_virtual_console();
    const char* colorterm = std::getenv("COLORTERM");
    state_.truecolor = colorterm != nullptr && (std::strcmp(colorterm, "truecolor") == 0 ||
                                                std::strcmp(colorterm, "24bit") == 0);
    const std::time_t started = std::time(nullptr);
    localtime_r(&started, &state_.locked_at);
    state_.prompt = "Mot de passe";
    state_.checking = true;

    tty_.enter_raw_mode();
    tty_.write_all(ENTER_SCREEN);
    if (state_.console)
        tty_.write_all(console_palette());
    draw();
}

Frontend::~Frontend() {
    stop_spinner();
    clear_secret();
    tty_.write_all(LEAVE_SCREEN);
    if (state_.console)
        tty_.write_all(RESET_CONSOLE_PALETTE);
}

bool Frontend::hung_up() const {
    return hangup_requested != 0;
}

void Frontend::draw() {
    const std::time_t timestamp = std::time(nullptr);
    std::tm now{};
    localtime_r(&timestamp, &now);

    const TtySize size = tty_.size();
    std::vector<std::string> rows = render_rows(state_, size, now);

    // Tout est repeint chaque minute : un message du système a pu salir l'écran.
    const bool repaint = redraw_requested != 0 || now.tm_min != last_minute_ ||
                         size.rows != last_size_.rows || size.cols != last_size_.cols ||
                         rows.size() != last_rows_.size();
    std::string output = repaint ? "\033[0m\033[2J" : "";
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (repaint || rows[i] != last_rows_[i])
            output += rows[i];
    if (output.empty())
        return;

    redraw_requested = 0;
    last_minute_ = now.tm_min;
    last_size_ = size;
    last_rows_ = std::move(rows);
    if (!tty_.write_all(output))
        hangup_requested = 1;
}

void Frontend::start_spinner() {
    try {
        spinner_ = std::jthread([this](std::stop_token stop) {
            sigset_t all;
            sigfillset(&all);
            pthread_sigmask(SIG_BLOCK, &all, nullptr); // les signaux restent au fil principal
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(SPINNER_PERIOD);
                ++state_.spinner_frame;
                try {
                    draw();
                } catch (const std::exception&) {
                    return;
                }
            }
        });
    } catch (const std::system_error&) {
        // Sans fil disponible, l'indicateur reste simplement immobile.
    }
}

void Frontend::stop_spinner() {
    if (spinner_.joinable()) {
        spinner_.request_stop();
        spinner_.join();
    }
}

void Frontend::clear_secret() {
    explicit_bzero(secret_.data(), secret_.size());
    secret_length_ = 0;
    parser_ = Parser::Normal;
    sync_field();
}

void Frontend::sync_field() {
    std::size_t cells = 0;
    for (std::size_t i = 0; i < secret_length_; ++i)
        if ((static_cast<unsigned char>(secret_[i]) & 0xC0) != 0x80)
            ++cells;
    state_.hidden_cells = state_.echo ? 0 : cells;
    state_.echoed = state_.echo ? std::string(secret_.data(), secret_length_) : std::string();
}

bool Frontend::feed(char byte) {
    const auto value = static_cast<unsigned char>(byte);

    switch (parser_) {
    case Parser::Escape:
        if (byte == '[')
            parser_ = Parser::Csi;
        else if (byte == 'O')
            parser_ = Parser::Ss3;
        else
            parser_ = Parser::Normal; // Alt + touche
        return false;
    case Parser::Csi:
        if (byte == '[')
            parser_ = Parser::ConsoleFunctionKey;
        else if (value >= 0x40 && value <= 0x7E)
            parser_ = Parser::Normal;
        return false;
    case Parser::ConsoleFunctionKey:
    case Parser::Ss3:
        parser_ = Parser::Normal;
        return false;
    case Parser::Normal:
        break;
    }

    if (byte == '\r' || byte == '\n')
        return secret_length_ > 0; // une saisie vide ne doit pas compter comme un échec
    if (byte == '\033') {
        parser_ = Parser::Escape;
    } else if (byte == 0x7F || byte == '\b') {
        while (secret_length_ > 0) {
            const char removed = secret_[--secret_length_];
            secret_[secret_length_] = '\0';
            if ((static_cast<unsigned char>(removed) & 0xC0) != 0x80)
                break;
        }
    } else if (byte == 0x15) { // Ctrl-U
        clear_secret();
    } else if (value >= 0x20 && secret_length_ < SECRET_CAPACITY - 1) {
        secret_[secret_length_++] = byte;
    }
    return false;
}

char* Frontend::ask(const char* prompt, bool echo) {
    stop_spinner();
    prompted_ = true;
    tty_.flush_input(); // ce qui a été tapé pendant la vérification ne compte pas
    state_.prompt = localized_prompt(prompt);
    state_.echo = echo;
    state_.checking = false;
    clear_secret();

    std::array<char, 64> chunk{};
    for (;;) {
        if (hung_up())
            return nullptr;
        draw();

        std::size_t count = 0;
        switch (tty_.read_input(chunk.data(), chunk.size(), TICK_MS, count)) {
        case TtyRead::Timeout:
        case TtyRead::Interrupted:
            continue;
        case TtyRead::Hangup:
            hangup_requested = 1;
            return nullptr;
        case TtyRead::Data:
            break;
        }

        bool submitted = false;
        for (std::size_t i = 0; i < count && !submitted; ++i)
            submitted = feed(chunk[i]);
        explicit_bzero(chunk.data(), chunk.size());

        if (submitted) {
            char* answer = static_cast<char*>(std::malloc(secret_length_ + 1));
            if (answer != nullptr) {
                std::memcpy(answer, secret_.data(), secret_length_);
                answer[secret_length_] = '\0';
            }
            state_.echo = false;
            state_.checking = true;
            state_.status_kind = StatusKind::None;
            clear_secret();
            draw();
            start_spinner();
            return answer;
        }

        // Une touche Échap seule arrive sans suite dans le même bloc.
        if (parser_ == Parser::Escape)
            clear_secret();
        sync_field();
    }
}

void Frontend::notice(const char* message, bool is_error) {
    stop_spinner();
    state_.checking = false; // sinon « Vérification... » masquerait le message
    state_.status_kind = is_error ? StatusKind::Error : StatusKind::Info;
    state_.status = message != nullptr ? message : "";
    draw();
}

void Frontend::begin_attempt() {
    prompted_ = false;
}

void Frontend::report_wrong_password() {
    stop_spinner();
    ++failures_;
    state_.checking = false;
    state_.status_kind = StatusKind::Error;
    state_.status = failures_ == 1
                        ? "Mot de passe incorrect"
                        : "Mot de passe incorrect (" + std::to_string(failures_) + " échecs)";
    draw();
}

void Frontend::report_error(const std::string& message) {
    stop_spinner();
    state_.checking = false;
    state_.status_kind = StatusKind::Error;
    state_.status = message;
    draw();
}

void Frontend::report_success() {
    stop_spinner();
    state_.checking = false;
    state_.status_kind = StatusKind::Success;
    state_.status = "Déverrouillé";
    draw();
    std::this_thread::sleep_for(SUCCESS_PAUSE);
}

void Frontend::pause(int milliseconds) {
    stop_spinner();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    std::array<char, 64> chunk{};
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0 || hung_up())
            break;
        draw();

        std::size_t count = 0;
        const int wait = left.count() < TICK_MS ? static_cast<int>(left.count()) : TICK_MS;
        if (tty_.read_input(chunk.data(), chunk.size(), wait, count) == TtyRead::Hangup)
            hangup_requested = 1;
    }
    explicit_bzero(chunk.data(), chunk.size());
}
