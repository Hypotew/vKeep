#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "screen.hpp"
#include "tty.hpp"

// L'écran de verrouillage et sa saisie. Reste actif jusqu'à sa destruction.
class Frontend {
public:
    Frontend(Tty& tty, const std::string& user, bool all_consoles);
    ~Frontend();
    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    // Renvoie une réponse allouée avec malloc, comme l'attend PAM, ou nullptr si
    // le terminal a disparu.
    char* ask(const char* prompt, bool echo);
    void notice(const char* message, bool is_error);

    void begin_attempt();
    void report_wrong_password();
    void report_error(const std::string& message);
    void report_success(); // laisse le temps de voir la confirmation
    void pause(int milliseconds); // garde l'écran à jour et jette ce qui est tapé

    bool prompted() const { return prompted_; }
    bool hung_up() const;

private:
    enum class Parser { Normal, Escape, Csi, ConsoleFunctionKey, Ss3 };

    void draw();
    void start_spinner();
    void stop_spinner();
    void clear_secret();
    bool feed(char byte); // renvoie true quand la saisie est validée
    void sync_field();

    Tty& tty_;
    ScreenState state_;
    std::vector<std::string> last_rows_;
    TtySize last_size_{};
    int last_minute_ = -1;
    std::jthread spinner_; // n'écrit que pendant que le fil principal est bloqué dans PAM
    Parser parser_ = Parser::Normal;
    bool prompted_ = false;
    int failures_ = 0;

    static constexpr std::size_t SECRET_CAPACITY = 512;
    std::array<char, SECRET_CAPACITY> secret_{};
    std::size_t secret_length_ = 0;
};
