#!/usr/bin/env bash
# Installe vKeep, depuis les sources ou depuis une archive de release.
#
# Usage : scripts/install.sh [--no-camera] [--no-deps]
#   --no-camera   n'installe pas ffmpeg : vkeep -a fonctionnera, sans photo
#   --no-deps     n'installe aucun paquet (à toi de fournir ce qui manque)
#
# Ce que le script fait (rien d'autre) :
#   1. les paquets qui manquent : ffmpeg et, depuis les sources, le compilateur et les en-têtes PAM ;
#   2. depuis les sources : la compilation ;
#   3. /usr/local/bin/vkeep et /etc/pam.d/vkeep (PREFIX change le premier).
#
# scripts/uninstall.sh annule tout ça, sauf les paquets.
set -uo pipefail

CAMERA=1
DEPS=1
for arg in "$@"; do
    case "$arg" in
        --no-camera) CAMERA=0 ;;
        --no-deps) DEPS=0 ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Option inconnue : $arg (voir --help)" >&2; exit 2 ;;
    esac
done

if ((EUID == 0)); then
    echo "Lance ce script avec ton compte, sans sudo : il le demandera quand il en aura besoin." >&2
    exit 1
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_LOG_NAME=install
# shellcheck source=lib/ui.sh
. "$HERE/scripts/lib/ui.sh"

DESTDIR="${DESTDIR:-}"
PREFIX="${PREFIX:-/usr/local}"
BINARY="$HERE/build/vkeep"
FROM_SOURCE=0
[[ -f $HERE/src/main.cpp ]] && FROM_SOURCE=1
if ((!FROM_SOURCE)) && [[ ! -x $BINARY ]]; then
    echo "Ni sources ni binaire dans $HERE : archive incomplète ?" >&2
    exit 1
fi

FAMILY=""
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    FAMILY="${ID:-} ${ID_LIKE:-}"
fi

ui_banner "$( ((FROM_SOURCE)) && echo "Installation depuis les sources" || echo "Installation · $("$BINARY" --version 2>/dev/null || echo "version inconnue")")"
ui_steps 4

# --- 1. ta machine ------------------------------------------------------------------------
ui_step "Ta machine"
ui_kv "Système" "${PRETTY_NAME:-inconnu} · noyau $(uname -r)"
ui_kv "Source" "$( ((FROM_SOURCE)) && echo "compilation locale" || echo "binaire de l'archive")"
if ((CAMERA)); then
    if compgen -G "/dev/video*" >/dev/null; then
        ui_kv "Webcam" "détectée, photos avec vkeep -a"
    else
        ui_kv "Webcam" "aucune pour l'instant, vkeep -a marchera sans photo"
    fi
else
    ui_kv "Webcam" "non (--no-camera)"
fi

# --- 2. paquets ---------------------------------------------------------------------------
ui_step "Paquets"
MISSING=()
needs_build_tools() { ((FROM_SOURCE)); }
if needs_build_tools; then
    command -v make >/dev/null || MISSING+=(make)
    command -v g++ >/dev/null || command -v clang++ >/dev/null || MISSING+=(compilateur)
    [[ -f /usr/include/security/pam_appl.h ]] || MISSING+=(pam-headers)
fi
if ((CAMERA)) && ! command -v ffmpeg >/dev/null; then
    MISSING+=(ffmpeg)
fi

if ((${#MISSING[@]} == 0)); then
    ui_ok "Rien à installer"
elif ((!DEPS)); then
    ui_warn "Paquets non installés (--no-deps) : ${MISSING[*]}"
else
    PACKAGES=()
    for item in "${MISSING[@]}"; do
        case "$item:$FAMILY" in
            make:*) PACKAGES+=(make) ;;
            compilateur:*arch*) PACKAGES+=(gcc) ;;
            compilateur:*) PACKAGES+=(g++) ;;
            pam-headers:*arch*) PACKAGES+=(pam) ;;
            pam-headers:*) PACKAGES+=(libpam0g-dev) ;;
            ffmpeg:*) PACKAGES+=(ffmpeg) ;;
        esac
    done
    if [[ $FAMILY == *arch* ]]; then
        INSTALL=(sudo pacman -S --needed --noconfirm "${PACKAGES[@]}")
    elif [[ $FAMILY == *debian* || $FAMILY == *ubuntu* ]]; then
        INSTALL=(sudo apt-get install -y "${PACKAGES[@]}")
    else
        ui_err "Distribution non reconnue : installe à la main ${MISSING[*]}, puis relance avec --no-deps."
        exit 1
    fi
    ui_sudo "Les paquets s'installent avec les droits administrateur." || exit 1
    ui_note "${PACKAGES[*]}"
    if ! ui_run "Installation des paquets" "${INSTALL[@]}"; then
        # ffmpeg seul ne doit pas empêcher d'installer le verrou.
        if [[ ${MISSING[*]} == ffmpeg ]]; then
            ui_warn "ffmpeg non installé : vkeep -a marchera sans photo."
        else
            ui_err "Paquets introuvables ou refusés."
            exit 1
        fi
    fi
fi

# --- 3. compilation -----------------------------------------------------------------------
ui_step "Compilation"
if ((FROM_SOURCE)); then
    ui_run "Compilation de vKeep" make -C "$HERE" -j"$(nproc)" all || {
        ui_err "La compilation a échoué."
        exit 1
    }
else
    ui_ok "Rien à compiler, le binaire est fourni"
fi
if ! "$BINARY" --version >/dev/null 2>&1; then
    ui_err "Le binaire ne démarre pas sur ce système : installe depuis les sources."
    ui_note "git clone https://github.com/Hypotew/vKeep.git && vKeep/scripts/install.sh"
    exit 1
fi

# --- 4. installation ----------------------------------------------------------------------
ui_step "Installation"
ui_sudo "vKeep s'installe avec les droits administrateur." || exit 1
ui_run "$PREFIX/bin/vkeep" sudo install -Dm755 "$BINARY" "$DESTDIR$PREFIX/bin/vkeep" || exit 1
ui_run "/etc/pam.d/vkeep" sudo install -Dm644 "$HERE/pam/vkeep" "$DESTDIR/etc/pam.d/vkeep" || exit 1

# Sans le module de mot de passe, personne ne pourrait déverrouiller : vkeep -a ne le voit qu'à l'usage.
if ! compgen -G "/usr/lib*/security/pam_unix.so" >/dev/null \
        && ! compgen -G "/lib*/security/pam_unix.so" >/dev/null \
        && ! compgen -G "/usr/lib/*/security/pam_unix.so" >/dev/null \
        && ! compgen -G "/lib/*/security/pam_unix.so" >/dev/null; then
    ui_warn "Module pam_unix introuvable : teste le déverrouillage dans un terminal avant de t'en servir."
fi
[[ -z $DESTDIR ]] && ! command -v vkeep >/dev/null && ui_warn "$PREFIX/bin n'est pas dans ton PATH."

# --- récapitulatif ------------------------------------------------------------------------
LINES_OUT=("${GREEN}✔${R} vKeep est installé." "" "vkeep      verrouille cette console" "vkeep -a   verrouille toutes les consoles et photographie" "           les tentatives" "" "Essaie d'abord vkeep dans un terminal : au pire, ferme-le.")
for warning in "${UI_WARNINGS[@]+"${UI_WARNINGS[@]}"}"; do
    LINES_OUT+=("${YELLOW}!${R} $warning")
done
ui_box "C'est prêt" "${LINES_OUT[@]}"
