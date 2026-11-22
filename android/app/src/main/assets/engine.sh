#!/system/bin/sh
# engine.sh — Win11 ARM64 -> DroidVM bootable raw image builder (root, Android)
# Usage: sh engine.sh <config.env>
# config.env KEY=VALUE lines, required:
#   BIN= tool dir (nativeLibraryDir)   WORK_DIR= scratch   OUT_IMG= output path
#   ISO_PATH= Win11 ARM64 iso          [DRIVERS_ZIP= gunyah drivers zip]
#   [DISK_MB=40960] [ESP_MB=512] [IMG_INDEX=] [USERNAME=USER] [PASSWORD=USER]
set -u
umask 022

log() { echo "[engine] $*"; }
die() { echo "[engine][FATAL] $*" >&2; exit 1; }

[ $# -ge 1 ] || die "usage: engine.sh <config.env>"
CONFIG="$1"
[ -f "$CONFIG" ] || die "config not found: $CONFIG"
. "$CONFIG"

export PATH="${BIN}:${PATH}"
export LD_LIBRARY_PATH="${BIN}"
export TMPDIR="${WORK_DIR}/tmp"
BB="${BIN}/lib_busybox"
[ -x "$BB" ] || die "busybox missing at $BB"

MKFS_VFAT="${BIN}/lib_mkfs.fat"
MKFS_NTFS="${BIN}/lib_mkfs.ntfs"
NTFS3G="${BIN}/lib_ntfs3g"
WIMINFO="${BIN}/lib_wiminfo"
WIMAPPLY="${BIN}/lib_wimapply"
WIMEXTRACT="${BIN}/lib_wimextract"
SEVENZ="${BIN}/lib_7zz"
HIVEUTIL="${BIN}/lib_hiveutil"
MAKEGPT="${BIN}/lib_makegpt"

find_file() { $BB find "$1" -name "$2" 2>/dev/null | $BB head -n1; }

DISK_MB="${DISK_MB:-40960}"
ESP_MB="${ESP_MB:-512}"
USERNAME="${USERNAME:-USER}"
PASSWORD="${PASSWORD:-USER}"
IMG_INDEX="${IMG_INDEX:-}"

MNT="$WORK_DIR/mnt"
$BB mkdir -p "$MNT" "$WORK_DIR/iso" "$WORK_DIR/drivers" "$WORK_DIR/out" "$TMPDIR"

log "=== DroidVM Win11 ARM64 builder ==="
log "ISO=$ISO_PATH OUT=$OUT_IMG DISK=${DISK_MB}MB"

# ---------- 1. extract install.wim + boot file from ISO ----------
log "step1: extracting install.wim from ISO (few GB, be patient)"
SEVENZ_OK=0
if ! $SEVENZ x -y -o"$WORK_DIR/iso" "$ISO_PATH" sources/install.wim >/dev/null 2>&1; then
  log "step1: install.wim missing, trying split install.swm"
  $SEVENZ x -y -o"$WORK_DIR/iso" "$ISO_PATH" "sources/install.swm" >/dev/null 2>&1 || die "cannot extract install.wim/.swm"
fi
ISO="$WORK_DIR/iso"
[ -f "$ISO/sources/install.wim" ] || die "install.wim not found after extraction"
BOOTAA64=$(find_file "$ISO" "bootaa64.efi")
log "step1: done (bootaa64: ${BOOTAA64:-none})"

# ---------- 2. pick image index ----------
if [ -z "$IMG_INDEX" ]; then
  $WIMINFO "$ISO/sources/install.wim"
  IDX=$($WIMINFO "$ISO/sources/install.wim" | $BB awk '/^Index:/{i=$2} /^Name:/{if($2 ~ /Pro/) {print i; exit}}')
  IMG_INDEX="${IDX:-1}"
fi
log "step2: install.wim index = $IMG_INDEX"

# ---------- 3. create raw image + GPT ----------
log "step3: creating ${DISK_MB}MB sparse raw image + GPT"
$MAKEGPT "$OUT_IMG" "$DISK_MB" "$ESP_MB" || die "makegpt failed"
ESP_START=$(( 2048 * 512 ))
ESP_SIZE=$(( ESP_MB * 1024 * 1024 ))
NTFS_START=$(( ESP_START + ESP_SIZE + 16 * 1024 * 1024 ))

# ---------- 4. ESP ----------
log "step4: ESP — format, bootmgfw.efi, BCD"
LOOP_ESP=$("${BIN}/lib_lsetup" "$OUT_IMG" "$ESP_START" "$ESP_SIZE") || die "lsetup ESP failed"
cleanup_esp() { $BB umount "$MNT/esp" 2>/dev/null; lsetup -d "$LOOP_ESP" 2>/dev/null; }
$MKFS_VFAT -F 32 "$LOOP_ESP" || { cleanup_esp; die "mkfs.vfat failed"; }
$BB mkdir -p "$MNT/esp"
$BB mount -t vfat "$LOOP_ESP" "$MNT/esp" || { cleanup_esp; die "mount ESP failed"; }
$BB mkdir -p "$MNT/esp/EFI/Boot" "$MNT/esp/EFI/Microsoft/Boot"
[ -n "$BOOTAA64" ] && $BB cp "$BOOTAA64" "$MNT/esp/EFI/Boot/bootaa64.efi"
$WIMEXTRACT "$ISO/sources/install.wim" "$IMG_INDEX" --no-acls \
  --dest-dir="$WORK_DIR/out" \
  "/Windows/Boot/EFI/bootmgfw.efi" "/Windows/Boot/EFI/bcd.template" >/dev/null 2>&1 \
  || die "cannot extract bootmgfw.efi/bcd.template (index=$IMG_INDEX)"
$BB cp "$WORK_DIR/out/bootmgfw.efi" "$MNT/esp/EFI/Microsoft/Boot/bootmgfw.efi" || die "bootmgfw.efi copy failed"
$BB cp "$WORK_DIR/out/bcd.template" "$MNT/esp/EFI/Microsoft/Boot/bcd" || die "bcd.template copy failed"
$HIVEUTIL bcd "$MNT/esp/EFI/Microsoft/Boot/bcd" || { cleanup_esp; die "BCD build failed"; }
$BB umount "$MNT/esp" && lsetup -d "$LOOP_ESP" && log "step4: ESP done"

# ---------- 5. NTFS: format + apply WIM ----------
log "step5: NTFS — mkfs + wimapply (longest step)"
LOOP_NTFS=$("${BIN}/lib_lsetup" "$OUT_IMG" "$NTFS_START") || die "lsetup NTFS failed"
cleanup_ntfs() { $BB umount "$MNT/ntfs" 2>/dev/null; lsetup -d "$LOOP_NTFS" 2>/dev/null; }
$MKFS_NTFS -f -L Windows "$LOOP_NTFS" || { cleanup_ntfs; die "mkfs.ntfs failed"; }
$BB mkdir -p "$MNT/ntfs"
$NTFS3G "$LOOP_NTFS" "$MNT/ntfs" || { cleanup_ntfs; die "ntfs-3g mount failed (check /dev/fuse)"; }
$WIMAPPLY "$ISO/sources/install.wim" "$IMG_INDEX" "$MNT/ntfs" --no-acls || { cleanup_ntfs; die "wimapply failed"; }
log "step5: WIM applied"

# ---------- 6. unattend / drivers / registry ----------
log "step6: unattend + drivers + registry patches"
$BB mkdir -p "$MNT/ntfs/Windows/Panther" "$MNT/ntfs/Windows/Setup/Scripts" "$MNT/ntfs/Drivers" \
  "$MNT/ntfs/Windows/System32/Sysprep"
[ -f "$ASSET_DIR/unattend.xml" ] && $BB cp "$ASSET_DIR/unattend.xml" "$MNT/ntfs/Windows/Panther/unattend.xml"
[ -f "$ASSET_DIR/SetupComplete.cmd" ] && $BB cp "$ASSET_DIR/SetupComplete.cmd" "$MNT/ntfs/Windows/Setup/Scripts/SetupComplete.cmd"

if [ -n "${DRIVERS_ZIP:-}" ] && [ -f "$DRIVERS_ZIP" ]; then
  log "step6: extracting drivers zip"
  $SEVENZ x -y -o"$WORK_DIR/drivers" "$DRIVERS_ZIP" >/dev/null 2>&1 || log "WARN: drivers extract failed"
  DRV_ROOT=$(find_file "$WORK_DIR/drivers" "*.inf" | $BB sed 's|/[^/]*$||')
  if [ -n "$DRV_ROOT" ]; then
    log "step6: copying drivers from $DRV_ROOT"
    $BB cp -r "$DRV_ROOT/." "$MNT/ntfs/Drivers/"
    $BB find "$MNT/ntfs/Drivers" -name "*.inf" > "$MNT/ntfs/Windows/Setup/Scripts/drivers.txt"
  fi
fi

SYS="$MNT/ntfs/Windows/System32/config/SYSTEM"
SOFT="$MNT/ntfs/Windows/System32/config/SOFTWARE"
$HIVEUTIL setval "$SOFT" "\Microsoft\Windows NT\CurrentVersion\Winlogon" "AutoAdminLogon" sz "1" >/dev/null 2>&1 || log "WARN: winlogon patch failed"
log "step6: registry patched"

# ---------- 7. finalize ----------
$BB sync
$BB umount "$MNT/ntfs" || { sleep 2; $BB umount "$MNT/ntfs"; } || die "umount NTFS failed"
lsetup -d "$LOOP_NTFS" 2>/dev/null
$BB sync
log "=== DONE: $OUT_IMG ==="
