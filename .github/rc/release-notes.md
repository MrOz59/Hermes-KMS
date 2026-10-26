# Hermes-KMS — October 2026 release candidate 1

Coordinated RC for testing before the end-of-October stable release. This is a **prerelease**, not a stable release.

- Source commit: [`8c8aa0812783437c91e6d65ff8073fdbe57068b2`](https://github.com/MrOz59/Hermes-KMS/commit/8c8aa0812783437c91e6d65ff8073fdbe57068b2)
- Validated build: [Actions run 40264831106](https://github.com/MrOz59/Hermes-KMS/actions/runs/40264831106)
- Embedded build version: `0.4.1`

This is an immutable promotion of the exact successful CI artifacts. Package/APK bytes, embedded build versions, filenames and signatures are preserved; the shared `october-2026-rc1` tag identifies the coordinated candidate. `RC_MANIFEST.json` records each source/build/artifact identity, and `SHA256SUMS` covers all attached payload files.

## Included

- Driver 0.4.1 candidate from PR #18, with unchanged UAPI **14**.
- Read-only render-node ACL/access fixes, Linux 6.17/6.18 timer compatibility and VT-free private seat brokers.
- Both split Arch/CachyOS packages, source archive, pinned PKGBUILD and install hooks.
- Userspace/ABI/EDID checks, Arch/Ubuntu/Fedora module builds, package manifests and module compilation from the actual DKMS package payload passed.

Stop active streams before upgrading. Install the DKMS package with matching kernel headers, add the optional seatd package if using private brokers, confirm `dkms status` and reboot to activate the new module. Keep the previous packages for rollback. The pending `fix/unload-post-detach-holders` branch and experimental NVIDIA probes are outside this candidate.

## Matching candidates

- [Hermes RC1](https://github.com/MrOz59/Hermes/releases/tag/october-2026-rc1) — `964c5fa24b4992f942a59142e82b3adee8214de9`
- [Hestia RC1](https://github.com/MrOz59/Hestia/releases/tag/october-2026-rc1) — `94a1bc58b510f7417bd208ed70cd22d2b89ed649`
- [Hestia-Android RC1](https://github.com/MrOz59/Hestia-Android/releases/tag/october-2026-rc1) — `0ef14edfd152b4d220d3f2992c82c101e582ca44`
- [Hermes-KMS RC1](https://github.com/MrOz59/Hermes-KMS/releases/tag/october-2026-rc1) — `8c8aa0812783437c91e6d65ff8073fdbe57068b2`

## Remaining validation

Real-host AMD capture/encode, desktop and Android streaming, SDR/HDR, disconnect/reconnect, concurrent isolated Gamescope sessions, DS4/optional DualSense, private audio/microphone and upgrade/rollback checks remain part of RC testing. Passing CI does not record those hardware tests as completed. PyroWave HDR is outside the supported candidate path; use the existing codecs for HDR.

When reporting a problem, include this RC tag, component build version, GPU/driver/kernel, client device and codec, and logs from both ends.
