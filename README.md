# vKeep

Un verrou de console pour Linux, dans la lignée de `vlock` : logo en dégradé, grande horloge,
cadre et indicateur d'attente.

```
vkeep       verrouille la console courante
vkeep -a    verrouille toutes les consoles virtuelles et photographie chaque tentative
```

Un mauvais mot de passe ou, avec `-a`, un changement de console refusé compte comme une tentative.
Les tentatives sont listées au déverrouillage et notées dans `~/.local/state/vkeep/attempts.log`.

Avec `-a`, chaque tentative est aussi photographiée par la webcam, détectée au lancement. Si la
détection se trompe, par exemple sur une caméra infrarouge, choisis-la avec `--camera=/dev/videoN`.
Les photos sont rangées au même endroit, dans un dossier accessible à toi seul. Il faut `ffmpeg`, et
ton compte doit pouvoir lire la caméra. Sans cela, le verrou marche quand même, sans photo. Sans
`-a`, la caméra n'est jamais utilisée.

Photographier quelqu'un à son insu peut être illégal : préviens les personnes concernées, ou
n'utilise pas `-a` ailleurs que chez toi.

Il vérifie le mot de passe via son propre service PAM, `/etc/pam.d/vkeep`, et tourne sans droit
particulier : pas de setuid, et `pam_unix` passe par `unix_chkpwd`. Sans ce fichier, personne ne
pourrait déverrouiller : vKeep refuse donc de se lancer tant qu'il n'est pas installé.

## Compilation et installation

```sh
make
sudo make install     # installe /usr/local/bin/vkeep et /etc/pam.d/vkeep
sudo make uninstall
```

## Tests

```sh
make test       # pseudo-terminal, avec une pile PAM de test
make coverage   # les mêmes tests, avec échec sous 80 % de lignes couvertes
make vm-test    # vraies consoles dans une VM QEMU, avec le vrai pam_unix
```

`make coverage` utilise `gcovr`, lancé par `uv`. La CI lance `make test` et `make coverage`. `make vm-test` reste à lancer en local : il dépend du
noyau et de l'installation PAM de la machine, et c'est le seul à exécuter `src/vt_guard.cpp`.

`make vm-test` a besoin de QEMU et d'un noyau lisible dans `/boot/vmlinuz-linux`. Les captures
d'écran arrivent dans `build/vm-screens/`.

## Limites

Comme `vlock` :

- SysRq et Ctrl-Alt-Suppr restent actifs, y compris avec `-a`.
- Sans `-a`, les autres consoles restent accessibles.
- Si le processus meurt (par exemple `kill -9` par le même utilisateur), le shell reprend la main.
  Sur console, les couleurs du verrou restent alors en place jusqu'à un `printf '\033]R'`.
- Dans un émulateur de terminal, seul ce terminal est verrouillé, et `-a` est refusé.

## Licence

vKeep est distribué sous licence MIT : voir [LICENSE](LICENSE).
