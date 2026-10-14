#!/usr/bin/env bash
# Boot the ISO headless, optionally type lines into it, then save the serial
# log and a PNG of the screen and quit.
#
# Usage: scripts/screenshot.sh [iso] [seconds-to-wait] [line to type]...
#   e.g. scripts/screenshot.sh viralos.iso 3 "help" "@2" "crash pf"
#   A line of the form "@N" types nothing and waits N seconds instead;
#   "key:ctrl-c" presses a key combination (QEMU sendkey syntax) without Enter;
#   "type:text" types text without pressing Enter;
#   "mon:mouse_move 10 5" sends a raw QEMU monitor command (mouse_move, mouse_button...).
set -euo pipefail
export LC_ALL=C

ISO=${1:-viralos.iso}
WAIT=${2:-3}
shift $(( $# < 2 ? $# : 2 ))

# Map a character to a QEMU `sendkey` key name (US layout).
key_for() {
    local c=$1
    case "$c" in
        [a-z0-9]) echo "$c" ;;
        [A-Z])    echo "shift-${c,,}" ;;
        ' ')  echo spc ;;
        '-')  echo minus ;;        '_') echo shift-minus ;;
        '=')  echo equal ;;        '+') echo shift-equal ;;
        '.')  echo dot ;;          ',') echo comma ;;
        '/')  echo slash ;;        '?') echo shift-slash ;;
        ';')  echo semicolon ;;    ':') echo shift-semicolon ;;
        "'")  echo apostrophe ;;   '!') echo shift-1 ;;
        '>')  echo shift-dot ;;    '<') echo shift-comma ;;
        '"')  echo shift-apostrophe ;;
        '*')  echo shift-8 ;;      '|') echo shift-backslash ;;
        '&')  echo shift-7 ;;      '~') echo shift-grave_accent ;;
        *) echo "screenshot.sh: can't type '$c'" >&2; exit 1 ;;
    esac
}

monitor_script() {
    sleep "$WAIT"
    for line in "$@"; do
        if [[ $line == @* ]]; then   # "@2.5" = just wait 2.5 seconds
            sleep "${line#@}"
            continue
        fi
        if [[ $line == shot:* ]]; then  # "shot:step1" = extra screenshot build/step1.png
            echo "screendump build/${line#shot:}.png -f png"
            sleep 0.5
            continue
        fi
        if [[ $line == mon:* ]]; then   # "mon:mouse_move 10 5" = any QEMU monitor command
            echo "${line#mon:}"
            sleep 0.3
            continue
        fi
        if [[ $line == key:* ]]; then   # "key:ctrl-c" = press a key combination
            echo "sendkey ${line#key:}"
            sleep 0.5
            continue
        fi
        local enter=1
        if [[ $line == type:* ]]; then  # "type:abc" = type without pressing Enter
            line=${line#type:}
            enter=0
        fi
        for (( i = 0; i < ${#line}; i++ )); do
            echo "sendkey $(key_for "${line:i:1}")"
            sleep 0.05
        done
        if (( enter )); then
            echo "sendkey ret"
            sleep 0.5
        fi
    done
    echo "screendump build/screen.png -f png"
    sleep 1
    echo quit
}

mkdir -p build
rm -f build/screen.png build/serial.log

# UEFI=1 boots with OVMF firmware instead of the default SeaBIOS.
# CPU=qemu64 picks another CPU model (default: max, which has AVX).
FIRMWARE=()
if [[ ${UEFI:-0} == 1 ]]; then
    FIRMWARE=(-bios /usr/share/qemu/OVMF.fd)
fi

# QEMU_EXTRA adds arguments, e.g. QEMU_EXTRA="-machine vmport=off" (plain PS/2 mouse).
# shellcheck disable=SC2086
monitor_script "$@" |
    qemu-system-x86_64 "${FIRMWARE[@]}" -M q35 -cpu "${CPU:-max}" -m 512M -rtc base=localtime -cdrom "$ISO" -no-reboot \
        ${QEMU_EXTRA:-} \
        -display none -serial file:build/serial.log -monitor stdio >/dev/null

echo "--- serial (build/serial.log) ---"
cat build/serial.log
echo "--- screen saved to build/screen.png ---"
