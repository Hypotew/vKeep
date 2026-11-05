# shellcheck shell=bash
# Affichage des scripts d'installation et de désinstallation : logo, étapes, indicateur
# d'attente, récapitulatif. Hors terminal (CI, redirection, NO_COLOR) : texte simple.

UI_FANCY=0
if [[ -t 1 && -z ${NO_COLOR:-} && ${TERM:-dumb} != dumb ]] && [[ $(locale charmap 2>/dev/null) == UTF-8 ]]; then
    UI_FANCY=1
fi
UI_TRUECOLOR=0
[[ ${COLORTERM:-} == truecolor || ${COLORTERM:-} == 24bit ]] && UI_TRUECOLOR=1

if ((UI_FANCY)); then
    B=$'\e[1m' R=$'\e[0m'
    RED=$'\e[38;5;203m' GREEN=$'\e[38;5;78m' YELLOW=$'\e[38;5;221m'
    BLUE=$'\e[38;5;75m' VIOLET=$'\e[38;5;141m' GREY=$'\e[38;5;245m'
else
    B='' R='' RED='' GREEN='' YELLOW='' BLUE='' VIOLET='' GREY=''
fi

UI_STEP=0
UI_STEPS=0
UI_WARNINGS=()
UI_LOG="${XDG_CACHE_HOME:-$HOME/.cache}/vkeep/${UI_LOG_NAME:-script}-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$UI_LOG")"
: >"$UI_LOG"

ui_cols() { local c; c=$(tput cols 2>/dev/null) || c=80; echo "${c:-80}"; }

# Couleur d'un point du dégradé (0 → 100) : bleu → violet → corail, comme l'écran de verrouillage.
_ui_grad() {
    local t=$1 r g b u
    if ((t < 50)); then
        r=$((47 + (138 - 47) * t / 50)); g=$((111 + (99 - 111) * t / 50)); b=$((209 + (230 - 209) * t / 50))
    else
        u=$((t - 50))
        r=$((138 + (229 - 138) * u / 50)); g=$((99 + (83 - 99) * u / 50)); b=$((230 + (75 - 230) * u / 50))
    fi
    if ((UI_TRUECOLOR)); then
        printf '\e[38;2;%d;%d;%dm' "$r" "$g" "$b"
    else
        printf '\e[38;5;%dm' $((16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + b * 5 / 255))
    fi
}

_UI_LOGO=(
"██╗   ██╗██╗  ██╗███████╗███████╗██████╗ "
"██║   ██║██║ ██╔╝██╔════╝██╔════╝██╔══██╗"
"██║   ██║█████╔╝ █████╗  █████╗  ██████╔╝"
"╚██╗ ██╔╝██╔═██╗ ██╔══╝  ██╔══╝  ██╔═══╝ "
" ╚████╔╝ ██║  ██╗███████╗███████╗██║     "
"  ╚═══╝  ╚═╝  ╚═╝╚══════╝╚══════╝╚═╝     "
)

