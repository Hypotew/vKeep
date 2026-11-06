#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <getopt.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "attempts.hpp"
#include "auth.hpp"
#include "camera.hpp"
#include "frontend.hpp"
#include "notify.hpp"
#include "tty.hpp"
#include "vt_guard.hpp"

namespace {

constexpr int RETRY_PAUSE_MS = 2000;
#ifndef VKEEP_VERSION
#define VKEEP_VERSION "dev"
#endif

void print_usage(std::ostream& out, const char* program) {
    out << "Utilisation : " << program << " [options]\n"
        << "Verrouille la console jusqu'à la saisie du mot de passe.\n\n"
        << "  -a, --all         verrouille toutes les consoles virtuelles et photographie chaque tentative\n"
        << "      --camera=DEV  webcam à utiliser avec -a (détectée automatiquement sinon)\n"
        << "      --test-notify envoie un message de test sur le webhook Discord configuré\n"
        << "  -v, --version     affiche la version\n"
        << "  -h, --help        affiche cette aide\n";
}

const passwd& current_user() {
    const passwd* entry = getpwuid(getuid());
    if (entry == nullptr)
        throw std::runtime_error("utilisateur courant inconnu");
    return *entry;
}

std::string duration_text(std::chrono::steady_clock::duration elapsed) {
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
    if (minutes < 1)
        return "moins d'une minute";
    if (minutes < 60)
        return std::to_string(minutes) + " min";
    const auto rest = minutes % 60;
    return std::to_string(minutes / 60) + " h " + (rest < 10 ? "0" : "") + std::to_string(rest);
}

std::string attempts_text(std::size_t count) {
    return count == 0 ? "aucune" : std::to_string(count);
}

NotifyConfig read_config(const std::string& home) {
    NotifyConfig config = load_notify_config(home);
    for (const std::string& warning : config.warnings)
        std::cerr << "vkeep : " << warning << '\n';
    return config;
}

int test_notify() {
    const NotifyConfig config = read_config(current_user().pw_dir);
    if (config.webhook.empty()) {
        std::cerr << "vkeep : aucun webhook configuré. Ajoute cette ligne dans " << config.path << " :\n"
                  << "  discord_webhook = https://discord.com/api/webhooks/...\n";
        return 1;
    }
    Notifier notifier(config.webhook, "/dev/null");
    Alert alert;
    alert.headline = "🔔 Test des alertes depuis **" + notifier.host() + "**";
    alert.title = "🔔 Test des alertes";
    alert.description = "Tout fonctionne : les alertes de cette machine arriveront dans ce salon.";
    alert.color = alert_color::VIOLET;
    alert.fields = {{"Machine", notifier.host()}};
    const int code = notifier.send_and_wait(alert);
    if (code == 0) {
        std::cout << "Message envoyé : il doit être arrivé sur ton salon Discord.\n";
        return 0;
    }
    std::cerr << "vkeep : échec de l'envoi" << (code == -1 ? " (curl est-il installé ?)" : " (curl : code " + std::to_string(code) + ")") << '\n';
    return 1;
}

int unlock_loop(Frontend& frontend, const std::string& user, const std::string& tty) {
    while (!frontend.hung_up()) {
        frontend.begin_attempt();
        // Une erreur interne garde l'écran verrouillé : quitter rendrait la main au shell.
        try {
            const AuthResult result = authenticate(user, tty, frontend);
            if (frontend.hung_up())
                break;
            if (result.outcome == AuthOutcome::Success) {
                frontend.report_success();
                return 0;
            }
            if (result.outcome == AuthOutcome::Denied)
                frontend.report_wrong_password();
            else
                frontend.report_error(result.detail);
            if (!frontend.prompted())
                frontend.pause(RETRY_PAUSE_MS); // PAM refuse sans rien demander : pas de boucle folle
        } catch (const std::exception&) {
            frontend.pause(RETRY_PAUSE_MS);
        }
    }
    return 1; // le terminal a disparu, et la session avec lui
}

int run(bool all_consoles, const std::string& chosen_camera) {
    if (!pam_service_available())
        throw std::runtime_error("service PAM « vkeep » introuvable, personne ne pourrait déverrouiller : lance sudo make install");

    const passwd& account = current_user();
    const std::string user = account.pw_name;
    const std::string home = account.pw_dir;
    const NotifyConfig config = read_config(home);
    Tty tty;
    if (all_consoles && !tty.is_virtual_console())
        throw std::runtime_error(tty.path() + " n'est pas une console virtuelle : -a ne peut pas tout verrouiller");

    prctl(PR_SET_DUMPABLE, 0); // ni core dump ni ptrace sur le mot de passe

    std::optional<VtGuard> guard;
    if (all_consoles)
        guard.emplace(tty.fd());

    // Avant de verrouiller : une caméra lente à répondre ne doit pas bloquer l'écran.
    bool photos_wanted = all_consoles;
    std::string camera = chosen_camera;
#ifdef VKEEP_TESTING
    // -a est refusé hors d'une vraie console : les tests en pseudo-terminal passent par ici.
    if (const char* test_camera = std::getenv("VKEEP_TEST_CAMERA")) {
        photos_wanted = true;
        camera = std::string(test_camera) == "auto" ? "" : test_camera;
    }
#endif
    if (photos_wanted && camera.empty())
        camera = find_camera();
    Notifier notifier(config.webhook, state_directory(home) + "/notify-errors.log");
    AttemptLog attempts(home, photos_wanted, camera, notifier);
    const auto locked_at = std::chrono::steady_clock::now();
    const std::string& host = notifier.host();
    const std::string scope = all_consoles ? "toutes les consoles"
                              : tty.path().starts_with("/dev/") ? tty.path().substr(5) : tty.path();
    Alert locked;
    locked.headline = "🔒 **" + host + "** est verrouillé";
    locked.title = "🔒 Session verrouillée";
    locked.color = alert_color::BLUE;
    locked.fields = {{"Machine", host}, {"Compte", user}, {"Consoles", scope}};
    notifier.send(locked, true);
    int status = 1;
    {
        Frontend frontend(tty, user, all_consoles, attempts);
        status = unlock_loop(frontend, user, tty.path());
    }
    attempts.finish();
    const std::string elapsed = duration_text(std::chrono::steady_clock::now() - locked_at);
    Alert ended;
    ended.fields = {{"Machine", host}, {"Durée", elapsed}, {"Tentatives", attempts_text(attempts.count())}};
    if (status == 0) {
        ended.headline = "🔓 **" + host + "** est déverrouillé";
        ended.title = "🔓 Session déverrouillée";
        ended.color = alert_color::GREEN;
    } else {
        ended.headline = "❔ Verrou de **" + host + "** interrompu";
        ended.title = "❔ Verrou interrompu";
        ended.description = "Le terminal a été fermé sans que le mot de passe soit saisi.";
        ended.color = alert_color::AMBER;
    }
    notifier.send(ended, true);
    notifier.poll();
    if (status == 0)
        tty.write_all(attempts.summary());
    return status;
}

} // namespace

