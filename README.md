# Zephyr ↔ Rust on one chip: an IPC demo for the BLIIoT ARMxy BL350

Two processors, one piece of silicon, talking to each other.

The TI AM62x on the BL350 has four Cortex-A53 cores running Linux and one
Cortex-M4F running [Zephyr](https://zephyrproject.org). This demo makes them
say hello. Rust on the Linux side, C on the Zephyr side, plain text over
RPMsg.

The board target used here, `am62x_m4_bl350`, is in **mainline Zephyr**
([zephyrproject-rtos/zephyr#113302](https://github.com/zephyrproject-rtos/zephyr/pull/113302)).
There is no fork here and no `BOARD_ROOT`.

```
     Cortex-A53 ×4                            Cortex-M4F
     Linux 6.1, Rust                          Zephyr RTOS
          │                                        │
          │   /dev/rpmsg_bl350_demo                │
          └──────────── RPMsg / OpenAMP ───────────┘
                   vrings in shared DDR
                 MCU mailbox for the kick
```

## Why two processors at all?

Linux/Windows is very good at the things Linux is good at: networking, storage,
databases, package management, your whole toolchain. What it does not offer is a
promise about *when*. A general-purpose kernel schedules your process when it
gets around to it, and "usually within a millisecond" is not the same as
"always".

Plenty of real work needs the second kind of answer: hold this pulse for exactly
2 ms, sample this sensor every 500 µs, drop this output within one cycle if
something goes wrong. So chips like the AM62x pair the Linux cores with a small
microcontroller core that runs nothing else. The microcontroller keeps time; the
Linux side does everything that benefits from being a normal computer.

Then they have to talk, and that is what this repo is about.

## The one idea

On Linux, the coprocessor is a **file**.

```rust
let mut endpoint = OpenOptions::new().read(true).write(true).open(path)?;
endpoint.write_all(b"Hello from Rust on the Cortex-A53")?;
let n = endpoint.read(&mut buf)?;
```

That is the whole Rust program. No driver, no `ioctl`, no crate `std` and a
character device. `Cargo.toml` has an empty `[dependencies]` on purpose.

## What it looks like

```
$ ipc-hello-rs
BL350 IPC demo: Rust on the Cortex-A53 (Linux)
talking to Zephyr on the Cortex-M4F, same chip
endpoint: /dev/rpmsg_bl350_demo

A53 -> M4F: "Hello from Rust on the Cortex-A53 (message 1)"
M4F -> A53: "Hello from Zephyr on the Cortex-M4F (reply 1 since boot, uptime 28317 ms)"
            round trip 0.203 ms

A53 -> M4F: "Hello from Rust on the Cortex-A53 (message 2)"
M4F -> A53: "Hello from Zephyr on the Cortex-M4F (reply 2 since boot, uptime 28990 ms)"
            round trip 0.191 ms
...
```

Round trips land in the low hundreds of microseconds. Treat that as typical
rather than guaranteed.

Run it a second time and the coprocessor keeps counting, `reply 6 since boot`
answering `message 1`. The firmware has been up since the board powered on and
does not restart when your Linux program does. Two computers, two lifetimes.

The same conversation from the coprocessor's own log:

```
I: endpoint "bl350-demo" announced, waiting for the A53 to speak first
I: A53 -> M4F: "Hello from Rust on the Cortex-A53 (message 1)"
I: M4F -> A53: "Hello from Zephyr on the Cortex-M4F (reply 1 since boot, uptime 28317 ms)"
```

## Layout

One directory per core.

| Path | What |
|---|---|
| `m4f/hello/` | Smallest possible Zephyr app. Proves the board and toolchain. |
| `m4f/ipc-hello/` | The M4F half: OpenAMP, rpmsg, the `bl350-demo` endpoint. |
| `a53/ipc-hello-rs/` | The A53 half, in Rust. No dependencies. |
| `udev/` | Binds the rpmsg channel and gives it a stable `/dev` name. |
| `scripts/` | `stage.sh` puts the board in demo state; `restore.sh` undoes it. |

## What you need

- A **BLIIoT ARMxy BL350** running its stock Debian 12 image, reachable over SSH
  as root. Nothing here is board-agnostic: the addresses, the carveouts and the
  `m4fss` name are AM62x specifics.
- A **Zephyr development environment**: see
  [Getting Started](https://docs.zephyrproject.org/latest/develop/getting_started/).
  You need the SDK and `west`.
- **Docker**, to cross-compile the Rust binary against the board's glibc.

## Build

### Zephyr

Against an existing Zephyr checkout. The quickest way in, and how these images
were built and verified:

```bash
export ZEPHYR_BASE=/path/to/zephyrproject/zephyr
west build -b am62x_m4_bl350/am6254/m4 --pristine always \
    --build-dir build/hello     m4f/hello
west build -b am62x_m4_bl350/am6254/m4 --pristine always \
    --build-dir build/ipc-hello m4f/ipc-hello
```

That checkout needs to contain the board, so `ca0da250b5b` or later. To get a
dedicated workspace instead, `west init -l .` here and `west update`, using the
revision `west.yml` pins.

### Rust

The board is aarch64 Debian 12, so cross-compile against a matching glibc:

```bash
docker run --rm -v "$PWD/a53/ipc-hello-rs:/w" -w /w rust:bookworm bash -c \
  'apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu libc6-dev-arm64-cross && \
   rustup target add aarch64-unknown-linux-gnu && \
   CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc \
   cargo build --release --target aarch64-unknown-linux-gnu && \
   mkdir -p out && cp target/aarch64-unknown-linux-gnu/release/ipc-hello-rs out/'
```

There are no C dependencies, so this needs no `pkg-config` and no `-dev`
packages beyond the cross toolchain itself.

## Run

`<board>` is a hostname or an `~/.ssh/config` alias for the BL350.

```bash
./scripts/stage.sh <board>     # installs everything and reboots
ssh <board> ipc-hello-rs       # the demo
./scripts/restore.sh <board>   # put the board back as it was
```

If something on your board already talks to the coprocessor, name its systemd
unit and the scripts will stop it for the duration and re-enable it afterwards:

```bash
COPROC_SERVICE=my-app ./scripts/stage.sh <board>
```

Watch the coprocessor's side at the same time:

```bash
M4F=$(for d in /sys/class/remoteproc/remoteproc*; do \
          grep -q m4fss "$d/name" && basename "$d"; done)
cat /sys/kernel/debug/remoteproc/$M4F/trace0
```

## Four things the documentation does not tell you

**1. Linux has to speak first.** rpmsg teaches the firmware the Linux endpoint's
address from the first inbound message. Until one arrives, the M4F endpoint has
no destination and `rpmsg_send()` has nowhere to go and it does not fail
loudly, it just goes nowhere. So the coprocessor only ever replies. An
unsolicited greeting at boot looks exactly like a broken link.

**2. The kernel will not bind your channel.** The firmware announces a
name-service channel called `bl350-demo`. It appears on the rpmsg bus with
**no driver bound**, because the in-kernel `rpmsg_chrdev` driver only auto-binds
channels literally named `rpmsg_chrdev`. Without the udev rule in `udev/` there
is no `/dev` node at all. That rule also provides the stable symlink, because
device numbering is not deterministic and the R5F registers rpmsg devices too, so
`/dev/rpmsg0` is very often not yours.

**3. The remoteproc index is not stable across boots.** The M4F has been
`remoteproc0` on one boot and `remoteproc1` on the next, with the R5F taking the
other slot. Both cores publish a `trace0`, so a hardcoded index does not fail
loudly. It prints the other core's log and looks like your firmware said
nothing. Always resolve by `name`.

**4. OpenAMP needs a heap, and Zephyr's default is zero.**
`CONFIG_HEAP_MEM_POOL_SIZE` defaults to `0`. With no heap,
`rproc_virtio_create_vdev()` returns `NULL`, the link never comes up, and the
only clue is one line in a RAM buffer you have to know how to read. This demo
sets 16 KB.

## There is no serial console

This board's M4F console would be MCU_UART0, which no cable on the BL350
reaches. So the firmware logs to a RAM ring instead: with `CONFIG_RAM_CONSOLE=y`
Zephyr's resource table grows an `RSC_TRACE` entry pointing at
`ram_console_buf`, and Linux `remoteproc` maps it and publishes it as
`/sys/kernel/debug/remoteproc/<M4F>/trace0` when it loads the firmware.

That means you can read a microcontroller's `printf` from a Linux shell, before
any vring exists and whether or not the Linux side ever starts. It is a plain
ring buffer: newest wins, no timestamps, gone at reset.

## Licence

Apache-2.0. Full text in [LICENSE](LICENSE).
