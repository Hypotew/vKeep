# vKeep

Un verrou de console pour Linux, dans la lignée de `vlock` : logo en dégradé, grande horloge,
cadre et indicateur d'attente.

```
vkeep       verrouille la console courante
vkeep -a    verrouille toutes les consoles virtuelles
```

Il réutilise le service PAM `/etc/pam.d/vlock` installé par le paquet `kbd`. Il ne demande
aucun droit particulier : pas de setuid, et `pam_unix` passe par `unix_chkpwd`.

## Compilation et installation

```sh
make
sudo make install     # installe /usr/local/bin/vkeep
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
