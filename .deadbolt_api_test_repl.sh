#!/bin/bash

set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$PROJECT_DIR/.deadbolt_api_test_env.sh"
NODE_ID="${1:-1}"
ENDPOINT_ID="${2:-1}"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing env file: $ENV_FILE"
  exit 1
fi

export NODE_ID ENDPOINT_ID
source "$ENV_FILE"

print_banner() {
  echo ""
  echo "deadbolt_api_test (chip-tool)"
  echo "Node: $NODE_ID  Endpoint: $ENDPOINT_ID  Cluster: DoorLock (0x0101)"
  echo "Type /help for commands, /exit to quit."
  echo ""
}

run_cmd() {
  local input="$1"
  input="${input#${input%%[![:space:]]*}}"
  input="${input%${input##*[![:space:]]}}"
  if [[ -z "$input" ]]; then
    return 0
  fi

  case "$input" in
    /help|help)        api_help ;;
    /node)             echo "node=$NODE_ID  endpoint=$ENDPOINT_ID" ;;
    /exit|exit|quit)   return 99 ;;
    /lock)             lock_door ;;
    /unlock)           unlock_door ;;
    /exit_open)        exit_open ;;
    /exit_open\ *)     eval "exit_open ${input#/exit_open }" ;;
    /state|/status)    lock_state ;;
    /pair)             pair ;;
    /pair_wifi\ *)     eval "pair_wifi ${input#/pair_wifi }" ;;
    /pair_auto)        pair_auto ;;
    /unpair)           unpair ;;
    /smoke)            smoke ;;
    *)
      if [[ "$input" == /* ]]; then
        local raw="${input#/}"
        eval "$raw"
      else
        eval "$input"
      fi
      ;;
  esac
}

print_banner
while true; do
  read -r -e -p "deadbolt> " line || break
  run_cmd "$line"
  rc=$?
  if [[ $rc -eq 99 ]]; then
    break
  fi
done
