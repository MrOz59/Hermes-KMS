# KWin render bridge (experimental)

Status: **experimental, NVIDIA + KWin 6.8, not yet validated on real
hardware.** The bridge is built and tested end to end against fake libgbm,
libdrm and libkwin (`make check-kwin-bridge`) and against glibc's
secure-execution loader with a capability-bearing stand-in for
`kwin_wayland`. Whether a real NVIDIA + KWin 6.8 session then streams
zero-copy is what the hardware test below has to establish.

## The problem it removes

[kwin-nvidia-prime-scanout.md](kwin-nvidia-prime-scanout.md) traced why a
Hermes-KMS output is still CPU-copied on NVIDIA: KWin 6.8 asks
`GpuManager::compatibleRenderDevice()` which render device serves the
Hermes-KMS card, finds none with a rendering engine, rejects `GpuCopy`, and
falls back to `MultiGpuImportMode::DumbBuffer`. It renders the output on the
NVIDIA GPU, reads it back with the CPU into a system-memory dumb buffer, and
Hermes, which NVIDIA cannot import that buffer for, copies it once more.

That document proposed a KWin change. The bridge gets the same result
without modifying KWin, by preloading a small library into `kwin_wayland`.
The approach (a GBM allocator for a display-only virtual card opened on a
real GPU's render node, installed as a set-user-ID trust-marked preload)
comes from Vibepollo's Linux host
([Nonary/libvirtualdisplay](https://github.com/Nonary/libvirtualdisplay),
`linux/kwin_gpu_interposer.cpp`, MIT). The hook points, the device policy
and the safety rules here are this project's own, written against KWin's
`Plasma/6.8` source.

## What it changes

Only inside `/usr/bin/kwin_wayland`, only for Hermes-KMS **primary** nodes:

1. **Allocator.** `gbm_create_device()` for a Hermes-KMS card returns a GBM
   device on the selected GPU's render node. KWin keeps the card's own KMS fd
   for every DRM ioctl (modeset, `ADDFB2`, PRIME import, atomic commit).
2. **Dumb buffers.** KWin 6.8 allocates dumb buffers (the cursor, the
   QPainter fallback) through `gbm_device_get_fd()`. On a routed device that
   call returns a duplicate of the card's KMS fd when, and only when, the
   caller is `libkwin`; GBM and EGL keep the render fd.
3. **Render device.** `KWin::GpuManager::compatibleRenderDevice()` (both the
   `DrmDevice *` and the `dev_t` overload) answers, for a routed card's
   primary or render node, with the RenderDevice KWin already has for the
   selected GPU.

With (3), the card's render device equals the compositor's, so
`EglGbmLayerSurface::createSurface` takes `MultiGpuImportMode::None`: KWin
renders straight into scanout buffers allocated by (1) and registers them on
the card through `DrmGpu::importBuffer` (`drmPrimeFDToHandle` +
`drmModeAddFB2WithModifiers`). Hermes-KMS already re-exports an imported
scanout as its original DMA-BUF and, since UAPI 15, reports it as imported
from `nvidia_drm` (the exporting module), which Hermes's
`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto` routes to the EGL/CUDA import
instead of the CPU copy.

`None` needs a layout the NVIDIA GPU renders that the virtual plane also
advertises. NVIDIA does not render to linear, and Hermes-KMS advertises only
linear unless `scanout_modifiers=` lists more, so **publishing the NVIDIA
layouts is part of the setup** (see below). Without them KWin moves on to
`GpuCopy`, which the bridge also unlocks: for a discrete GPU that is KWin's
double copy, GPU to system memory and back into an NVIDIA buffer, done by the
GPU's copy engines rather than the CPU. The scanout is then imported from
NVIDIA too, but it is not zero-copy.

## Safety rules

- **Pass-through everywhere else.** The library links only libc and reaches
  libgbm/libdrm through symbols the compositor already loaded. Loaded into
  `kwin_wayland_wrapper` or any other process, its constructor does nothing
  and every exported function forwards to the real one.
- **Reviewed KWin only.** It enables itself only when every KWin entry point
  it relies on resolves into the same library and that library is
  `libkwin.so.6.8.*`. Another release logs why and stays off unless
  `HERMES_KMS_KWIN_BRIDGE=force`.
- **One routed allocator per card.** A second live GBM device for the same
  card is DrmGpu's software-rendering fallback RenderDevice (created only
  without `/dev/udmabuf`), which builds an EGL display on it; it stays on the
  card's native GBM.
- **Primary nodes only.** A Hermes-KMS render node is never routed, so KWin
  can never mistake it for a second NVIDIA GPU (which would also re-enable
  udmabuf import on NVIDIA and turn SHM clients black).
- **Redirect only what is routed.** The render-device answer changes only
  while a routed allocator for that card is alive, so a card whose routing
  failed keeps KWin's native path rather than mixing an NVIDIA renderer with
  a system-memory allocator, which is the import NVIDIA corrupts.
- **No silent scanout failure.** If the GPU refuses `GBM_BO_USE_SCANOUT` on
  its render node, the bridge retries the allocation without it (Hermes-KMS
  never scans out physically) and logs it once.
- **Executing the library does nothing.** Its ELF entry point exits before
  any libc code runs, so the set-user-ID marker cannot be used as a program.

## Requirements

- KDE Plasma 6.8 started through systemd (`plasma-kwin_wayland.service`,
  Plasma's default). The drop-in does not reach a KWin started any other way,
  and does not cover the SDDM/Plasma Login greeter.
- An NVIDIA GPU with a render node, the NVIDIA GBM backend, and Hermes-KMS
  from the `experimental/nvidia-zero-copy` branch (UAPI 15 frame origin).
- Hermes from its `experimental/nvidia-zero-copy` branch with
  `HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto` in `hermes.service`.
- `scanout_modifiers=` listing the NVIDIA layouts (below).
- `/dev/udmabuf` present (CachyOS and Arch kernels have it).

## Install

Arch/CachyOS, from this branch:

```bash
makepkg -si   # builds hermes-kms-dkms-git, hermes-kms-seatd-git and hermes-kms-kwin-bridge-git
```

Install only the bridge from the built packages if you prefer
(`pacman -U hermes-kms-kwin-bridge-git-*.pkg.tar.zst`). From a clone:

```bash
make kwin-bridge
sudo make install-kwin-bridge       # KWIN_BRIDGE_LIBDIR=/usr/lib64 or the
                                    # multiarch directory off Arch
```

This installs `/usr/lib/libhermes-kms-kwin-bridge.so` (mode `4755`) and
`/usr/lib/systemd/user/plasma-kwin_wayland.service.d/60-hermes-kms-kwin-bridge.conf`.
Log out and back in, or reboot: KWin loads the bridge only when it starts.

`kwin_wayland` carries `cap_sys_nice`, so glibc runs it in secure mode and
preloads only a bare library name from its trusted directory whose file has
the set-user-ID bit. The bit is that trust marker, nothing more; the
alternative, stripping KWin's capability, would cost every user KWin's
realtime scheduling.

### Publish the NVIDIA layouts

For the direct path (no copy at all), let the virtual plane advertise the
layouts the NVIDIA GPU renders:

```bash
make tools
tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test \
  --list-modifiers --gpu /dev/dri/renderDN --device /dev/dri/cardN
```

and put the suggested `scanout_modifiers=` value in
`/etc/modprobe.d/hermes-kms.conf`, then reboot. Tiled frames can only be read
by an NVIDIA import, so an encoder on another GPU cannot use them.

## Verify

After logging in, the compositor's journal:

```bash
journalctl --user -b -u plasma-kwin_wayland.service | grep hermes-kms-kwin-bridge
```

| Line | Meaning |
|:-----|:--------|
| `active in kwin_wayland (libkwin.so.6.8.N)` | Loaded and enabled |
| `Hermes-KMS card 226:N allocates on /dev/dri/renderDM (...)` | Allocator routed |
| `KWin renders Hermes-KMS card 226:N on ...` | Render device answered for the card |
| `first GPU buffer for card 226:N: ... flags 0x5` | KWin allocated an NVIDIA scanout buffer (`0x5` = rendering + scanout) |
| `... before asking for its render device ...` | This KWin build resolved the lookup internally (for instance with LTO); the output keeps the CPU copy |
| `... has not been reviewed ...` / `lacks the 6.8 ... entry points` | Different KWin; bridge off |

The first three lines appear at login, before any stream. The fourth appears
when the virtual output is first enabled. Then, during a stream, Hermes
should log `Hermes-KMS NVENC auto route: scanout modifier 0x... (imported
from nvidia_drm) was allocated by the NVIDIA GPU` followed by `Hermes-KMS
zero-copy capture ready`, and `hermes-kmsctl` / debugfs should report the
framebuffer as imported. Check moving content across the whole image, a
resolution change, a disconnect and reconnect, log out while streaming, and
a reboot with the output enabled before calling it validated.

## Configuration

Set in `systemctl --user edit plasma-kwin_wayland.service`, then log out and
back in.

| Variable | Effect |
|:---------|:-------|
| `HERMES_KMS_KWIN_BRIDGE=off` | Disabled; KWin behaves as without the bridge |
| `HERMES_KMS_KWIN_BRIDGE=force` | Enable on an unreviewed KWin release (testing only) |
| `HERMES_KMS_KWIN_RENDER_PCI=0000:01:00.0` | Use this GPU instead of the lowest-addressed NVIDIA GPU; any vendor, so AMD/Intel testers can try it explicitly |

`HERMES_KMS_KWIN_BRIDGE_ACTIVE=1` is the drop-in's marker; without it the
library stays inert even inside KWin.

## Known limits and risks

- **KWin internals.** The render-device hook depends on KWin calling
  `compatibleRenderDevice()` across a library boundary the dynamic linker can
  interpose. A KWin built with LTO or `-fno-semantic-interposition` may
  resolve it internally; the bridge then logs the warning above and the
  output keeps today's CPU copy. It never breaks the output: dumb buffers
  still reach the card.
- **New KWin releases** stay off until reviewed. Plasma 6.9 needs this
  document's three hook points rechecked against its source.
- **Primary GPU.** The bridge does not choose KWin's compositing GPU. On a
  hybrid machine compositing on another GPU, KWin takes `GpuCopy` across GPUs
  into an NVIDIA buffer; set `HERMES_KMS_KWIN_RENDER_PCI` to the compositing
  GPU if that is not what you want.
- **Lifetime.** The card now holds NVIDIA-exported buffers while the output
  is on. KWin releases them at logout; a forced reboot with a stream running
  is part of the validation above.
- **Direct scanout of clients.** Because the card now maps to the NVIDIA
  render device, KWin may scan a fullscreen client's NVIDIA buffer straight
  out on the virtual output, which Hermes-KMS re-exports like any import.

## Removing it

`pacman -R hermes-kms-kwin-bridge-git` or `sudo make uninstall-kwin-bridge`,
then log out and back in.

## Tests

`make check-kwin-bridge-policy` (part of `make check`) covers the pure
decisions. `make check-kwin-bridge` builds the bridge with fake libgbm,
libdrm and two fake libkwin releases, a fake sysfs and a scenario driver
posing as `kwin_wayland`, and checks routing, the render-device answers, the
dumb-buffer fd, the scanout retry, the release gate, the GPU selection rules,
`LD_PRELOAD` cleanup, inertness outside KWin and the exit-only entry point.
