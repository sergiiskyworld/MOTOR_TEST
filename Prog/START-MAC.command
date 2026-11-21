#!/bin/bash
# Starting the thrust stand on macOS — double-click this file.
#
# A Terminal window opens and shows the startup progress; once the stand
# window appears, it closes by itself.
#
# There is deliberately no application bundle (.app) here. macOS does not let
# a bundle that lives in ~/Documents run a script next to it: startup failed
# with "Operation not permitted" and did not work without granting access to
# the Documents folder in the privacy settings. A .command file is run by
# Terminal, which already has that access.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/launcher/start-stand.sh"
