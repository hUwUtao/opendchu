#!/usr/bin/env bash
# Glowing animation for DCHU keyboard backlight LED
# Default LED: /sys/class/leds/dchu::kbd_backlight (max_brightness=5)

set -euo pipefail

LED_DEFAULT="/sys/class/leds/dchu::kbd_backlight"
LED_PATH="${LED_DEFAULT}"
DELAY="0.08"       # seconds between steps
CYCLES="-1"        # -1 = infinite, otherwise number of up+down cycles
MAX_OVERRIDE=""    # override max_brightness

usage() {
  cat <<EOF
Usage: $(basename "$0") [-d LED_PATH] [-s STEP_DELAY] [-c CYCLES] [-m MAX]

  -d LED_PATH   LED sysfs path (default: ${LED_DEFAULT})
  -s DELAY      Step delay in seconds (default: ${DELAY})
  -c CYCLES     Number of glow cycles; -1 = infinite (default: ${CYCLES})
  -m MAX        Override max_brightness (default: read from sysfs)

Examples:
  sudo $(basename "$0")
  sudo $(basename "$0") -s 0.05 -c 20
  sudo $(basename "$0") -m 5
EOF
}

while getopts ":d:s:c:m:h" opt; do
  case "$opt" in
    d) LED_PATH="$OPTARG" ;;
    s) DELAY="$OPTARG" ;;
    c) CYCLES="$OPTARG" ;;
    m) MAX_OVERRIDE="$OPTARG" ;;
    h) usage; exit 0 ;;
    :) echo "Missing value for -$OPTARG" >&2; usage; exit 2 ;;
    \?) echo "Unknown option: -$OPTARG" >&2; usage; exit 2 ;;
  esac
done

# Auto-detect LED if default not present
if [ ! -d "$LED_PATH" ]; then
  cand=$(ls -d /sys/class/leds/dchu::* 2>/dev/null | head -n1 || true)
  if [ -n "$cand" ]; then
    LED_PATH="$cand"
  fi
fi

if [ ! -d "$LED_PATH" ]; then
  echo "LED path not found: $LED_PATH" >&2
  exit 1
fi

BR="$LED_PATH/brightness"
MB="$LED_PATH/max_brightness"

if [ ! -f "$BR" ]; then
  echo "Missing brightness file: $BR" >&2
  exit 1
fi

if [ -n "$MAX_OVERRIDE" ]; then
  MAX="$MAX_OVERRIDE"
else
  if [ -f "$MB" ]; then
    MAX=$(cat "$MB")
  else
    MAX=5
  fi
fi

# Sanitize numeric inputs
if ! [[ "$MAX" =~ ^[0-9]+$ ]] || [ "$MAX" -lt 1 ]; then
  echo "Invalid max brightness: $MAX" >&2
  exit 2
fi

if ! [[ "$CYCLES" =~ ^-?[0-9]+$ ]]; then
  echo "Invalid cycles count: $CYCLES" >&2
  exit 2
fi

write_brightness() {
  local v="$1"
  # Try direct write; if it fails, attempt sudo tee
  if ! echo "$v" > "$BR" 2>/dev/null; then
    echo "$v" | sudo tee "$BR" >/dev/null
  fi
}

ORIG=$(cat "$BR" 2>/dev/null || echo 0)
cleanup() {
  write_brightness "$ORIG" || true
}
trap cleanup EXIT INT TERM

# Main loop: ramp 0..MAX..0
cycle=0
while :; do
  # Up
  for ((i=0; i<=MAX; i++)); do
    write_brightness "$i"
    sleep "$DELAY"
  done
  # Down (avoid repeating endpoints too long)
  for ((i=MAX-1; i>=1; i--)); do
    write_brightness "$i"
    sleep "$DELAY"
  done

  if [ "$CYCLES" -ge 0 ]; then
    cycle=$((cycle+1))
    if [ "$cycle" -ge "$CYCLES" ]; then
      break
    fi
  fi
done

exit 0

