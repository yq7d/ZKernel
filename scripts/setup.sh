#!/usr/bin/env bash
# installs build deps on arch
#
# usage: ./scripts/setup.sh [--with-clang] [--with-rust] [--yes]
set -euo pipefail

WITH_CLANG=0
WITH_RUST=0
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        --with-clang) WITH_CLANG=1 ;;
        --with-rust)  WITH_RUST=1 ;;
        --yes|-y)     ASSUME_YES=1 ;;
        -h|--help)    sed -n '2,4p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

c_ok()   { printf '\e[32m[ OK ]\e[0m %s\n' "$*"; }
c_warn() { printf '\e[33m[WARN]\e[0m %s\n' "$*"; }
c_err()  { printf '\e[31m[FAIL]\e[0m %s\n' "$*"; }
c_info() { printf '\e[36m[INFO]\e[0m %s\n' "$*"; }

if ! command -v pacman >/dev/null; then
    c_err "pacman not found, this is for arch"
    exit 1
fi
if [[ $EUID -eq 0 ]]; then
    c_err "don't run as root, it uses sudo"
    exit 1
fi

PKGS=(
    base-devel gcc binutils make cmake nasm gdb git
    limine xorriso mtools dosfstools
    qemu-desktop edk2-ovmf
)
(( WITH_CLANG )) && PKGS+=(clang lld llvm)
(( WITH_RUST ))  && PKGS+=(rustup)

c_info "packages:"
printf '    %s\n' "${PKGS[@]}"

if (( ! ASSUME_YES )); then
    read -rp "Proceed? [Y/n] " ans
    [[ "${ans,,}" == n* ]] && { echo "Aborted."; exit 1; }
fi

sudo pacman -Syu --needed --noconfirm "${PKGS[@]}"

if (( WITH_RUST )); then
    c_info "installing rust nightly"
    rustup toolchain install nightly --component rust-src llvm-tools-preview
    rustup default nightly
fi

# kvm
if [[ -e /dev/kvm ]]; then
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
        c_ok "kvm available"
    else
        c_info "adding $USER to the kvm group"
        sudo usermod -aG kvm "$USER"
        c_warn "log out and back in for the group change to apply"
    fi
else
    c_warn "no /dev/kvm, enable VT-x/AMD-V in firmware settings. qemu will be slow"
fi

echo
c_info "checking"
FAILED=0
check() {
    if command -v "$1" >/dev/null; then
        c_ok "$(printf '%-20s' "$1") $("$1" --version 2>/dev/null | head -1)"
    else
        c_err "$1 missing"; FAILED=1
    fi
}
for t in gcc ld nasm make gdb xorriso mformat qemu-system-x86_64 limine; do
    check "$t"
done
(( WITH_CLANG )) && { check clang; check ld.lld; }
(( WITH_RUST ))  && { check rustc; check cargo; }

LIMINE_DIR=/usr/share/limine
if [[ -f $LIMINE_DIR/limine-bios-cd.bin && -f $LIMINE_DIR/BOOTX64.EFI ]]; then
    c_ok "Limine boot files found in $LIMINE_DIR"
else
    c_err "Limine boot files not found in $LIMINE_DIR"; FAILED=1
fi

OVMF=""
for f in /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
    [[ -f $f ]] && { OVMF=$f; break; }
done
if [[ -n $OVMF ]]; then
    c_ok "OVMF UEFI firmware: $OVMF"
else
    c_warn "ovmf not found, vm.sh --uefi won't work"
fi

echo
if (( FAILED )); then
    c_err "some things are missing"
    exit 1
fi
c_ok "done"
