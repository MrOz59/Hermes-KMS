# HDR capture contract (UAPI v14)

## Scope and representation

Hermes-KMS is a pixel-preserving virtual RGB sink. The HDR path supports static
HDR10 signalling: BT.2020 RGB, ST2084/PQ, and CTA type-1 static metadata. It does
not implement HLG, Dolby Vision, HDR10+, YUV scanout, tone mapping or encoding.
Those are separate features, not synonyms for ten-bit framebuffer support.

Enable advertisement with `hdr_enable=1 color_depth=10`. The default stays SDR.
The existing CTA blocks advertise PQ and BT.2020, and the connector exposes
`Colorspace` and `HDR_OUTPUT_METADATA`. The compositor chooses its output state.
Eight-bit PQ is permitted (for compositor dithering); format and transfer function
are independent. For a ten-bit streaming path verify the acquired fourcc.

After driver identification, capability discovery and session authorization,
require `HERMES_KMS_CAP_FRAME_COLOR`. Zero a `drm_hermes_kms_acquire_frame2`, set
`frame.flags` to the desired buffer/fence requests, and call `ACQUIRE_FRAME2`.
Never supplement an old acquire with a separate query of current KMS properties:
that races output transitions and cannot identify the captured samples.

The response includes:

- The unchanged frame descriptor, including sequence, format/modifier, damage,
  DMA-BUFs and explicit producer fence.
- `COLOR_VALID` and `COLOR_RGB_FULL_RANGE`: RGB sample interpretation. An encoder
  may convert these to limited-range YUV; that does not change the source range.
- `colorspace`: Default (0, the virtual sink's sRGB primaries) or BT.2020 RGB (9).
- `COLOR_HDR_VALID`: a committed HDR blob is present. Inspect `hdr` only with this
  flag. The flag alone does not mean PQ: EOTF 0 denotes SDR; EOTF 2 denotes PQ.
- The DRM `hdr_output_metadata` fields, using the exact units from `drm_mode.h`:
  chromaticities in 1/50000, maximum mastering luminance/CLL/FALL in cd/m²,
  minimum mastering luminance in 1/10000 cd/m². Zero values are unspecified.
  Mastering primaries describe the content's mastering display, not the RGB
  encoding primaries. Do not replace BT.2020 interpretation with those primaries.

The structure is 224 bytes on LP64 and ILP32. Existing ioctl numbers, sizes and
reserved fields are untouched. The new colour section must be zero on input;
unknown request flags and nonzero reserved input fail with EINVAL. No kernel
pointers, property blob IDs, or application-specific information cross this ABI.

## Atomicity and lifecycle

The atomic transaction holds its connector state until flush completes. Flush
copies validated metadata into the same state-lock snapshot as framebuffer,
sequence and damage. Blob memory and padding are not exported. This avoids both
query/acquire races and a later nonblocking commit replacing live connector state
before an earlier flush reads it.

Changing only colour properties joins the primary plane to the transaction,
advances `frame_sequence`, wakes `WAIT_FRAME`/`WAIT_UPDATE` and invalidates damage.
An encoder must reprocess the full frame on that transition. Identical metadata
on a cursor-only commit does not advance the primary stream. Clearing the blob,
disabling scanout or ending a session clears stored HDR data. TEST_ONLY and failed
commits do not publish state. The existing export sequence checks return ESTALE
if frame replacement races fd export; retry with a small bounded loop.

Authorization, token revocation and fence requirements are shared with the old
acquire implementation. Revocation prevents new exports but cannot recall fds
already returned. Wait for the producer fence before sampling, and close every
returned fd even when conversion or encoding fails.

## Consumer integration and production release gates

The accompanying Hermes changes consume `ACQUIRE_FRAME2`, preserve ten-bit RGB,
and reinitialize encoding when frame colour state changes. Updating the driver
alone cannot enable correct HDR streaming in an older consumer. A consumer must:

1. Discover the new capability and acquire colour metadata with every frame.
   Treat absent capability as unknown colour support, not proof that an output
   is SDR. Keep HDR disabled on that integration until the contract is supported.
2. Import all negotiated ten-bit RGB formats/modifiers, preserving precision.
   Convert RGB to P010 on the real GPU with the correct BT.2020 non-constant-
   luminance matrix, range and PQ interpretation. Do not apply PQ twice.
3. Select a ten-bit encoder profile (for example HEVC Main10), set transfer,
   primaries, matrix and range correctly in the bitstream, and propagate the
   static metadata without manufacturing unspecified luminance values.
4. Handle SDR/HDR and metadata-only changes before submitting the affected
   frame; drain/reconfigure the encoder and signal the client as appropriate.
   Explicitly negotiate SDR fallback/tone mapping for clients without HDR.
5. Validate cursor composition separately. The legacy cursor stream contains no
   independently associated colour metadata. During HDR, use compositor-baked
   cursors until the consumer and compositor agree on cursor colour semantics.
   Blending an sRGB cursor directly into PQ samples is incorrect; conversion and
   linear-light blending are consumer responsibilities.

The initial release target is KDE/KWin, AMD VAAPI and NVIDIA NVENC. NVIDIA
retains the existing CPU scanout copy; colour conversion and encoding remain
hardware accelerated. NVIDIA/KWin zero-copy work is explicitly deferred.
Release validation requires each compositor and GPU backend claimed by the
product, and an
HDR-capable client/display. Test ramps (no eight-bit truncation), reference white,
near-black, saturated BT.2020 colours, clipping, metadata changes, SDR fallback,
reconnects, multi-output isolation, dropped frames and cursor visibility. Inspect
encoded bitstream metadata as well as visual output. A headless VM can validate
DRM and UAPI behaviour, but cannot establish these GPU/display properties.

## Reproducible checks

`make check` pins the public ABI, session helper and generated EDID. Parser
conformity compares the existing tracked findings; it does not claim a pristine
EDID conformity report. `make check-hdr-build` compiles the integration test.
`tests/hdr-capture.c` is run by `scripts/vm-hdr-capture-test.sh` only in a disposable
VM. It covers ten-bit SDR, metadata-only HDR commits, TEST_ONLY, rejection of
malformed/unsupported state, nonblocking transitions, old ABI access and token
revocation. No compositor or hardware encoder is required for this test.

Reference: [Linux DRM/KMS connector properties and atomic state](https://docs.kernel.org/gpu/drm-kms.html)
and [DRM userspace HDR metadata definitions](https://docs.kernel.org/gpu/drm-uapi.html).
