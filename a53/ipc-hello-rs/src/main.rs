// Copyright (c) 2026 Chrispine Tinega <dev@chrispinetinega.com>
//
// SPDX-License-Identifier: Apache-2.0

//! BL350 IPC demo: the Linux half, in Rust on the Cortex-A53.
//!
//! The whole point of this file is how ordinary it is. The Cortex-M4F running
//! Zephyr, on the other side of the chip, is reachable as a character device.
//! You open it, you write bytes to it, you read bytes back. No driver, no
//! ioctl, no crate: `std` and a file.
//!
//! That file exists because a udev rule bound `rpmsg_chrdev` to the channel the
//! firmware announces (see `udev/99-bl350-demo.rules` at the repository root).
//! The kernel does not do it for you: it only auto-binds channels literally
//! named `rpmsg_chrdev`.
//!
//! LINUX SPEAKS FIRST, always. rpmsg teaches the firmware our endpoint address
//! from the first inbound message; before that the M4F has no destination to
//! send to. So this program greets, and the coprocessor answers.
//!
//! Reads here are blocking. If the coprocessor is not running, or is running a
//! firmware that does not answer, this waits rather than failing, check
//! `trace0` (see the README) and Ctrl-C.

use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::time::{Duration, Instant};

/// Created by the udev rule. Never hard-code /dev/rpmsg0: numbering is not
/// deterministic, and on this board the R5F registers rpmsg devices too.
const DEFAULT_DEVICE: &str = "/dev/rpmsg_bl350_demo";

const ROUND_TRIPS: usize = 5;

fn main() -> std::io::Result<()> {
    let path = std::env::args().nth(1).unwrap_or_else(|| DEFAULT_DEVICE.to_string());

    println!("BL350 IPC demo: Rust on the Cortex-A53 (Linux)");
    println!("talking to Zephyr on the Cortex-M4F, same chip");
    println!("endpoint: {path}");
    println!();

    let mut endpoint = OpenOptions::new().read(true).write(true).open(&path)?;

    let mut buf = [0u8; 512];

    for i in 1..=ROUND_TRIPS {
        let greeting = format!("Hello from Rust on the Cortex-A53 (message {i})");

        let sent_at = Instant::now();
        endpoint.write_all(greeting.as_bytes())?;
        println!("A53 -> M4F: \"{greeting}\"");

        let n = endpoint.read(&mut buf)?;
        let elapsed = sent_at.elapsed();

        println!("M4F -> A53: \"{}\"", String::from_utf8_lossy(&buf[..n]));
        println!("            round trip {:.3} ms", elapsed.as_secs_f64() * 1000.0);
        println!();

        std::thread::sleep(Duration::from_millis(700));
    }

    println!("{ROUND_TRIPS} round trips completed.");
    Ok(())
}
