# hermes-kms-kwin-bridge

Experimental `kwin_wayland` preload that lets KWin 6.8 render Hermes-KMS
virtual outputs on the NVIDIA GPU and PRIME-import its own scanout buffers,
instead of reading every frame back into a system-memory dumb buffer.

- Build: `make kwin-bridge` (libdrm headers only; links libc alone)
- Install: `sudo make install-kwin-bridge`, or the `hermes-kms-kwin-bridge-git`
  package, then log out and back in
- Test without a GPU: `make check-kwin-bridge-policy check-kwin-bridge`

How it works, the safety rules, configuration and how to verify it on real
hardware: [docs/kwin-render-bridge.md](../../docs/kwin-render-bridge.md).
