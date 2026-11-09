#!/bin/sh
# Entry point of the packer image. Mount your game folder at /game (read-only is fine; with the Enhanced
# Edition's sh2e folder in it, which the port needs) and an empty folder at /out. The result is
# /out/sh2-vita/, whose contents go to ux0:data/sh2-vita/ on the Vita.
# If a run stops partway, the same command picks up where it stopped; set FRESH=1 to start over.
set -e
if [ ! -f /game/sh2pc.exe ] || [ ! -d /game/data ]; then
	echo "Mount your Silent Hill 2 folder at /game: it must hold sh2pc.exe and the data folder." >&2
	echo '  docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/out:/out" sh2-vita-packer' >&2
	exit 2
fi
if [ ! -d /game/sh2e ]; then
	echo "warning: no sh2e folder in the game folder: the Enhanced Edition is required (README.md)" >&2
fi
OUT=/out/sh2-vita
echo "sh2-vita packer $(cat /opt/sh2-vita/VERSION), writing $OUT"
/opt/sh2-vita/tools/make_pack.sh /game "$OUT" --strip-extras
echo "== textures in the PS Vita's form"
python3 /opt/sh2-vita/tools/sh2conv/vitatex.py "$OUT"
echo
echo "Done. Copy the contents of sh2-vita/ in your output folder to ux0:data/sh2-vita/ on the Vita."
du -sh "$OUT" 2>/dev/null || true
