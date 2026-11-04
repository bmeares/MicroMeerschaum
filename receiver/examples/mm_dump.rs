// SPDX-License-Identifier: Apache-2.0
//! Scans journal segment files and prints one JSON line per file.
//!
//! `continuity` compares each file's first_seq with the last seq of the
//! previous file (in argument order) of the same key: `first`, `ok`, `gap`
//! (with the `missing` seq range) or `overlap`. Exits 1 if any file is
//! unreadable or any gap or overlap is found.

use std::collections::HashMap;
use std::fmt::Write;
use std::process::ExitCode;

use micromeerschaum::scan_segment;

fn json(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if u32::from(c) < 0x20 => {
                write!(out, "\\u{:04x}", u32::from(c)).expect("write to String")
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn main() -> ExitCode {
    let paths: Vec<_> = std::env::args_os().skip(1).collect();
    if paths.is_empty() {
        eprintln!("usage: mm_dump SEGMENT...");
        return ExitCode::from(2);
    }
    let mut clean = true;
    let mut last_seq: HashMap<String, u64> = HashMap::new();
    for path in paths {
        let name = json(&path.to_string_lossy());
        let data = match std::fs::read(&path) {
            Ok(data) => data,
            Err(error) => {
                println!("{{\"path\":{name},\"error\":{}}}", json(&error.to_string()));
                clean = false;
                continue;
            }
        };
        let scan = match scan_segment(&data) {
            Ok(scan) => scan,
            Err(error) => {
                println!(
                    "{{\"path\":{name},\"bytes\":{},\"error\":\"{error}\"}}",
                    data.len()
                );
                clean = false;
                continue;
            }
        };
        let first = scan.header.first_seq;
        let continuity = match last_seq.insert(scan.header.key.to_owned(), scan.last_seq) {
            None => "\"first\"".to_owned(),
            Some(prev) if first == prev.wrapping_add(1) => "\"ok\"".to_owned(),
            Some(prev) if first > prev => {
                format!("\"gap\",\"missing\":[{},{}]", prev + 1, first - 1)
            }
            Some(_) => "\"overlap\"".to_owned(),
        };
        clean &= continuity == "\"first\"" || continuity == "\"ok\"";
        println!(
            "{{\"path\":{name},\"key\":{},\"boot\":{},\"first_seq\":{first},\"last_seq\":{},\"records\":{},\
             \"bytes\":{},\"frontier\":{},\"stop\":\"{}\",\"continuity\":{continuity}}}",
            json(scan.header.key),
            scan.header.boot,
            scan.last_seq,
            scan.records,
            data.len(),
            scan.frontier,
            scan.stop.map_or("end", |stop| stop.name()),
        );
    }
    if clean {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
