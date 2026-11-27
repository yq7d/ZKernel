#!/usr/bin/env bash
# usage: ./scripts/vbox.sh [options] [image.iso]   (default build/zkernel.iso)
#
# makes (or fixes) a virtualbox vm for zkernel and starts it
#
#   -N, --name NAME   vm name, default ZKernel. an existing vm gets fixed up
#   -u, --uefi        boot with efi firmware
#   -m, --mem MB      default 512
#   -c, --cpus N      default 2
#       --no-disk     don't attach disk.img
#       --no-start    set the vm up but don't boot it
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/build/zkernel.iso"
NAME=ZKernel UEFI=0 MEM=512 CPUS=2 DISK=1 START=1

while (($#)); do
    case "$1" in
        -N|--name)  NAME="$2"; shift ;;
        -u|--uefi)  UEFI=1 ;;
        -m|--mem)   MEM="$2"; shift ;;
        -c|--cpus)  CPUS="$2"; shift ;;
        --no-disk)  DISK=0 ;;
        --no-start) START=0 ;;
        -h|--help)  sed -n '2,11p' "$0"; exit 0 ;;
        -*)         echo "Unknown option: $1" >&2; exit 1 ;;
        *)          ISO="$1" ;;
    esac
    shift
done

VBM=$(command -v VBoxManage || command -v vboxmanage || true)
[[ -n $VBM ]] || { echo "VBoxManage not found, install virtualbox" >&2; exit 1; }
[[ -f $ISO ]] || { echo "$ISO not found, run make first" >&2; exit 1; }
ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"

if ! "$VBM" showvminfo "$NAME" >/dev/null 2>&1; then
    "$VBM" createvm --name "$NAME" --ostype Other_64 --register >/dev/null
fi

if "$VBM" showvminfo "$NAME" --machinereadable | grep -q '^VMState="running"'; then
    echo "$NAME is running, power it off first" >&2
    exit 1
fi

# a 32 bit os type hides long mode from the guest, limine then says
# "This CPU does not support 64-bit mode"
"$VBM" modifyvm "$NAME" \
    --ostype Other_64 --longmode on --ioapic on --pae on \
    --hwvirtex on --nested-paging on \
    --memory "$MEM" --cpus "$CPUS" \
    --firmware "$( ((UEFI)) && echo efi || echo bios )" \
    --graphicscontroller vmsvga --vram 32 \
    --mouse ps2 --keyboard ps2 \
    --nic1 nat --nictype1 virtio \
    --uart1 0x3f8 4 --uartmode1 file "$ROOT/build/vbox-serial.log" \
    --boot1 dvd --boot2 disk --boot3 none --boot4 none

# sata is ahci, which the kernel has a driver for
"$VBM" showvminfo "$NAME" --machinereadable | grep -q '^storagecontrollername[0-9]*="SATA"' ||
    "$VBM" storagectl "$NAME" --name SATA --add sata --controller IntelAhci --portcount 2

"$VBM" storageattach "$NAME" --storagectl SATA --port 0 --device 0 \
    --type dvddrive --medium "$ISO"

# virtualbox can't use a raw image directly, so it gets a vdi copy once
if (( DISK )); then
    VDI="$ROOT/disk.vdi"
    if [[ ! -f $VDI ]]; then
        [[ -f $ROOT/disk.img ]] || make -C "$ROOT" disk.img
        "$VBM" convertfromraw "$ROOT/disk.img" "$VDI" --format VDI >/dev/null
    fi
    "$VBM" storageattach "$NAME" --storagectl SATA --port 1 --device 0 \
        --type hdd --medium "$VDI"
fi

(( START )) || { echo "$NAME is ready"; exit 0; }
echo "serial log: $ROOT/build/vbox-serial.log"
echo "if it still says no 64-bit mode, turn on VT-x/AMD-V in the host bios"
echo "(and on windows, turn off hyper-v / memory integrity)"
exec "$VBM" startvm "$NAME"
