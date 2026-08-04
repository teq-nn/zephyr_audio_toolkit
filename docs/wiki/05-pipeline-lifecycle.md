# Pipeline lifecycle and control API

The whole control API is seven functions and one state field. This article is the
reference: the state machine, what each entry point does, every error code it can return,
and the sequences you will actually write.

## The five states

The state is a single `atomic_t` on the instance, holding a value private to the subsystem
(`enum audio_pipeline_state`, `audio_internal.h`):

| State | Worker thread | Node chain | How you observe it |
| --- | --- | --- | --- |
| `UNINIT` | no | closed | every entry point returns `-EINVAL`; **zero means uninitialised**, so a zeroed instance is uninitialised by construction |
| `INIT` | no | closed | `is_running() == false` |
| `OPEN` | yes | open | `is_running() == true`, `is_playing() == false` |
| `PLAYING` | yes | open | `is_playing() == true` |
| `CLOSED` | yes | closed | `is_running() == true`, `is_playing() == false` — where a **node error** leaves you |

`CLOSED` is the one that surprises people: after a processing error the worker tears the
chain down but stays alive, so `is_running()` keeps saying yes. `start()` reopens the chain
onto that same thread.

## The transition table

```
  UNINIT ──init()──► INIT ──start()──► OPEN ──play()──► PLAYING
                                        ▲                  │
                                        └──── stop() ──────┤
                                        └── EOF (worker) ──┘

  OPEN | PLAYING ──node error (worker)──► CLOSED ──start()──► OPEN
                                          (worker thread stays alive,
                                           the chain is reopened on it)

  INIT | OPEN | PLAYING | CLOSED ──join()──► INIT
```

Written out — this is `pipeline_transitions[]` in `audio_pipeline_core.c` verbatim:

| Trigger | Legal from → to |
| --- | --- |
| `init()` | `UNINIT → INIT`, `INIT → INIT` |
| `start()` | `INIT → OPEN`, `CLOSED → OPEN` |
| `play()` | `OPEN → PLAYING`, `PLAYING → PLAYING` |
| `stop()` | `PLAYING → OPEN`, `OPEN → OPEN`, `INIT → INIT`, `CLOSED → CLOSED` |
| EOF (worker) | `PLAYING → OPEN` |
| node error (worker) | `PLAYING → CLOSED`, `OPEN → CLOSED` |
| `join()` | `PLAYING`/`OPEN`/`CLOSED`/`INIT → INIT` |

Anything not listed is refused, and the refusal is what produces the documented errno.
Notable absences:

* **No `start()` row from `OPEN` or `PLAYING`.** `start()` has nothing to do there and moves
  nothing — that is what makes it idempotent. An identity row would let a `start()` racing a
  node error re-declare a chain the worker just closed as open.
* **No `play()` row from `INIT` or `CLOSED`.** `INIT` has no thread, `CLOSED` has no chain;
  both are `-EPERM`.
* **No EOF row from `OPEN`.** If `stop()` won the race, it has already done the job.

## Entry points

### `audio_pipeline_init(pipeline, config, sink)`

Binds configuration and topology to a **zero-initialised** instance. Does not touch the
nodes and does not create the thread. It also:

* claims the built-in stack / frame buffer / event slots for every resource field left
  `NULL`, all-or-nothing, so a refusal leaves the instance untouched;
* clamps `frame_capacity` to `MIN(allocated, config->frame_samples)`;
* (re)initialises the event queue, so a rebound instance never delivers events from its
  previous life;
* **clears the bound format** — a carried-over format would be a default nobody chose.

| Return | Means |
| --- | --- |
| `0` | bound |
| `-EINVAL` | NULL argument, or `frame_samples` is 0 or above `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES` |
| `-EBUSY` | this instance's worker is still running, or another instance holds a built-in resource this one needs |

`audio_pipeline_config_is_valid()` checks the same configuration without binding it.

### `audio_pipeline_set_format(pipeline, fmt)`

The **only** way to bind a format. `fmt` is copied, so a temporary is fine. Legal only
while the chain is closed: before the first `start()`, or after `join()`.

