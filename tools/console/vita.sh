#!/bin/bash
# Puts a build on a PS Vita over Wi-Fi and gets its logs back (port/vita/README.md, "On the console").
#
#   tools/console/vita.sh ip <address>   remember the console's address (or set VITA_IP)
#   tools/console/vita.sh install        replace the installed game's eboot.bin with the one just built
#   tools/console/vita.sh vpk            upload the whole VPK to ux0:data, to install it in VitaShell
#   tools/console/vita.sh pull [dir]     download the logs (default .lr3ds/console/vita-<date-time>,
#                                        and .lr3ds/console/vita-latest points at it)
#   tools/console/vita.sh put <file> [path]  upload a file (default the pack's ui/, ux0:/data/sh23ds/ui/)
#   tools/console/vita.sh ls [path]      list a folder (default the pack, ux0:/data/sh23ds/)
#   tools/console/vita.sh get <path> [dir]   download one file, e.g. ux0:/data/psp2core-x.psp2dmp
#   tools/console/vita.sh run | stop     start or close the game (vitacompanion only)
#
# The FTP server is VitaShell's (SELECT, once START > Main settings > SELECT button is set to FTP; stay
# in it) or the vitacompanion plugin's,
# which is always on, even in game. Both use port 1337 (VITA_FTP_PORT). With vitacompanion, install
# also closes the game first, without saving, and starts it again after.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TITLE=SHIL00002
PACK=ux0:/data/sh23ds
BUILD=$ROOT/port/vita
IP_FILE=$ROOT/.lr3ds/vita_ip
PORT=${VITA_FTP_PORT:-1337}
LOGS="vita_log.txt perf.txt stall.txt mem.txt movie_log.txt ptlog.txt enemylog.txt autotest.txt options.txt debug.txt"

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
case "$1" in
ip|install|vpk|put|pull|ls|get|run|stop) ;;
*) usage ;;
esac

if [ "$1" = ip ]; then
	[ -n "$2" ] || { echo "usage: $0 ip <address>" >&2; exit 2; }
	mkdir -p "$(dirname "$IP_FILE")"
	echo "$2" > "$IP_FILE"
	echo "console address: $2"
	exit
fi

IP=${VITA_IP:-$(cat "$IP_FILE" 2>/dev/null || true)}
[ -n "$IP" ] || { echo "no address yet: $0 ip <address> (VitaShell shows it after SELECT)" >&2; exit 2; }
URL=ftp://$IP:$PORT

# nocwd: the path goes whole in one command, as vitacompanion's README does it
ftp() { curl -sS --connect-timeout 5 --ftp-method nocwd "$@"; }

need_ftp() {
	curl -s --connect-timeout 5 -o /dev/null "$URL/" && return
	echo "no FTP at $IP:$PORT: open VitaShell and press SELECT (with its SELECT button set to FTP), or check that vitacompanion is on" >&2
	exit 1
}

# vitacompanion's command port; fails quietly when there is no vitacompanion
send() { timeout 3 bash -c 'printf "%s\n" "$2" > "/dev/tcp/$1/1338"' _ "$IP" "$1" 2>/dev/null; }

case "$1" in
install)
	[ -f "$BUILD/eboot.bin" ] || { echo "no eboot.bin: run port/vita/build.sh first" >&2; exit 1; }
	need_ftp
	curl -sS --connect-timeout 5 "$URL/ux0:/app/" | grep -q "$TITLE" || {
		echo "$TITLE is not installed: the first time needs the VPK ($0 vpk)" >&2
		exit 1
	}
	if send destroy; then
		companion=1
		sleep 2   # the game has to let go of its eboot
	fi
	ftp -T "$BUILD/eboot.bin" "$URL/ux0:/app/$TITLE/eboot.bin"
	# a cut-off eboot would leave the game unable to start, so read it back
	back=$(mktemp)
	trap 'rm -f "$back"' EXIT
	ftp "$URL/ux0:/app/$TITLE/eboot.bin" -o "$back"
	cmp -s "$BUILD/eboot.bin" "$back" || { echo "eboot.bin on the console differs: run install again" >&2; exit 1; }
	echo "eboot.bin installed and read back, $(stat -c %y "$BUILD/eboot.bin" | cut -d. -f1)"
	if [ -n "$companion" ]; then
		send "launch $TITLE"
		echo "game started"
	fi
	;;
vpk)
	[ -f "$BUILD/sh2vita.vpk" ] || { echo "no sh2vita.vpk: run port/vita/build.sh first" >&2; exit 1; }
	need_ftp
	ftp -T "$BUILD/sh2vita.vpk" "$URL/ux0:/data/sh2vita.vpk"
	echo "in ux0:data/sh2vita.vpk: open it in VitaShell to install it"
	;;
put)
	[ -f "$2" ] || { echo "no such file: $2" >&2; exit 2; }
	need_ftp
	dest=${3:-$PACK/ui/}
	ftp -T "$2" "$URL/${dest%/}/$(basename "$2")"
	echo "sent $(basename "$2") to ${dest%/}/"
	;;
pull)
	need_ftp
	OUT=${2:-$ROOT/.lr3ds/console/vita-$(date +%Y%m%d-%H%M%S)}
	mkdir -p "$OUT"
	got=0
	for f in $LOGS; do
		if ftp -s -f "$URL/$PACK/$f" -o "$OUT/$f" 2>/dev/null; then
			echo "got $f"
			got=$((got + 1))
		else
			rm -f "$OUT/$f"
		fi
	done
	if [ -z "$2" ]; then
		ln -sfn "$(basename "$OUT")" "$ROOT/.lr3ds/console/vita-latest"
	fi
	echo "$got files in $OUT"
	dumps=$(curl -sS --connect-timeout 5 "$URL/ux0:/data/" | grep -oE '[^ ]+\.(psp2dmp|sgx)(\.gz)?$' || true)
	if [ -n "$dumps" ]; then
		echo "crash dumps on the console (fetch one with: $0 get ux0:/data/<name>):"
		echo "$dumps" | sed 's/^/  /'
	fi
	;;
ls)
	need_ftp
	path=${2:-$PACK/}
	curl -sS --connect-timeout 5 "$URL/${path%/}/"
	;;
get)
	[ -n "$2" ] || { echo "usage: $0 get <path> [dir]" >&2; exit 2; }
	need_ftp
	OUT=${3:-.}
	mkdir -p "$OUT"
	ftp -f "$URL/$2" -o "$OUT/$(basename "$2")"
	echo "got $OUT/$(basename "$2")"
	;;
run)
	send "launch $TITLE" || { echo "no vitacompanion at $IP: start the game on the console" >&2; exit 1; }
	;;
stop)
	send destroy || { echo "no vitacompanion at $IP: close the game on the console" >&2; exit 1; }
	;;
esac
