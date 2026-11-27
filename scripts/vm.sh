#!/usr/bin/env bash
# usage: ./scripts/vm.sh [options] [image.iso]   (default build/zkernel.iso)
#
#   -u, --uefi        boot with ovmf
#   -d, --debug       wait for gdb on :1234
#   -n, --nographic   serial only, ctrl-a x to quit
#   -m, --mem SIZE    default 512M
#   -D, --disk FILE   raw disk image as virtio-blk
#   -c, --cpus N      default 2
#       --no-kvm
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/build/zkernel.iso"
UEFI=0 DEBUG=0 NOGRAPHIC=0 KVM=1 DISK=""
MEM=512M CPUS=2

while (($#)); do
    case "$1" in
        -u|--uefi)      UEFI=1 ;;
        -d|--debug)     DEBUG=1 ;;
        -n|--nographic) NOGRAPHIC=1 ;;
        -m|--mem)       MEM="$2"; shift ;;
        -D|--disk)      DISK="$2"; shift ;;
        -c|--cpus)      CPUS="$2"; shift ;;
        --no-kvm)       KVM=0 ;;
        -h|--help)      sed -n '2,10p' "$0"; exit 0 ;;
        -*)             echo "Unknown option: $1" >&2; exit 1 ;;
        *)              ISO="$1" ;;
    esac
    shift
done

if [[ ! -f $ISO ]]; then
    echo "$ISO not found, run make first" >&2
    exit 1
fi

ARGS=(
    -machine q35
    -m "$MEM"
    -smp "$CPUS"
    -cdrom "$ISO"
    -boot d
    -no-reboot -no-shutdown
    -d int,cpu_reset,guest_errors
    -D "$ROOT/build/qemu.log"
)

if [[ -n $DISK ]]; then
    [[ -f $DISK ]] || { echo "$DISK not found" >&2; exit 1; }
    # legacy virtio interface, that's what the driver speaks
    ARGS+=(-drive "file=$DISK,if=none,id=disk0,format=raw"
           -device virtio-blk-pci,drive=disk0,disable-modern=on)
fi

# writing to port 0xf4 exits qemu
ARGS+=(-device isa-debug-exit,iobase=0xf4,iosize=0x04)

# no kvm when debugging
if (( KVM && !DEBUG )) && [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ARGS+=(-enable-kvm -cpu host)
else
    ARGS+=(-cpu max)
fi

if (( UEFI )); then
    OVMF=""
    for f in /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
        [[ -f $f ]] && { OVMF=$f; break; }
    done
    [[ -z $OVMF ]] && { echo "ovmf not found (edk2-ovmf)" >&2; exit 1; }
    ARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF")
fi

if (( NOGRAPHIC )); then
    ARGS+=(-nographic)
else
    ARGS+=(-serial stdio)
fi

if (( DEBUG )); then
    ARGS+=(-s -S)
    echo "waiting for gdb on :1234"
    echo "  gdb $ROOT/build/kernel.elf -ex 'target remote :1234'"
fi

mkdir -p "$ROOT/build"
exec qemu-system-x86_64 "${ARGS[@]}"
