# Stock maps' showcase files

The showcase screensaver's camera paths for Halo's 13 stock multiplayer maps (`<map>.txt`, made in Halo Map Porter's
Level window). The release package carries them:

    python3 port/oxbox/tools/mkpackage.py ... --showcase port/oxbox/showcase

and the installer writes them to `maps\showcase\` in the player's Halo folder, on installs and updates, leaving any
file the player has changed or deleted as they left it (`installer/main.c` install_showcase, `opence_showcase.txt`).

mkpackage.py packages every `.txt` in this folder, each of which must be named after a map, plain ASCII and at most
64 KB: notes about them go in this `.md` file, not in a `.txt`.
