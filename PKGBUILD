# Maintainer: MrOz59 <https://github.com/MrOz59>
#
# DKMS package for the Hermes-KMS virtual display driver. DKMS rebuilds the
# module automatically for every installed kernel, the same way evdi-dkms does.
#
# This is a -git PKGBUILD: it builds from the latest commit on the default
# branch. Once tagged releases exist, a versioned PKGBUILD can pin a tag.

pkgname=hermes-kms-dkms-git
_pkgbase=hermes-kms
pkgver=0.4.0
pkgrel=1
pkgdesc="Hermes-KMS zero-copy virtual display DRM/KMS driver (DKMS)"
arch=('any')
url="https://github.com/MrOz59/Hermes-KMS"
license=('GPL2' 'MIT')
depends=('dkms' 'seatd')
makedepends=('git')
optdepends=('polkit: authorize hermes-kms-setup through pkexec')
provides=('hermes-kms')
conflicts=('hermes-kms')
# Reports a stale /etc/modprobe.d override that masks the shipped default.
install="${_pkgbase}.install"
source=("git+https://github.com/MrOz59/Hermes-KMS.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/Hermes-KMS"
  # <driver-version>.r<commits>.g<short-sha> — sortable and unique per commit.
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

  # DKMS needs only the module build files. Do not copy generated/host-native
  # tool binaries or unrelated packaging assets into the privileged source tree.
  cp -a Makefile dkms.conf include kernel "$_dest/"

  # Make the DKMS package version match the directory DKMS expects.
  sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"${pkgver}\"/" "$_dest/dkms.conf"

  # Keep package and image-based installs on the same runtime manifest.
  make DESTDIR="$pkgdir" \
    HERMES_LICENSE_DIR="/usr/share/licenses/$pkgname" \
    install-configs install-uapi
}
