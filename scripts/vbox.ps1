# usage: .\scripts\vbox.ps1 [-Iso zkernel.iso] [options]
#
# makes (or fixes) a virtualbox vm for zkernel on windows and boots the iso.
# with no -Iso it asks for one.
#
#   -Name NAME   vm name, default ZKernel. an existing vm gets fixed up
#   -Disk FILE   disk.img (copied to a .vdi next to it once) or a .vdi
#   -Mem MB      default 512
#   -Cpus N      default 2
#   -Uefi        boot with efi firmware
#   -NoStart     set the vm up but don't boot it
param(
    [string]$Iso,
    [string]$Name = "ZKernel",
    [string]$Disk,
    [int]$Mem = 512,
    [int]$Cpus = 2,
    [switch]$Uefi,
    [switch]$NoStart
)
$ErrorActionPreference = "Stop"

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

# VBoxManage isn't on PATH by default
$vbm = $null
foreach ($dir in @($env:VBOX_MSI_INSTALL_PATH, $env:VBOX_INSTALL_PATH,
                   "$env:ProgramFiles\Oracle\VirtualBox")) {
    if ($dir -and (Test-Path (Join-Path $dir "VBoxManage.exe"))) {
        $vbm = Join-Path $dir "VBoxManage.exe"; break
    }
}
if (-not $vbm) {
    $cmd = Get-Command VBoxManage.exe -ErrorAction SilentlyContinue
    if ($cmd) { $vbm = $cmd.Source }
}
if (-not $vbm) { Fail "VBoxManage.exe not found, install VirtualBox" }

function VBox {
    & $vbm @args
    if ($LASTEXITCODE -ne 0) { Fail "VBoxManage $($args -join ' ') failed" }
}

if (-not $Iso) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Pick the ZKernel iso"
    $dlg.Filter = "ISO images (*.iso)|*.iso|All files (*.*)|*.*"
    if ($dlg.ShowDialog() -ne "OK") { Fail "no iso picked" }
    $Iso = $dlg.FileName
}
if (-not (Test-Path $Iso)) { Fail "$Iso not found" }
$Iso = (Resolve-Path $Iso).Path

# powershell 5.1 turns redirected native stderr into an error under Stop
$ErrorActionPreference = "Continue"
& $vbm showvminfo $Name *> $null
$exists = $LASTEXITCODE -eq 0
$ErrorActionPreference = "Stop"
if (-not $exists) {
    # a vm folder left behind (deleted from the list but not from disk)
    # makes createvm fail, so register that one instead
    $folder = ((& $vbm list systemproperties) | Where-Object { $_ -match '^Default machine folder:' }) `
        -replace '^Default machine folder:\s*', ''
    $cfg = Join-Path (Join-Path $folder $Name) "$Name.vbox"
    if ($folder -and (Test-Path $cfg)) {
        VBox registervm $cfg
        Write-Host "registered the existing $cfg"
    } else {
        VBox createvm --name $Name --ostype Other_64 --register | Out-Null
        Write-Host "made vm $Name"
    }
}

$info = & $vbm showvminfo $Name --machinereadable
if ($info -match '^VMState="(running|paused|stuck)"') { Fail "$Name is running, power it off first" }
# a saved state (closed with "save the machine state") locks the settings
if ($info -match '^VMState="saved"') {
    VBox discardstate $Name
    Write-Host "dropped the saved state of $Name"
    $info = & $vbm showvminfo $Name --machinereadable
}

$firmware = "bios"
if ($Uefi) { $firmware = "efi" }
$vmdir = Split-Path (($info | Where-Object { $_ -match '^CfgFile=' }) -replace '^CfgFile="(.*)"$', '$1' -replace '\\\\', '\')
$serial = Join-Path $vmdir "serial.log"

# a 32 bit os type hides long mode from the guest, limine then says
# "This CPU does not support 64-bit mode"
VBox modifyvm $Name `
    --ostype Other_64 --longmode on --ioapic on --pae on `
    --hwvirtex on --nested-paging on `
    --memory $Mem --cpus $Cpus --firmware $firmware `
    --graphicscontroller vmsvga --vram 32 `
    --mouse ps2 --keyboard ps2 `
    --nic1 nat --nictype1 virtio `
    --uart1 0x3f8 4 --uartmode1 file $serial `
    --boot1 dvd --boot2 disk --boot3 none --boot4 none

# sata is ahci, which the kernel has a driver for
if (-not ($info -match '^storagecontrollername\d+="SATA"')) {
    VBox storagectl $Name --name SATA --add sata --controller IntelAhci --portcount 2
}
VBox storageattach $Name --storagectl SATA --port 0 --device 0 --type dvddrive --medium $Iso

if ($Disk) {
    if (-not (Test-Path $Disk)) { Fail "$Disk not found" }
    $Disk = (Resolve-Path $Disk).Path
    # virtualbox can't use a raw image directly, so it gets a vdi copy once
    if ([IO.Path]::GetExtension($Disk) -ne ".vdi") {
        $vdi = [IO.Path]::ChangeExtension($Disk, ".vdi")
        if (-not (Test-Path $vdi)) {
            VBox convertfromraw $Disk $vdi --format VDI | Out-Null
            Write-Host "copied $Disk to $vdi"
        }
        $Disk = $vdi
    }
    VBox storageattach $Name --storagectl SATA --port 1 --device 0 --type hdd --medium $Disk
}

# with hyper-v on, virtualbox runs on top of it and is often slow or broken
try {
    if ((Get-CimInstance Win32_ComputerSystem).HypervisorPresent) {
        Write-Host "hyper-v is on. if the vm fails or crawls, turn off hyper-v," -ForegroundColor Yellow
        Write-Host "windows hypervisor platform and memory integrity, then reboot" -ForegroundColor Yellow
    }
} catch {}

if ($NoStart) { Write-Host "$Name is ready"; exit 0 }
Write-Host "serial log: $serial"
Write-Host "if it still says no 64-bit mode, turn on VT-x/AMD-V in the bios"
VBox startvm $Name