| Return | Means |
| --- | --- |
| `0` | bound |
| `-EINVAL` | NULL argument, uninitialised pipeline, `sample_rate_hz == 0`, `channels == 0`, or `channels > frame_capacity` |
| `-EBUSY` | the node chain is open (playing or merely idle) |

### `audio_pipeline_start(pipeline)`

Opens the chain and, from `INIT`, creates the worker thread. Nodes are opened **sink first,
then walking upstream**, so a sink can hand resources to the nodes feeding it. The bound
format is installed on each node immediately before that node's `open()`.

If a node's `open()` fails, the nodes already opened are closed again, an
`AUDIO_PIPELINE_EVENT_ERROR` is published and the failure is returned; **no thread is
created**, and the state does not move. The worker starts out **idle** — `play()` begins
the pulling.

| Return | Means |
| --- | --- |
| `0` | chain open (also when already started — idempotent) |
| `-EINVAL` | NULL pipeline, no sink, or uninitialised |
| `-ENODATA` | no format bound — call `set_format()` first |
| `-EBUSY` | another instance took over a built-in resource this one was joined out of |
| `-ELOOP` | the upstream chain is deeper than `AUDIO_PIPELINE_MAX_CHAIN_DEPTH` (16) |
| `-ENOTSUP` | a node cannot deliver or accept the bound format |
| `< 0` | the first node `open()` error, verbatim |

### `audio_pipeline_play(pipeline)`

Starts or resumes pulling. Releases the worker's wake semaphore only when coming from
`OPEN`, so an already-playing pipeline is never handed a spare count.

| Return | Means |
| --- | --- |
| `0` | pulling (also when already playing) |
| `-EINVAL` | NULL or uninitialised |
| `-EPERM` | no worker thread (`INIT`), or the chain is closed (`CLOSED`) |

### `audio_pipeline_stop(pipeline)`

Halts pulling; the thread stays alive and idles, nodes stay open, `play()` resumes on the
same thread. **Asynchronous by design** — a sink is allowed to block inside `process()`, and
`stop()` must not deadlock behind it, so the frame in flight may still complete.

Returns `0` in every initialised state, `-EINVAL` otherwise.

### `audio_pipeline_join(pipeline)`

Ends the worker thread and closes the chain. Blocks until the thread has left its loop, so
**never call it from the worker thread**. Idempotent, and leaves the instance exactly as
`init()` produced it, so `start()` can be called again.

It also releases any built-in resource this instance holds. Between the join and the next
successful `start()` the instance owns nothing even though it still points at the built-ins.

| Return | Means |
| --- | --- |
| `0` | thread gone, chain closed |
| `-EINVAL` | NULL or uninitialised |
| `< 0` | the first node `close()` error (also published as an ERROR event) |

> An instance running on the built-ins **must** be joined before it goes out of scope.
> Nothing else gives them back, and an abandoned one locks every later hand-rolled
> pipeline out with `-EBUSY`.

### `audio_pipeline_process_frame(pipeline)`

Pulls exactly one frame through the chain, synchronously, on the calling thread. This is
what the worker loop calls; it is exposed for tests and for applications that want to drive
the pipeline from their own thread instead of `play()`/`stop()`.

| Return | Means |
| --- | --- |
| `0` | a frame was produced |
| `-EPIPE` | end of stream (the sink produced 0 samples) |
| `-EINVAL` | NULL pipeline, no sink, or no frame buffer |
| `-ENOSYS` | the sink has no `process` op |
| `< 0` | the node error that aborted the frame |

> ⚠️ It has **no** guard against a joined instance. Do not call it on one, or it will read
> and write the frame buffer the new owner is using.

### `audio_pipeline_is_running()` / `audio_pipeline_is_playing()`

`is_running()` is true in `OPEN`, `PLAYING` and `CLOSED` — i.e. "there is a worker thread".
`is_playing()` is true in `PLAYING` only.

## Events

```c
enum audio_pipeline_event_type {
	AUDIO_PIPELINE_EVENT_EOF = 0,
	AUDIO_PIPELINE_EVENT_ERROR,
	AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event { enum audio_pipeline_event_type type; int err; };
```

