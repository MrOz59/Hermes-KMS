# Maintainer: MrOz59 <https://github.com/MrOz59>
#
# DKMS package for the Hermes-KMS virtual display driver. DKMS rebuilds the
# module automatically for every installed kernel, the same way evdi-dkms does.
#
# This is a -git PKGBUILD: it builds from the latest commit on the default
# branch. Once tagged releases exist, a versioned PKGBUILD can pin a tag.

pkgname=hermes-kms-dkms-git
_pkgbase=hermes-kms
pkgver=0.1.2
pkgrel=1
pkgdesc="Hermes-KMS zero-copy virtual display DRM/KMS driver (DKMS)"
arch=('x86_64')
url="https://github.com/MrOz59/Hermes-KMS"
license=('GPL2')
depends=('dkms')
makedepends=('git')
optdepends=('libva: VAAPI import-check tool'
            'libdrm: VAAPI import-check tool'
            'kscreen: enable the virtual output on KDE/KWin')
provides=('hermes-kms')
conflicts=('hermes-kms')
source=("git+https://github.com/MrOz59/Hermes-KMS.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/Hermes-KMS"
  # 0.1.2.r<commits>.g<short-sha> — sortable and unique per commit.
  local _ver
  _ver=$(awk '
    /^#define HERMES_KMS_DRIVER_MAJOR/ { maj=$3 }
    /^#define HERMES_KMS_DRIVER_MINOR/ { min=$3 }
    /^#define HERMES_KMS_DRIVER_PATCH/ { pat=$3 }
    END { print maj"."min"."pat }' kernel/hermes-kms/hermes_kms.c)
  printf '%s.r%s.g%s' "$_ver" \
    "$(git rev-list --count HEAD)" \
    "$(git rev-parse --short HEAD)"
}

package() {
  cd "$srcdir/Hermes-KMS"

  local _dest="$pkgdir/usr/src/${_pkgbase}-${pkgver}"
  install -dm755 "$_dest"

  # Ship the whole source tree so DKMS can build the module (the module Makefile
  # references ../../include/uapi, so the layout must be preserved).
  cp -a Makefile dkms.conf include kernel tools udev scripts "$_dest/"

  # Make the DKMS package version match the directory DKMS expects.
  sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${pkgver}\"/" "$_dest/dkms.conf"

  # Auto-load the module at boot with initial_enabled=1 so the compositor
  # adopts the virtual output before the session starts.
  install -Dm644 packaging/modules-load.d/hermes-kms.conf \
    "$pkgdir/usr/lib/modules-load.d/hermes-kms.conf"
  install -Dm644 packaging/modprobe.d/hermes-kms.conf \
    "$pkgdir/usr/lib/modprobe.d/hermes-kms.conf"
}