# ui_banner "Installation" : logo en dégradé, ou titre simple si le terminal est étroit.
ui_banner() {
    local subtitle=$1 width=41 pad=2 y x t line out
    if ((!UI_FANCY)); then
        printf '\n== vKeep : %s ==\n\n' "$subtitle"
        return
    fi
    printf '\n'
    if (($(ui_cols) < width + 2 * pad)); then
        printf '  %s%svKeep%s\n  %s%s%s\n\n' "$B" "$BLUE" "$R" "$GREY" "$subtitle" "$R"
        return
    fi
    for ((y = 0; y < ${#_UI_LOGO[@]}; y++)); do
        line=${_UI_LOGO[y]}
        out=""
        for ((x = 0; x < ${#line}; x += 3)); do
            t=$((x * 100 / width + y * 4))
            out+="$(_ui_grad $((t > 100 ? 100 : t)))${line:x:3}"
        done
        printf '%*s%s%s\n' "$pad" '' "$out" "$R"
    done
    printf '\n%*s%s%s%s\n\n' $((pad + (width - ${#subtitle}) / 2)) '' "$GREY" "$subtitle" "$R"
}

ui_steps() { UI_STEPS=$1; }

ui_step() {
    UI_STEP=$((UI_STEP + 1))
    local title=$1 width rest
    printf '\n## %s\n' "$title" >>"$UI_LOG"
    if ((UI_FANCY)); then
        width=$(($(ui_cols) - 2))
        ((width > 68)) && width=68
        rest=$((width - ${#title} - ${#UI_STEP} - ${#UI_STEPS} - 5))
        ((rest < 3)) && rest=3
        printf '\n%s%s[%d/%d]%s %s%s%s %s%s%s\n' "$B" "$VIOLET" "$UI_STEP" "$UI_STEPS" "$R" "$B" "$title" "$R" \
            "$GREY" "$(printf '─%.0s' $(seq 1 "$rest"))" "$R"
    else
        printf '\n[%d/%d] %s\n' "$UI_STEP" "$UI_STEPS" "$title"
    fi
}

ui_ok()   { printf '  %s✔%s %s\n' "$GREEN" "$R" "$*"; }
ui_info() { printf '  %s›%s %s\n' "$BLUE" "$R" "$*"; }
ui_note() { printf '    %s%s%s\n' "$GREY" "$*" "$R"; }
ui_err()  { printf '  %s✘ %s%s\n' "$RED" "$*" "$R" >&2; }
# Avertissement : affiché tout de suite et repris dans le récapitulatif.
ui_warn() {
    printf '  %s!%s %s\n' "$YELLOW" "$R" "$*" >&2
    UI_WARNINGS+=("$*")
}
# Remplissage à la main : printf compte les octets, et « è » en fait deux.
ui_kv() { printf '  %s%s%*s%s %s\n' "$GREY" "$1" $((14 - ${#1})) '' "$R" "$2"; }

_ui_job=""
_ui_cleanup() {
    [[ -n $_ui_job ]] && kill "$_ui_job" 2>/dev/null
    ((UI_FANCY)) && tput cnorm 2>/dev/null
    true
}
trap _ui_cleanup EXIT
trap 'printf "\n"; ui_err "Interrompu."; exit 130' INT

# ui_run "Libellé" commande args… : la sortie va au journal, sa fin s'affiche en cas d'échec.
ui_run() {
    local label=$1; shift
    printf '\n$ %s\n' "$*" >>"$UI_LOG"
    local from rc frames='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏' i=0 pid
    from=$(($(wc -l <"$UI_LOG") + 1))
    if ((!UI_FANCY)); then
        printf '  › %s\n' "$label"
        "$@" >>"$UI_LOG" 2>&1 </dev/null
        rc=$?
    else
        "$@" >>"$UI_LOG" 2>&1 </dev/null &
        pid=$!
        _ui_job=$pid
        tput civis 2>/dev/null
        while kill -0 "$pid" 2>/dev/null; do
            printf '\r  %s%s%s %s\e[K' "$VIOLET" "${frames:i++%10:1}" "$R" "$label"
            sleep 0.1
        done
        wait "$pid"
        rc=$?
        _ui_job=""
        tput cnorm 2>/dev/null
        printf '\r\e[K'
    fi
    if ((rc == 0)); then
        ui_ok "$label"
    else
        printf '  %s✘%s %s\n' "$RED" "$R" "$label"
        tail -n +"$from" "$UI_LOG" | tail -n 12 | cut -c1-$(($(ui_cols) - 10)) | sed "s/^/      ${GREY}│${R} /"
        printf '      %sJournal complet : %s%s\n' "$GREY" "${UI_LOG/#$HOME/\~}" "$R"
    fi
    return $rc
}

# ui_sudo "raison" : demande le mot de passe une seule fois, avant les commandes silencieuses.
ui_sudo() {
    sudo -n true 2>/dev/null && return 0
    ui_info "$1"
    sudo -v -p "  ${YELLOW}?${R} Mot de passe administrateur : "
}

# ui_confirm "Question" : oui avec UI_YES=1, sinon demande (non par défaut).
ui_confirm() {
    ((${UI_YES:-0})) && return 0
    local answer
    read -r -p "  ${YELLOW}?${R} $1 ${GREY}[o/N]${R} " answer
    [[ $answer =~ ^[oOyY] ]]
}

# ui_box "Titre" ligne… : encadré final.
ui_box() {
    local title=$1 w=62 l plain; shift
    (($(ui_cols) - 4 < w)) && w=$(($(ui_cols) - 4))
    if ((!UI_FANCY)); then
        printf '\n== %s ==\n' "$title"
        for l in "$@"; do printf '  %s\n' "$l"; done
        printf '\n'
        return
    fi
    printf '\n  %s╭%s╮%s\n' "$GREY" "$(printf '─%.0s' $(seq 1 "$w"))" "$R"
    printf '  %s│%s %s%s%s%*s%s│%s\n' "$GREY" "$R" "$B" "$title" "$R" $((w - 1 - ${#title})) '' "$GREY" "$R"
    printf '  %s├%s┤%s\n' "$GREY" "$(printf '─%.0s' $(seq 1 "$w"))" "$R"
    for l in "$@"; do
        plain=$(sed 's/\x1b\[[0-9;]*m//g' <<<"$l")
        if ((${#plain} < w)); then
            printf '  %s│%s %s%*s%s│%s\n' "$GREY" "$R" "$l" $((w - 1 - ${#plain})) '' "$GREY" "$R"
        else
            printf '  %s│%s %s\n' "$GREY" "$R" "$l"
        fi
    done
    printf '  %s╰%s╯%s\n\n' "$GREY" "$(printf '─%.0s' $(seq 1 "$w"))" "$R"
}
