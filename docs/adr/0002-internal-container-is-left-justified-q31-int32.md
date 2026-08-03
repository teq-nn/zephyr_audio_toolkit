# The internal sample container is a left-justified Q31 `int32_t`

**Status:** accepted

Every sample in a pipeline buffer is a signed 32-bit integer holding a left-justified, full-scale value — `AUDIO_SAMPLE_FORMAT_S32_LE`, the only value the format enum carries. A source normalizes its wire depth into that container by shifting up: the file reader does `s32 = s16 << 16` (`file_reader_node.c:80`), the I2S wire does the same on the way in (`audio_i2s_wire.c:106`), and the tone generator synthesizes into it directly (`tone_gen_node.c:132`). The reason for 32 bits is that one container at one fixed scale removes per-depth code paths from every filter: a coefficient means the same thing whether the audio arrived as 16, 24 or 32 bit, and `gain_filter_node.c` needs no knowledge of where it came from.

That is the whole reason. Two rationales that sound plausible are wrong, and this ADR exists mostly to say so.

**It is not "we wanted to avoid floats."** Avoiding floats explains *not* `float`; it does not select 32 over 16, since `int16_t` avoids floats equally well. Anyone reasoning from that premise can halve the container and be consistent with it.

**It is not headroom.** The container has none. Because samples are left-justified, a full-scale 16-bit input already sits at roughly `INT32_MAX` after its shift — there is no room above the wire depth, at any wire depth. Headroom lives in the filter's intermediate, not in the container: `gain_filter_process()` widens to `int64_t`, applies the Q15 gain and shifts back (`gain_filter_node.c:38-43`). Widening the container would not change this, because the intermediate is where a gain above unity actually needs the room.

`valid_bits_per_sample` stays a separate field describing the *wire*, not the container. It is part of the format bound by `audio_pipeline_set_format()` and validated by each node's `open()`, exactly as ADR 0001 requires — a node reads it to decide what it can carry, not to discover what scale the samples are at. The samples are always Q31.

## Considered options

**Right-justified with guard bits** — value in the low 24 bits, 8 bits of headroom above. Rejected on three counts. It does not remove the need to saturate on the store back down; it only moves where the wrap happens. It introduces a headroom convention every node must know and preserve, so a node that ignores it silently produces samples 8 bits too loud. And it makes the wire conversion `shift by (32 - depth - guard)` — depth-dependent arithmetic reintroduced in a subtler form, which is precisely what the single container was adopted to eliminate. True 32-bit sources would also need lossy down-scaling to fit beneath the guard.

**A 16-bit container.** Consistent with "avoid floats" and half the buffer memory, but it forces the wire depth into the container, so 24- and 32-bit input has to be either truncated at the source or carried in a second format the filters branch on. It also leaves nowhere to put a gain result without an immediate narrowing.

**`float` as the internal format.** Genuinely viable on the M4/M7-class targets in scope, which have an FPU, so this was not rejected on cost. It is out of scope for v1 because every shipped node hard-assumes `int32_t` samples: admitting a second container format changes what a node agrees to when it accepts the bound format under ADR 0001, which is real work rather than a new enum value. See the consequences below — this is an extension point, not a closed door.

## Consequences

- **Nothing saturates on the way down.** `gain_filter_process()` casts the `int64_t` intermediate back to `int32_t` with no clamp, so a gain above unity on a near-full-scale sample wraps rather than clips — loud positive becomes loud negative. This is a real defect that follows directly from the container having no headroom, and it is tracked in #39. Any filter that widens for an intermediate owes a clamp on the store.
- **`int64_t` intermediates are cheap, but only above ARMv6-M.** On Cortex-M4/M7 a 32×32→64 multiply is a single `SMULL`, and CMSIS-DSP is built the same way — `q31_t` data with 64-bit accumulators. On Cortex-M0/M0+ there is no `SMULL` and the same expression becomes an `__aeabi_lmul` call. M0-class parts are out of scope; if that changes, the filter arithmetic needs revisiting, not the container.
- **Float remains an extension point.** The format enum exists to be widened, and `valid_bits_per_sample` already separates wire depth from container, so a `AUDIO_SAMPLE_FORMAT_F32` is expressible without reshaping the config. What it would cost is per-node: each node's `open()` would have to refuse or handle the new format, and ADR 0001's consequences need re-reading, since the bound format is what a node is agreeing to.
- **The 24-bit path is still unbuilt, and this decision does not build it.** `file_reader_node.c` rejects anything but 16-bit with `-ENOTSUP` and the I2S wire accepts only 16-bit words (`audio_i2s_wire.c:31`). Left-justification is what makes 24-bit a shift of 8 rather than a new code path when it does arrive.
- **v1's remaining non-goals are recorded nowhere.** They were in the deleted spec documents and in no ADR. This ADR settles only the container question that was entangled with them.
