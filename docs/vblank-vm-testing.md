# Testing the vblank timer safely in a VM (lockdep)

The software vblank timer (branch `experimental-vblank-timer`) soft-locked the
desktop 3x on the host. The host kernel lacks the debug options that would catch
the bugs *before* they hang the machine:

```text
CONFIG_PROVE_LOCKING      is not set   # would catch lock-ordering / deadlocks
CONFIG_DEBUG_ATOMIC_SLEEP is not set   # would catch sleeping in atomic context
                                       # (the hrtimer_cancel-in-disable_vblank bug)
```

So we test the module in a throwaway VM running a kernel with those enabled.
modetest drives the display (no KWin), which isolates the kernel-level bugs
(soft-lock, refcount, lock ordering) — exactly the ones that hung the host.

## Approach: virtme-ng (packaged on CachyOS)

virtme-ng builds a kernel from source with the configs we want and boots it in a
VM in seconds, mounting the host filesystem. No OS install needed. It is
packaged: `virtme-ng 1.41`.

### 1. Install tooling (host, one-time)

```bash
sudo pacman -S --needed qemu-base virtme-ng
```

### 2. Get a kernel source tree with debug enabled

```bash
# A kernel source tree (mainline or CachyOS). Example with mainline:
git clone --depth 1 https://github.com/torvalds/linux.git ~/linux-debug
cd ~/linux-debug

# Build with virtme-ng, enabling the lock/atomic debuggers that catch our bugs.
# --config appends extra Kconfig options on top of a sane default.
vng --build \
    --config <(printf '%s\n' \
       CONFIG_PROVE_LOCKING=y \
       CONFIG_DEBUG_ATOMIC_SLEEP=y \
       CONFIG_DEBUG_KERNEL=y \
       CONFIG_LOCKDEP=y \
       CONFIG_DEBUG_LOCK_ALLOC=y \
       CONFIG_PROVE_RCU=y \
       CONFIG_DRM=y CONFIG_DRM_KMS_HELPER=y)
```

(Add `CONFIG_KASAN=y CONFIG_KASAN_GENERIC=y` to also catch use-after-free /
refcount leaks; slower.)

### 3. Boot the VM and test the module

Build the Hermes-KMS module against that kernel tree, then run inside the VM:

```bash
# From the Hermes-KMS repo, build against the debug kernel:
make modules KDIR=~/linux-debug

# Boot the VM with the repo mounted, run the test, capture dmesg:
cd ~/linux-debug
vng --run -- bash -c '
  cd /home/ozzy/Projects/Hermes-KMS
  insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1 hotplug_events=0
  modetest -M hermes-kms -a -s ... -P ...   # see scripts/test-driver-zero-copy.sh
  dmesg | grep -iE "BUG|lockup|WARNING|RIP|atomic|sleeping|hermes"
  rmmod hermes_kms      # must succeed cleanly (no refcount leak)
'
```

Even simpler: `vng` mounts the host fs, so `scripts/test-driver-zero-copy.sh`
can be run inside the VM directly once the module is built for the VM kernel.

What lockdep/atomic-sleep will flag if present:

- "sleeping function called from invalid context" → an hrtimer_cancel or mutex
  in atomic context (disable_vblank etc.).
- "possible circular locking dependency" → lock-ordering bug.
- A WARN/RIP backtrace pinpointing the exact line.

## What to fix once the VM reproduces the bugs

1. The benign WARN in `drm_crtc_init_with_planes` during probe (a diagnostic
   `drm_dbg_kms` prints the primary plane type).
2. KWin adopting the virtual connector and blanking the physical monitor —
   `DRM_CONNECTOR_POLL_CONNECT` leaks the display; likely needs `non_desktop`
   default or removing the poll flag for the isolated/streaming case.
3. Any lockdep/atomic-sleep splat the debug kernel surfaces.

## Why this matters for the product

FPS caps at ~35-40 because KWin composes virtual outputs on the output's vblank
cadence. A clean 60/120/144Hz vblank timer is the lever to reach full refresh
and to cut the pacing spikes (Hermes-KMS had ~2x EVDI's spike rate). Getting the
vblank timer stable is the path to a premium streaming experience.
