// SPDX-FileCopyrightText: 2026 Antonio De Gaetano <antux912012@users.noreply.github.com>
// SPDX-License-Identifier: GPL-3.0-or-later

//! Build script that compiles the bundled Papirus icon GResource for RecGo.
//!
//! Upstream caller: Cargo build pipeline before compiling the `recgo` binary.
//! Downstream consumers: `src/application.rs` embeds the compiled `recgo.gresource` file.

use std::env;
use std::path::PathBuf;
use std::process::Command;

/// Compiles `data/recgo.gresource.xml` into `$OUT_DIR/recgo.gresource`.
///
/// # Panics
///
/// Panics if Cargo does not set `OUT_DIR` or if `glib-compile-resources` fails.
fn main() {
    println!("cargo:rerun-if-changed=data/recgo.gresource.xml");
    println!("cargo:rerun-if-changed=data/icons/papirus");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("Cargo must set OUT_DIR"));
    let target_file = out_dir.join("recgo.gresource");

    let status = Command::new("glib-compile-resources")
        .arg("--sourcedir=data")
        .arg(format!("--target={}", target_file.display()))
        .arg("data/recgo.gresource.xml")
        .status()
        .expect("Failed to execute glib-compile-resources");

    assert!(
        status.success(),
        "glib-compile-resources exited with status {status}"
    );
}
