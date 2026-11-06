#!/usr/bin/env bash
# Désinstalle vKeep.
#
# Usage : scripts/uninstall.sh [--purge] [--yes]
#   --purge   supprime aussi les photos, le journal des tentatives et la configuration des alertes
#   --yes     ne demande pas de confirmation pour --purge
#
# Les paquets installés (ffmpeg, compilateur) restent : d'autres programmes s'en servent peut-être.
set -uo pipefail

PURGE=0
UI_YES=0
for arg in "$@"; do
    case "$arg" in
        --purge) PURGE=1 ;;
        --yes) UI_YES=1 ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Option inconnue : $arg (voir --help)" >&2; exit 2 ;;
    esac
done

if ((EUID == 0)); then
    echo "Lance ce script avec ton compte, sans sudo : il le demandera quand il en aura besoin." >&2
    exit 1
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_LOG_NAME=uninstall
# shellcheck source=lib/ui.sh
. "$HERE/scripts/lib/ui.sh"

DESTDIR="${DESTDIR:-}"
PREFIX="${PREFIX:-/usr/local}"
TARGETS=("$DESTDIR$PREFIX/bin/vkeep" "$DESTDIR/etc/pam.d/vkeep")
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/vkeep"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vkeep"

ui_banner "Désinstallation"
ui_steps 3

ui_step "Fichiers de vKeep"
EXISTING=()
for target in "${TARGETS[@]}"; do
    [[ -e $target ]] && EXISTING+=("$target")
done
if ((${#EXISTING[@]} == 0)); then
    ui_ok "Déjà désinstallé"
else
    ui_sudo "La suppression demande les droits administrateur." || exit 1
    for target in "${EXISTING[@]}"; do
        ui_run "Suppression de ${target#"$DESTDIR"}" sudo rm -f "$target" || exit 1
    done
fi

ui_step "Photos et journal"
if [[ ! -d $STATE_DIR ]]; then
    ui_ok "Rien à supprimer"
elif ((PURGE)); then
    if ui_confirm "Supprimer définitivement $STATE_DIR ?"; then
        rm -rf -- "$STATE_DIR"
        ui_ok "Supprimés"
    else
        ui_info "Gardés dans $STATE_DIR"
    fi
else
    ui_info "Gardés dans $STATE_DIR (--purge pour les supprimer)"
fi

ui_step "Configuration des alertes"
if [[ ! -d $CONFIG_DIR ]]; then
    ui_ok "Rien à supprimer"
elif ((PURGE)); then
    if ui_confirm "Supprimer $CONFIG_DIR (et l'adresse de ton webhook) ?"; then
        rm -rf -- "$CONFIG_DIR"
        ui_ok "Supprimée"
    else
        ui_info "Gardée dans $CONFIG_DIR"
    fi
else
    ui_info "Gardée dans $CONFIG_DIR (--purge pour la supprimer)"
fi

ui_box "C'est fait" "${GREEN}✔${R} vKeep est désinstallé."