Only `EOF` and `ERROR` are ever published in v1 — nothing in the subsystem raises
`RECONFIG`. Handle it in a `default:` arm and move on.

Two paths, and the queue is the primary one:

* **Queue** — `audio_pipeline_get_event(pipeline, &event, timeout)`, plain `k_msgq`
  semantics: any thread, in publication order, `K_NO_WAIT` / `K_FOREVER` / any duration.
  Depth is `CONFIG_AUDIO_PIPELINE_EVENT_QUEUE_DEPTH` (default 4). A **full queue drops the
  newest** event, so the oldest — and with it the *first* error, the one that explains the
  others — always survives.
* **Callback** — `config->event_cb`, invoked on the thread that produced the event (worker
  for EOF and processing errors, control thread for open/close failures). It runs *before*
  the event reaches the queue and **must not block**. Register one only when an event has to
  be observed synchronously on the publishing thread.

An event is queued **last**, once the pipeline has finished reacting to it: on the error
path the chain is already closed and the callback has already returned by the time
`get_event()` hands it over.

`get_event()` returns:

| Return | Means |
| --- | --- |
| `0` | an event was written |
| `-EINVAL` | NULL argument or uninitialised pipeline |
| `-ENOMSG` | queue empty and `timeout` was `K_NO_WAIT` |
| `-EAGAIN` | the waiting period expired |
| `-EPERM` | the built-in event slots this instance points at were claimed by another instance after it was joined — drain before joining, or use `AUDIO_PIPELINE_DEFINE()` |

## The sequences you will write

### Play a track, then play it again

```c
audio_pipeline_init(&p, &cfg, &sink);
audio_pipeline_set_format(&p, &fmt);
audio_pipeline_start(&p);          /* opens the chain, creates the thread */
audio_pipeline_play(&p);

audio_pipeline_get_event(&p, &ev, K_FOREVER);   /* EOF */

/* EOF left the chain OPEN and the thread alive: a second play() runs another
 * track with no reopen. Whether it produces anything is up to the source - the
 * file reader is at end of data until it is closed and reopened.
 */
audio_pipeline_play(&p);
```

### Recover from a node error

```c
audio_pipeline_get_event(&p, &ev, K_FOREVER);
if (ev.type == AUDIO_PIPELINE_EVENT_ERROR) {
	/* The worker already closed the chain: state is CLOSED, thread alive.
	 * By the time you see the event, the pipeline is fully quiesced.
	 */
	int ret = audio_pipeline_start(&p);   /* reopens the chain on the same thread */

	if (ret == 0) {
		audio_pipeline_play(&p);
	}
}
```

### Change the format between runs

```c
audio_pipeline_join(&p);                     /* the only thing that closes the chain */
audio_pipeline_set_format(&p, &other_fmt);   /* -EBUSY before the join */
audio_pipeline_start(&p);
audio_pipeline_play(&p);
```

### Drive frames yourself, without the worker

```c
audio_pipeline_init(&p, &cfg, &sink);
audio_pipeline_set_format(&p, &fmt);
audio_pipeline_start(&p);            /* opens the chain; the worker idles */

for (;;) {
	int ret = audio_pipeline_process_frame(&p);

	if (ret == -EPIPE) {         /* end of stream */
		break;
	}
	if (ret < 0) {               /* node error; nothing was closed for you */
		break;
	}
}

audio_pipeline_join(&p);
```

Never call `play()` while doing this — two threads pulling the same chain through one frame
buffer is exactly what the single worker exists to prevent.

### Run two pipelines at once

There is one built-in stack, one built-in frame buffer and one built-in event queue, so at
most one zero-initialised instance may run at a time. Give at least the second one its own
storage:

```c
AUDIO_PIPELINE_DEFINE(second, 128, 2048, 5);   /* stack + frame buf + event slots */
```

Otherwise `init()` or `start()` refuses the second claimant with `-EBUSY` and logs
*"a built-in pipeline resource is already owned by another instance"*.