int main(int argc, char** argv) {
    // Dès le départ, avant le verrou : écrire à un curl déjà sorti doit donner une erreur, pas tuer vkeep.
    std::signal(SIGPIPE, SIG_IGN);

    static const option options[] = {
        {"all", no_argument, nullptr, 'a'},
        {"camera", required_argument, nullptr, 'c'},
        {"test-notify", no_argument, nullptr, 'n'},
        {"version", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    bool all_consoles = false;
    bool notify_test = false;
    std::string camera;
    int choice = 0;
    while ((choice = getopt_long(argc, argv, "ahv", options, nullptr)) != -1) {
        switch (choice) {
        case 'a':
            all_consoles = true;
            break;
        case 'c':
            camera = optarg;
            break;
        case 'n':
            notify_test = true;
            break;
        case 'v':
            std::cout << "vKeep " << VKEEP_VERSION << '\n';
            return 0;
        case 'h':
            print_usage(std::cout, argv[0]);
            return 0;
        default:
            print_usage(std::cerr, argv[0]);
            return 2;
        }
    }
    if (optind != argc || (!camera.empty() && !all_consoles)) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    if (notify_test && (all_consoles || !camera.empty())) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    try {
        if (notify_test)
            return test_notify();
        return run(all_consoles, camera);
    } catch (const std::exception& error) {
        std::cerr << argv[0] << " : " << error.what() << '\n';
        return 1;
    }
}
