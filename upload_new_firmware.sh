#!/bin/bash
set -euo pipefail

USB_DEVICE="Adafruit"
MOUNT_POINT="/run/media/bartek/NICENANO"
FIRMWARE_DEST="/media/bartek/LEXAR/DEV/keyboards/uniCorne/zmk-config-unicorne/firmware/"
SIDE=""

RED=$'\e[31m'
GREEN=$'\e[32m'
RESET=$'\e[0m'

while getopts ":lr" opt; do
  case "$opt" in
    l) SIDE="left" ;;
    r) SIDE="right" ;;
    \?) 
      echo -e "${RED}Invalid option!${RESET}"
      echo -e "-l for flashing LEFT side\n-r for flashing RIGHT side" 
      exit 1 
      ;;
  esac
done

if [[ -z "$SIDE" ]]; then
  echo "${RED}No option selected!${RESET}"
  echo -e "-l for flashing LEFT side\n-r for flashing RIGHT side" 
  exit 1
fi

if ! lsusb | grep -q "$USB_DEVICE"; then
  echo "${RED}Connect nice!nano!${RESET}"
  exit 1
fi

DEVICE=$(lsblk -pn -o NAME,LABEL | awk '$2=="NICENANO" {print $1}')
if [[ "$DEVICE" == "" ]]; then
  echo "${RED}No Device Detected!${RESET}"
  exit 1
fi

if [[ -f ~/Downloads/firmware.zip ]]; then
  mv ~/Downloads/firmware.zip "$FIRMWARE_DEST"
else
  echo "${RED}Firmware not found!${RESET}"
  exit 1
fi

cd "$FIRMWARE_DEST"
unzip -o firmware.zip
udisksctl mount -b "$DEVICE"
sudo mount "$DEVICE" "$MOUNT_POINT"

if [[ "$SIDE" == "left" ]]; then
 sudo cp unicorne_left-nice_nano_v2-zmk.uf2 "$MOUNT_POINT"
 echo "Flashing Left Side"
elif [[ "$SIDE" == "right" ]]; then
 sudo cp unicorne_right-nice_nano_v2-zmk.uf2 "$MOUNT_POINT"
 echo "Flashing Right Side"
fi

echo "${GREEN}Firmware uploaded${RESET}"
