#pragma once

#include <string>

class Frontend;

enum class AuthOutcome { Success, Denied, Failed };

struct AuthResult {
    AuthOutcome outcome;
    std::string detail; // explication si outcome vaut Failed
};

// Sans service PAM, personne ne pourrait déverrouiller : à vérifier avant de verrouiller.
bool pam_service_available();

AuthResult authenticate(const std::string& user, const std::string& tty, Frontend& frontend);
