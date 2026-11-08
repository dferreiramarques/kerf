#!/bin/bash
# Kerf launcher for PortMaster (aarch64 handhelds such as the Anbernic RG35XX H).
# Started from the Ports list of muOS 2606.0 on an RG35XX H (version 0.1.0); other firmwares are untested.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

# Where this launcher is (on muOS: ROMS/PORTS), so the updater can replace it too.
LAUNCHER="$(readlink -f "$0" 2>/dev/null || echo "$0")"

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR=/$directory/ports/kerf
cd "$GAMEDIR" || exit 1

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Kerf reads the pad through SDL itself, so gptokeyb is not started: by default it would add
# keyboard keys on top of the pad, and Kerf also listens to the keyboard.
# The way out is built into Kerf: hold SELECT + START for one second.

chmod +x "$GAMEDIR/kerf.${DEVICE_ARCH}" 2>/dev/null

# Online: a newer Kerf from the GitHub release is checked, downloaded and put in place first
# (kerf/update.sh; an empty file kerf/no-update turns it off). Offline it gives up after 3 seconds.
if [ -f "$GAMEDIR/update.sh" ]; then
  type pm_message >/dev/null 2>&1 && pm_message "Kerf: checking for updates..."
  bash "$GAMEDIR/update.sh" "$GAMEDIR" "$LAUNCHER" "kerf.${DEVICE_ARCH}"
  chmod +x "$GAMEDIR/kerf.${DEVICE_ARCH}" 2>/dev/null
fi

pm_platform_helper "$GAMEDIR/kerf.${DEVICE_ARCH}"
"./kerf.${DEVICE_ARCH}" --fullscreen

pm_finish
