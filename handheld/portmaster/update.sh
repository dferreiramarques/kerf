#!/bin/bash
# Kerf self-updater for the PortMaster package. Kerf.sh runs it before starting Kerf:
#
#   bash kerf/update.sh <kerf folder> <path of Kerf.sh> <program file name>
#
# With the handheld online, it asks GitHub for the handheld release (tag "muOS" of
# dferreiramarques/kerf), and when that holds a newer Kerf-handheld-<version>-portmaster-aarch64.zip
# than the installed program it downloads the zip, checks it against the release's SHA256SUMS file,
# checks that the new program starts (--version) and only then puts the new files in place.
#
# Never in the way: offline, GitHub down, a missing tool, a bad download or anything unexpected only
# writes a line to log.txt and Kerf starts as it is. Nothing of yours is touched: kerf.cfg, the logs,
# the samples (only a missing demo is put back) and the projects stay. The program it replaces is kept
# as <program>.prev. To turn the updater off, make an empty file called "no-update" in the kerf folder.

set -u

GAMEDIR="${1:?kerf folder}"
LAUNCHER="${2:-}"
BIN="${3:-kerf.aarch64}"

REPO="dferreiramarques/kerf"
TAG="muOS"
API="https://api.github.com/repos/$REPO/releases/tags/$TAG"
WORK="$GAMEDIR/.update"

log() { echo "update: $*"; }

finish() { rm -rf "$WORK"; exit 0; }

[ -e "$GAMEDIR/no-update" ] && { log "off (kerf/no-update exists)"; exit 0; }

case "$BIN" in
    *.aarch64) ;;
    *) log "only the aarch64 package is published; skipped for $BIN"; exit 0 ;;
esac

for tool in curl unzip sha256sum awk sed; do
    command -v "$tool" >/dev/null 2>&1 || { log "skipped: '$tool' not found on this firmware"; exit 0; }
done

# "a" newer than "b"? Versions are dotted numbers (0.5.0, 0.6.1 ...).
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        na = split(a, x, "."); nb = split(b, y, "."); n = na > nb ? na : nb
        for (i = 1; i <= n; i++) { p = x[i] + 0; q = y[i] + 0; if (p > q) exit 0; if (p < q) exit 1 }
        exit 1 }'
}

program_version() {   # program_version <program>: "0.5.0", or nothing when it does not start
    "$1" --version 2>/dev/null | sed -n 's/^Kerf \([0-9][0-9.]*\).*/\1/p' | head -n1
}

current="$(program_version "$GAMEDIR/$BIN")"
[ -n "$current" ] || { log "skipped: cannot tell the installed version ($BIN --version)"; exit 0; }

json="$(curl -fsSL --connect-timeout 3 --max-time 10 -H 'Accept: application/vnd.github+json' "$API" 2>/dev/null)" \
    || { log "no update check (offline or GitHub not reachable); Kerf $current"; exit 0; }

urls="$(printf '%s' "$json" | tr ',{}' '\n\n\n' | sed -n 's/.*"browser_download_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
# The release may hold packages of several versions: take the highest.
latest=""
zipurl=""
while IFS= read -r u; do
    v="$(printf '%s' "${u##*/}" | sed -n 's/^Kerf-handheld-\([0-9][0-9.]*\)-portmaster-aarch64\.zip$/\1/p')"
    [ -n "$v" ] || continue
    if [ -z "$latest" ] || newer "$v" "$latest"; then latest="$v"; zipurl="$u"; fi
done <<< "$urls"
zipname="${zipurl##*/}"

[ -n "$latest" ] || { log "no handheld package in the release '$TAG'; Kerf $current"; exit 0; }

# Its checksums: SHA256SUMS-<version>.txt, or a plain SHA256SUMS.txt.
sumurl="$(printf '%s\n' "$urls" | grep -E "/SHA256SUMS-${latest//./\\.}\.txt$" | head -n1)"
[ -n "$sumurl" ] || sumurl="$(printf '%s\n' "$urls" | grep -E '/SHA256SUMS\.txt$' | head -n1)"

if ! newer "$latest" "$current"; then
    log "Kerf $current is up to date (latest $latest)"
    exit 0
fi

[ -n "$sumurl" ] || { log "Kerf $latest is out, but the release has no SHA256SUMS file: not installed"; exit 0; }

log "Kerf $latest is out (installed: $current): downloading $zipname"
rm -rf "$WORK"
mkdir -p "$WORK" || { log "cannot make $WORK"; exit 0; }

curl -fsSL --connect-timeout 5 --max-time 180 -o "$WORK/$zipname" "$zipurl" 2>/dev/null \
    || { log "download failed: not installed"; finish; }
curl -fsSL --connect-timeout 5 --max-time 30 -o "$WORK/sums.txt" "$sumurl" 2>/dev/null \
    || { log "checksum download failed: not installed"; finish; }

expected="$(tr -d '\r' < "$WORK/sums.txt" | awk -v n="$zipname" '{ f = $2; sub(/^\*/, "", f) } f == n { print $1; exit }')"
actual="$(sha256sum "$WORK/$zipname" | awk '{ print $1 }')"

if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
    log "checksum does not match (expected '${expected:-none}', got '$actual'): not installed"
    finish
fi

unzip -q -o "$WORK/$zipname" -d "$WORK/x" >/dev/null 2>&1 || { log "cannot unpack the zip: not installed"; finish; }
NEW="$WORK/x/kerf"
[ -f "$NEW/$BIN" ] && [ -f "$WORK/x/Kerf.sh" ] || { log "the zip is not a Kerf PortMaster package: not installed"; finish; }

chmod +x "$NEW/$BIN" 2>/dev/null
got="$(program_version "$NEW/$BIN")"
[ "$got" = "$latest" ] || { log "the new program does not start here (said '${got:-nothing}'): not installed"; finish; }

# Put each file in place with a copy next to it and a rename over the old one, so a file is either
# the old one or the new one, never half written.
put() {   # put <new file> <destination>
    mkdir -p "$(dirname "$2")" && cp "$1" "$2.new" && mv -f "$2.new" "$2"
}

cp -f "$GAMEDIR/$BIN" "$GAMEDIR/$BIN.prev" 2>/dev/null

failed=0
while IFS= read -r f; do
    f="${f#./}"
    case "$f" in
        samples/*) [ -e "$GAMEDIR/$f" ] && continue ;;   # your sounds stay; only a missing demo comes back
    esac
    put "$NEW/$f" "$GAMEDIR/$f" || { failed=1; log "could not write $f"; }
done < <(cd "$NEW" && find . -type f)

chmod +x "$GAMEDIR/$BIN" "$GAMEDIR/update.sh" 2>/dev/null

if [ "$failed" -ne 0 ]; then
    # Back to the program that worked; the other files are text and pictures.
    [ -f "$GAMEDIR/$BIN.prev" ] && cp -f "$GAMEDIR/$BIN.prev" "$GAMEDIR/$BIN"
    log "update to $latest incomplete: Kerf $current kept"
    finish
fi

# The launcher lives elsewhere on muOS (ROMS/PORTS). The running copy keeps reading the old file:
# the rename gives the new one a new name on disk.
if [ -n "$LAUNCHER" ] && [ -f "$LAUNCHER" ]; then
    if put "$WORK/x/Kerf.sh" "$LAUNCHER"; then
        chmod +x "$LAUNCHER" 2>/dev/null
    else
        log "could not update $LAUNCHER (the old one still works)"
    fi
fi

printf '%s\n' "$current -> $latest" > "$GAMEDIR/updated.txt"
log "updated Kerf $current -> $latest (the old program is kept as $BIN.prev)"
finish
