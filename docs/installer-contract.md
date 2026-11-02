# Hermes ecosystem installer: Hermes-KMS package contract

Hermes-KMS ships an independent kernel component. The planned ecosystem
installer will live in the **[Hermes repository](https://github.com/MrOz59/Hermes)**,
not in Hermes-KMS. This document is a **package and integration contract**, not
an installer implementation or download link. The future Hermes installer
should detect the Linux distribution, install the correct native package,
verify the DKMS result and **only then** configure optional private session
seats. It must not copy the kernel module or change display settings
behind the package manager's back.

## Stable release assets (starting with v0.4.1)

| Platform | Required package | Optional private-session package |
| --- | --- | --- |
| Arch/CachyOS | `hermes-kms-dkms-git-*.pkg.tar.zst` | `hermes-kms-seatd-git-*.pkg.tar.zst` |
| Ubuntu/Debian | `hermes-kms-dkms_0.4.1-1_all.deb` | `hermes-kms-seatd_0.4.1-1_all.deb` |
| Fedora Workstation | `hermes-kms-dkms-0.4.1-1.fc44.noarch.rpm` | `hermes-kms-seatd-0.4.1-1.fc44.noarch.rpm` |

RPM release suffixes follow the build Fedora version (`fc44`, etc.);
the installer must discover assets and validate their metadata, not assume
one literal Fedora suffix. The `-git` Arch package version also contains
commit-derived build metadata. For reproducibility, use release-attached
artifacts and compare the published `SHA256SUMS` instead of invoking a
default-branch PKGBUILD.

All three platforms keep the **same driver version 0.4.1 and UAPI 14**.
The source and package names for DKMS must remain `hermes-kms`, and the
kernel module is `hermes_kms`.

## Native package installation (mutable systems)

On Ubuntu/Debian:

```sh
sudo apt install ./hermes-kms-dkms_0.4.1-1_all.deb
# Optional, for private seat brokers:
sudo apt install ./hermes-kms-seatd_0.4.1-1_all.deb
```

On Fedora Workstation, use the RPM matching the distribution version:

```sh
sudo dnf install ./hermes-kms-dkms-0.4.1-1.fc44.noarch.rpm
# Optional:
sudo dnf install ./hermes-kms-seatd-0.4.1-1.fc44.noarch.rpm
```

On Arch/CachyOS:

```sh
sudo pacman -U ./hermes-kms-dkms-git-*.pkg.tar.zst
# Optional:
sudo pacman -U ./hermes-kms-seatd-git-*.pkg.tar.zst
```

**Important:** The examples assume matching release assets and kernel headers
are present. The installer must discover those dependencies for the user's
current kernel, rather than silently assuming a kernel flavor. DKMS may
register successfully even when module compilation for the running kernel
fails. Treat missing matching headers and build errors as an incomplete
installation, not success.

No package should force-load or forcibly remove a module while KWin, Xwayland,
a streaming client, Gamescope, or a DMA-BUF holder is using it. A reboot is
the safe upgrade boundary.

## Suggested noninteractive installer contract

1. Check `/etc/os-release`, package manager, architecture, root access and
   whether the system has an immutable `/usr` (e.g. bootc/ostree).
2. Verify release asset checksums before executing a privileged installation.
   Do not accept experimental/nightly tags unless explicitly chosen.
3. Check or install headers that **match the intended boot kernel**. Verify
   `/lib/modules/<kernel>/build/Makefile`; select suitable compiler (GCC or
   Clang) for that kernel. Do not replace the active kernel silently.
4. Install the core package via `apt`, `dnf`, or `pacman`; optionally
   install the matching broker package if the user requests isolated sessions.
5. Read `dkms status -m hermes-kms` and ensure the intended kernel has the
   expected `0.4.1` module in an installed state. The installed DKMS source is
   `/usr/src/hermes-kms-0.4.1/`. A package transaction alone is not proof.
6. If private sessions were requested and seatd is installed, run
   `sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user <UID>` with
   an explicitly selected non-root UID. Do not infer a UID from a root service
   account or enable unused brokers indiscriminately. The helper may return
   code 10 when a reboot is needed for a changed module topology.
7. Request a reboot on upgrade or incompatible loaded-module parameters;
   after reboot, check `modinfo -F version hermes_kms`,
   `journalctl -k -g hermes-kms`, render node permissions and actual Hermes
   backend discovery. Log failures with the kernel, package and DKMS versions.

## Exceptions and recovery

- **Bazzite / Silverblue / bootc / ostree:** do not run a DKMS package install
  into a read-only `/usr` or treat the Fedora Workstation RPM as sufficient.
  Use the image-based `packaging/bazzite/Containerfile` workflow and build
  the module against the image's kernel; see the main README. A `bootc switch`
  can discard an rpm-ostree-layered Hermes host package; re-layer/rebuild it.
- **Secure Boot:** DKMS must build and sign using a trusted, enrolled key
  according to the distribution's policy. Never automatically disable Secure
  Boot. Untrusted unsigned modules may build but cannot be loaded.
- **Downgrade or rollback:** retain old packages and their checksums.
  Reinstall matching core+broker packages and reboot rather than force-unload.
- **Nonstandard kernels:** if no exact header/devel tree exists, show the
  missing dependency and fail the driver stage, but leave unrelated ecosystem
  components alone.
- **Safety:** leave `/etc/modprobe.d`, user seat selection, physical monitor
  mode and other user customizations intact unless explicitly approved.
  Core render-node access must remain read-only and isolated seatd brokers must
  keep `SEATD_VTBOUND=0`.

## Where the packages come from

- Debian/Ubuntu packaging: `debian/`, build with
  `dpkg-buildpackage -b -us -uc`.
- Fedora: `packaging/rpm/hermes-kms.spec`, build with `rpmbuild` from the
  exact release commit archive.
- Arch/CachyOS: `PKGBUILD`, with release CI pinning `#commit=<SHA>`.
- CI validates each package format independently. The **published** v0.4.1
  release contains the verified Arch, Debian/Ubuntu and Fedora packages,
  source archive and SHA-256 checksums.

The future **Hermes repository** installer may handle Hermes, Hestia,
libvirtualhid and a Gamescope fork independently, but they must not become
prerequisites of the kernel-only core driver package. No working ecosystem
installer is included in the Hermes-KMS v0.4.1 assets.
