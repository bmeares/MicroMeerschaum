// SPDX-License-Identifier: Apache-2.0
//! Meerschaum's instance methods backed by one authorized device session.
use meerschaum_plugin::{
    chrono::{DateTime, NaiveDateTime, SecondsFormat, Utc},
    prelude::*,
};
use serde::Deserialize;
use std::{
    collections::{BTreeSet, HashSet},
    env, fs,
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::Mutex,
};

pub(super) const TRANSPORT: &str = concat!(
    include_str!("write_guard.py"),
    "\n",
    include_str!("transport.py")
);
const MAX_ROWS: usize = 128;
const MAX_CATALOG: usize = 4096;
pub(crate) const METHODS: &[&str] = &[
    "test_connection",
    "fetch_pipes_keys",
    "get_pipe_id",
    "get_pipe_attributes",
    "pipe_exists",
    "get_pipe_data",
    "sync_pipe",
    "get_pipe_rowcount",
    "get_sync_time",
    "get_pipe_columns_types",
];

pub(super) fn error(reason: impl ToString) -> Error {
    Error::Message(reason.to_string())
}

pub(super) fn python(config: &Value) -> Command {
    let executable = config["python"]
        .as_str()
        .map(str::to_owned)
        .or_else(|| std::env::var("MRSM_PYTHON").ok())
        .unwrap_or_else(|| "python3".into());
    Command::new(executable)
}

pub(super) fn exchange(config: &Value, request: Value) -> Result<Value> {
    // The host can inspect metadata from background threads during a sync.
    static LINK: Mutex<()> = Mutex::new(());
    let _link = LINK
        .lock()
        .map_err(|_| error("Device session interrupted."))?;
    let mut child = python(config)
        .args(["-P", "-c", TRANSPORT])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(error)?;
    let mut input = child.stdin.take().expect("piped stdin");
    let output = child.stdout.take().expect("piped stdout");
    let write = request["op"] == "write";
    let result = (|| -> Result<Value> {
        serde_json::to_writer(&mut input, &json!({"config":config, "request":request}))
            .map_err(error)?;
        writeln!(input).map_err(error)?;
        let mut reader = BufReader::new(output);
        let mut read_reply = || -> Option<Value> {
            let mut line = String::new();
            reader.read_line(&mut line).ok()?;
            serde_json::from_str(&line).ok()
        };
        let mut response = read_reply().ok_or_else(|| error(
            "Device transport closed without a result. Any sent action or write has an unknown outcome."
        ))?;
        if write && let Some(pending) = response["pending"].as_str() {
            response = read_reply().ok_or_else(|| error(pending))?;
        }
        if response["ok"] != true {
            return Err(error(
                response["error"]
                    .as_str()
                    .unwrap_or("Invalid device transport result."),
            ));
        }
        if write {
            if !response["data"].is_string() {
                return Err(error(
                    "Outcome unknown after sending write: invalid acknowledgement.",
                ));
            }
            // A lost acknowledgement leaves the write guarded. Its device result is already known.
            let _ = writeln!(input, "ack");
        }
        Ok(response["data"].clone())
    })();
    drop(input);
    let _ = child.wait();
    result
}

type Key = (String, String, Option<String>);

#[derive(Deserialize)]
struct Entry {
    id: i64,
    key: Key,
    instance: String,
    read: bool,
    write: bool,
    #[serde(default)]
    filters: Vec<String>,
    #[serde(default)]
    columns: Option<serde_json::Map<String, Value>>,
    #[serde(default)]
    dtypes: Option<serde_json::Map<String, Value>>,
}

impl Entry {
    fn attributes(&self) -> Value {
        let locks = if self.write {
            json!(["clear", "drop", "delete", "parameters"])
        } else {
            json!(["all"])
        };
        let mut parameters = serde_json::Map::new();
        if let Some(columns) = self.columns.as_ref().filter(|m| !m.is_empty()) {
            parameters.insert("columns".into(), Value::Object(columns.clone()));
        }
        if let Some(dtypes) = self.dtypes.as_ref().filter(|m| !m.is_empty()) {
            parameters.insert("dtypes".into(), Value::Object(dtypes.clone()));
        }
        parameters.insert("locks".into(), locks);
        parameters.insert(
            "micro".into(),
            json!({
                "instance": self.instance,
                "read": self.read,
                "write": self.write,
                "filters": self.filters,
            }),
        );
        json!({
            "pipe_id": self.id,
            "connector_keys": self.key.0,
            "metric_key": self.key.1,
            "location_key": self.key.2,
            "parameters": Value::Object(parameters),
        })
    }
}

#[derive(Deserialize)]
struct Page {
    items: Vec<Entry>,
    more: bool,
}

fn catalog(config: &Value) -> Result<Vec<Entry>> {
    let mut entries = Vec::new();
    let mut keys = HashSet::new();
    let mut ids = HashSet::new();
    loop {
        let page: Page = serde_json::from_value(exchange(
            config,
            json!({"op":"catalog",
            "offset":entries.len(), "limit":16}),
        )?)
        .map_err(error)?;
        if page.items.len() > 16
            || (page.more && page.items.is_empty())
            || entries.len() + page.items.len() > MAX_CATALOG
        {
            return Err(error("Invalid device catalog page."));
        }
        for entry in page.items {
            if entry.id <= 0
                || entry.key.0.is_empty()
                || entry.key.1.is_empty()
                || !ids.insert(entry.id)
                || !keys.insert(entry.key.clone())
            {
                return Err(error("Invalid or duplicate identity in device catalog."));
            }
            entries.push(entry);
        }
        if !page.more {
            // Meerschaum owns this namespace for sync metrics, users and tokens.
            entries.retain(|entry| entry.key.0 != "mrsm");
            return Ok(entries);
        }
    }
}

fn write_incident_or(config: &Value, label: &str, key: &Key, failure: Error) -> Error {
    let query = json!({"op":"pending_write","key":key,"label":label});
    match exchange(config, query) {
        Ok(Value::String(incident)) => error(incident),
        _ => failure,
    }
}

fn catalog_entry(config: &Value, label: &str, key: &Key) -> Result<Option<Entry>> {
    let entries =
        catalog(config).map_err(|failure| write_incident_or(config, label, key, failure))?;
    Ok(entries.into_iter().find(|entry| &entry.key == key))
}

fn key_of(context: &Value) -> Result<Key> {
    let p = &context["pipe"]["keys"];
    serde_json::from_value(json!([
        p["connector_keys"],
        p["metric_key"],
        p["location_key"]
    ]))
    .map_err(error)
}

fn selected(value: &str, filter: &Value) -> bool {
    let mut has_positive = false;
    let mut found = false;
    for item in filter.as_array().into_iter().flatten() {
        let text = item.as_str().unwrap_or("[None]");
        if let Some(excluded) = text.strip_prefix('_') {
            if excluded == value {
                return false;
            }
        } else {
            has_positive = true;
            found |= text == value || (value == "[None]" && text == "None");
        }
    }
    !has_positive || found
}

fn list(config: &Value, query: &Value) -> Result<Value> {
    if query["params"].as_object().is_some_and(|p| !p.is_empty()) {
        return Err(error("Device catalog parameter filters are unsupported."));
    }
    let mut found = serde_json::Map::new();
    for e in catalog(config)? {
        if selected(&e.key.0, &query["connector_keys"])
            && selected(&e.key.1, &query["metric_keys"])
            && selected(
                e.key.2.as_deref().unwrap_or("[None]"),
                &query["location_keys"],
            )
            && selected("", &query["tags"])
        {
            found.insert(e.id.to_string(), json!(e.key));
        }
    }
    Ok(found.into())
}

fn wire_datetime(s: &str) -> Result<Value> {
    let us = DateTime::parse_from_rfc3339(s)
        .map(|dt| dt.timestamp_micros())
        .or_else(|_| {
            NaiveDateTime::parse_from_str(s, "%Y-%m-%dT%H:%M:%S%.f")
                .or_else(|_| NaiveDateTime::parse_from_str(s, "%Y-%m-%d %H:%M:%S%.f"))
                .map(|dt| dt.and_utc().timestamp_micros())
        })
        .map_err(error)?;
    Ok(json!({"$us":us}))
}

fn from_wire_datetime(obj: &serde_json::Map<String, Value>) -> Result<String> {
    let us = obj
        .get("$us")
        .and_then(Value::as_i64)
        .filter(|_| obj.len() == 1)
        .and_then(DateTime::<Utc>::from_timestamp_micros)
        .ok_or_else(|| error("Invalid UTC microseconds in device reply."))?;
    Ok(us.to_rfc3339_opts(SecondsFormat::Micros, true))
}

fn is_op_unavailable(err: &Error) -> bool {
    matches!(err, Error::Message(msg) if msg.eq_ignore_ascii_case("operation unavailable"))
}

fn attach_bounds(request: &mut Value, query: &Value) -> Result<()> {
    for name in ["begin", "end", "params"] {
        let v = &query[name];
        if !v.is_null() && v.as_object().is_none_or(|m| !m.is_empty()) {
            request[name] = if name != "params" && v.is_string() {
                wire_datetime(v.as_str().unwrap())?
            } else {
                v.clone()
            };
        }
    }
    Ok(())
}

fn parse_sync_time_reply(value: Value) -> Result<Value> {
    match value {
        Value::Null => Ok(Value::Null),
        Value::Number(n) if n.is_i64() => Ok(Value::Number(n)),
        Value::Object(obj) => Ok(Value::String(from_wire_datetime(&obj)?)),
        _ => Err(error("Invalid device sync_time result.")),
    }
}

fn sync_time(config: &Value, key: &Key, context: &Value) -> Result<Value> {
    if context["newest"].as_bool() == Some(false) {
        return Ok(Value::Null);
    }
    let mut request = json!({"op": "sync_time", "key": key});
    let params = if !context["params"].is_null() {
        &context["params"]
    } else {
        &context["query"]["params"]
    };
    if !params.is_null() && params.as_object().is_none_or(|m| !m.is_empty()) {
        request["params"] = params.clone();
    }
    match exchange(config, request) {
        Ok(value) => parse_sync_time_reply(value),
        Err(err) if is_op_unavailable(&err) => Ok(Value::Null),
        Err(err) => Err(err),
    }
}

fn rowcount(config: &Value, key: &Key, query: &Value) -> Result<Value> {
    if query["remote"] == true {
        return Err(error("Device rowcount supports local rows only."));
    }
    let mut request = json!({"op": "rowcount", "key": key});
    attach_bounds(&mut request, query)?;
    match exchange(config, request) {
        Ok(value) => {
            let count = value
                .as_u64()
                .ok_or_else(|| error("Invalid device rowcount result."))?;
            Ok(json!(count))
        }
        Err(err) if is_op_unavailable(&err) => Ok(json!(read(config, key, query)?.len())),
        Err(err) => Err(err),
    }
}

fn read(config: &Value, key: &Key, query: &Value) -> Result<Vec<Value>> {
    let explicit = query["limit"].as_u64();
    if (!query["limit"].is_null() && explicit.is_none())
        || explicit.is_some_and(|n| n > MAX_ROWS as u64)
    {
        return Err(error("Device reads accept a limit from 0 to 128 rows."));
    }
    if query["remote"] == true {
        return Err(error("Device reads support local rows only."));
    }
    let desc = match query["order"].as_str() {
        None if query["order"].is_null() => false,
        Some(s) if s.eq_ignore_ascii_case("asc") => false,
        Some(s) if s.eq_ignore_ascii_case("desc") => true,
        _ => {
            return Err(error(
                "Device reads support ascending ('asc') or descending ('desc') order.",
            ));
        }
    };
    let limit = explicit.map(|n| n as usize).unwrap_or(MAX_ROWS);
    let mut request = json!({"op": "read", "key": key, "limit": limit});
    if desc {
        request["order"] = json!("desc");
    }
    attach_bounds(&mut request, query)?;
    let rows = exchange(config, request)?;
    let rows = rows
        .as_array()
        .ok_or_else(|| error("Invalid device row result."))?;
    if rows.len() > limit || (explicit.is_none() && rows.len() == limit) {
        return Err(error(
            "The full result is not known to fit. Supply a limit or narrower device-supported bounds.",
        ));
    }
    Ok(rows.clone())
}

fn frame(rows: Vec<Value>, query: &Value) -> Result<Frame> {
    let mut rows = rows;
    let mut time_columns = BTreeSet::new();
    for row in &mut rows {
        let doc = row
            .as_object_mut()
            .ok_or_else(|| error("Invalid document reply."))?;
        doc.retain(|col, _| {
            query["select_columns"]
                .as_array()
                .is_none_or(|cols| cols.is_empty() || cols.iter().any(|v| v == col))
                && query["omit_columns"]
                    .as_array()
                    .is_none_or(|cols| !cols.iter().any(|v| v == col))
        });
        for (name, v) in doc.iter_mut() {
            if let Some(obj) = v.as_object() {
                *v = json!(from_wire_datetime(obj)?);
                time_columns.insert(name.clone());
            } else if v.is_array() {
                return Err(error("Invalid nested device document."));
            }
        }
    }
    let dtypes: Vec<_> = time_columns
        .iter()
        .map(|s| (s.as_str(), "datetime"))
        .collect();
    Frame::from_rows(rows, &dtypes)
}

fn write(config: &Value, label: &str, key: &Key, data: Option<Frame>) -> Result<Value> {
    if !catalog_entry(config, label, key)?.is_some_and(|entry| entry.write) {
        return Err(write_incident_or(
            config,
            label,
            key,
            error("Pipe is not exposed for writing."),
        ));
    }
    let frame = data.ok_or_else(|| error("Pipe.sync requires rows."))?;
    if frame.height() > MAX_ROWS {
        return Err(error("A device write accepts at most 128 rows."));
    }
    let dtypes = frame.dtypes();
    let mut rows = frame.try_to_rows()?;
    for row in &mut rows {
        for (name, value) in row.as_object_mut().expect("frame row").iter_mut() {
            if dtypes.get(name).is_some_and(|t| t.starts_with("datetime")) && !value.is_null() {
                *value = wire_datetime(value.as_str().ok_or_else(|| error("Invalid timestamp."))?)?;
            } else if value.is_array() || value.is_object() {
                return Err(error("Device documents accept flat scalar fields."));
            }
        }
    }
    let mut settings = config.clone();
    settings["_micro_label"] = json!(label);
    let message = exchange(&settings, json!({"op":"write","key":key,"rows":rows}))?;
    Ok(json!([
        true,
        message
            .as_str()
            .ok_or_else(|| error("Invalid write acknowledgement."))?
    ]))
}

fn is_serial_port_label(label: &str) -> bool {
    if label.starts_with('/') {
        return true;
    }
    let upper = label.to_ascii_uppercase();
    if let Some(rest) = upper.strip_prefix("COM") {
        return !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit());
    }
    false
}

fn is_auto_label(label: &str) -> bool {
    matches!(label, "auto" | "usb")
}

fn has_transport_endpoint(config: &Value) -> bool {
    let Some(obj) = config.as_object() else {
        return false;
    };
    obj.get("port")
        .and_then(Value::as_str)
        .is_some_and(|s| !s.is_empty())
        || obj
            .get("address")
            .and_then(Value::as_str)
            .is_some_and(|s| !s.is_empty())
}

fn port_priority(path: &str) -> u8 {
    let lower = path.to_ascii_lowercase();
    if lower.contains("espressif")
        || lower.contains("esp32")
        || lower.contains("micropython")
        || lower.contains("circuitpython")
        || lower.contains("rp2040")
        || lower.contains("pico")
        || lower.contains("cp210")
        || lower.contains("silicon_labs")
        || lower.contains("ch340")
        || lower.contains("ch341")
        || lower.contains("ch910")
        || lower.contains("ftdi")
        || lower.contains("usb_jtag")
    {
        return 0;
    }
    if lower.contains("ttyacm") || lower.contains("usbmodem") {
        return 1;
    }
    if lower.contains("ttyusb") || lower.contains("usbserial") || lower.contains("slab_usbtouart") {
        return 2;
    }
    3
}

fn discover_usb_ports_in(by_id_dir: &Path, dev_dir: &Path) -> Vec<String> {
    let mut ports = Vec::new();
    let mut seen_targets = HashSet::new();

    if let Ok(entries) = fs::read_dir(by_id_dir) {
        let mut links: Vec<PathBuf> = entries.filter_map(|e| e.ok().map(|e| e.path())).collect();
        links.sort();
        for link in links {
            let target = fs::canonicalize(&link).unwrap_or_else(|_| link.clone());
            if seen_targets.insert(target) {
                ports.push(link.to_string_lossy().into_owned());
            }
        }
    }

    if let Ok(entries) = fs::read_dir(dev_dir) {
        let mut devs: Vec<PathBuf> = entries
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| {
                p.file_name().and_then(|n| n.to_str()).is_some_and(|name| {
                    name.starts_with("ttyACM")
                        || name.starts_with("ttyUSB")
                        || name.starts_with("cu.usbmodem")
                        || name.starts_with("cu.usbserial")
                        || name.starts_with("cu.SLAB_USBtoUART")
                        || name.starts_with("cu.wchusbserial")
                })
            })
            .collect();
        devs.sort();
        for dev in devs {
            let target = fs::canonicalize(&dev).unwrap_or_else(|_| dev.clone());
            if seen_targets.insert(target) {
                ports.push(dev.to_string_lossy().into_owned());
            }
        }
    }

    ports.sort_by(|a, b| {
        port_priority(a)
            .cmp(&port_priority(b))
            .then_with(|| a.cmp(b))
    });
    ports
}

fn discover_usb_ports(defaults: &Value) -> Vec<String> {
    if let Some(raw) = env::var_os("MRSM_MICRO_USB_PORTS") {
        if raw.is_empty() {
            return Vec::new();
        }
        let mut ports: Vec<String> = env::split_paths(&raw)
            .filter(|p| !p.as_os_str().is_empty())
            .map(|p| p.to_string_lossy().into_owned())
            .collect();
        ports.sort_by(|a, b| {
            port_priority(a)
                .cmp(&port_priority(b))
                .then_with(|| a.cmp(b))
        });
        return ports;
    }

    let ports = discover_usb_ports_in(Path::new("/dev/serial/by-id"), Path::new("/dev"));
    if !ports.is_empty() {
        return ports;
    }

    if let Ok(Value::Array(items)) = exchange(defaults, json!({"op": "discover"})) {
        let mut found: Vec<String> = items
            .into_iter()
            .filter_map(|v| v.as_str().map(str::to_owned))
            .filter(|s| !s.is_empty())
            .collect();
        found.sort_by(|a, b| {
            port_priority(a)
                .cmp(&port_priority(b))
                .then_with(|| a.cmp(b))
        });
        return found;
    }
    Vec::new()
}

fn micro_defaults() -> Value {
    host::config(&["meerschaum", "connectors", "micro", "default"])
        .ok()
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}))
}

fn merge_defaults(defaults: &Value, overrides: &Value) -> Value {
    let mut merged = defaults.as_object().cloned().unwrap_or_default();
    if let Some(map) = overrides.as_object() {
        for (k, v) in map {
            merged.insert(k.clone(), v.clone());
        }
    }
    Value::Object(merged)
}

fn serial_config(defaults: &Value, port: &str) -> Value {
    merge_defaults(defaults, &json!({"transport": "serial", "port": port}))
}

fn short_port_label(port: &str) -> String {
    let target = fs::canonicalize(port).unwrap_or_else(|_| PathBuf::from(port));
    target
        .file_name()
        .and_then(|n| n.to_str())
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
        .unwrap_or_else(|| port.to_owned())
}

fn canonical_port(port: &str) -> PathBuf {
    fs::canonicalize(port).unwrap_or_else(|_| PathBuf::from(port))
}

fn configured_micro_connectors(defaults: &Value) -> Vec<(String, Value)> {
    let Ok(Value::Object(map)) = host::config(&["meerschaum", "connectors", "micro"]) else {
        return Vec::new();
    };
    let mut items: Vec<(String, Value)> = map
        .into_iter()
        .filter(|(label, value)| label != "default" && value.is_object())
        .map(|(label, value)| (label, merge_defaults(defaults, &value)))
        .filter(|(_, cfg)| has_transport_endpoint(cfg))
        .collect();
    items.sort_by(|a, b| a.0.cmp(&b.0));
    items
}

fn match_configured_port(port: &str, defaults: &Value) -> Option<(String, Value)> {
    let target = canonical_port(port);
    for (label, cfg) in configured_micro_connectors(defaults) {
        let transport = cfg["transport"].as_str().unwrap_or("serial");
        if matches!(transport, "serial" | "usb")
            && let Some(cfg_port) = cfg["port"].as_str()
            && canonical_port(cfg_port) == target
        {
            return Some((format!("micro:{label}"), cfg));
        }
    }
    None
}

fn probe_micro_port(defaults: &Value, port: &str) -> bool {
    let mut cfg = serial_config(defaults, port);
    if let Some(obj) = cfg.as_object_mut() {
        let existing = obj.get("timeout").and_then(Value::as_f64).unwrap_or(1.0);
        obj.insert("timeout".into(), json!(existing.min(1.0)));
    }
    exchange(&cfg, json!({"op": "actions"})).is_ok()
        || exchange(&cfg, json!({"op": "catalog", "offset": 0, "limit": 0})).is_ok()
}

fn auto_detect_usb(defaults: &Value, fallback_sole_configured: bool) -> Result<(String, Value)> {
    let ports = discover_usb_ports(defaults);
    if ports.is_empty() {
        if fallback_sole_configured {
            let configured = configured_micro_connectors(defaults);
            if let [(label, cfg)] = configured.as_slice() {
                return Ok((format!("micro:{label}"), cfg.clone()));
            }
        }
        return Err(error(
            "No USB MicroMeerschaum device detected. Connect a device over USB, pass -i micro:<port>, or configure meerschaum.connectors.micro.<label>.",
        ));
    }

    let selected = if ports.len() == 1 {
        ports[0].clone()
    } else {
        let responding: Vec<String> = ports
            .iter()
            .filter(|port| probe_micro_port(defaults, port))
            .cloned()
            .collect();
        match responding.as_slice() {
            [one] => one.clone(),
            [] => {
                return Err(error(format!(
                    "Found USB serial ports ({}), but none answered the MicroMeerschaum session protocol. Select a port explicitly with -i micro:<port>.",
                    ports.join(", ")
                )));
            }
            many => {
                return Err(error(format!(
                    "Multiple MicroMeerschaum USB devices detected ({}). Select one with -i micro:<port> or configure meerschaum.connectors.micro.<label>.",
                    many.join(", ")
                )));
            }
        }
    };

    if let Some(matched) = match_configured_port(&selected, defaults) {
        return Ok(matched);
    }
    let label = short_port_label(&selected);
    Ok((format!("micro:{label}"), serial_config(defaults, &selected)))
}

fn resolve_call_config(req: &Request) -> Result<(String, Value)> {
    if has_transport_endpoint(&req.config) {
        return Ok((req.label.clone(), req.config.clone()));
    }
    let defaults = micro_defaults();
    let merged = merge_defaults(&defaults, &req.config);
    if has_transport_endpoint(&merged) {
        return Ok((req.label.clone(), merged));
    }
    if is_serial_port_label(&req.label) {
        return Ok((
            short_port_label(&req.label),
            serial_config(&merged, &req.label),
        ));
    }
    if is_auto_label(&req.label) || req.label == "main" {
        let (instance, cfg) = auto_detect_usb(&merged, false)?;
        let label = instance
            .strip_prefix("micro:")
            .unwrap_or(&req.label)
            .to_owned();
        return Ok((label, cfg));
    }
    Err(error(format!(
        "Configure meerschaum.connectors.micro.{}, or use -i micro:auto / -i micro:<port>.",
        req.label
    )))
}

fn resolve_action_target(kw: &Value) -> Result<(String, Value)> {
    let defaults = micro_defaults();
    let explicit = kw["mrsm_instance"]
        .as_str()
        .filter(|s| !s.is_empty())
        .map(str::to_owned);
    let configured_default = host::config(&["meerschaum", "instance"])
        .ok()
        .and_then(|v| v.as_str().map(str::to_owned))
        .unwrap_or_default();

    let selected_micro = explicit
        .as_deref()
        .and_then(|s| s.strip_prefix("micro:"))
        .or_else(|| {
            if explicit.is_none() {
                configured_default.strip_prefix("micro:")
            } else {
                None
            }
        })
        .filter(|l| !l.is_empty());

    if let Some(label) = selected_micro {
        let configured = host::config(&["meerschaum", "connectors", "micro", label])?;
        if configured.is_object() {
            return Ok((
                format!("micro:{label}"),
                merge_defaults(&defaults, &configured),
            ));
        }
        if is_auto_label(label) {
            return auto_detect_usb(&defaults, true);
        }
        if is_serial_port_label(label) {
            return Ok((
                format!("micro:{}", short_port_label(label)),
                serial_config(&defaults, label),
            ));
        }
        return Err(error(format!(
            "Configure meerschaum.connectors.micro.{label} (or omit -i / use -i micro:auto to auto-detect a USB device)."
        )));
    }

    auto_detect_usb(&defaults, true)
}

pub(crate) fn call(req: &Request, data: Option<Frame>) -> Result<Reply> {
    let (label, resolved_config) = resolve_call_config(req)?;
    let config = &resolved_config;
    let context = &req.context;
    let query = &context["query"];
    let method = req.method.as_str();
    if method == "fetch_pipes_keys" {
        return Ok(Reply::json(list(config, query)?));
    }
    if method == "test_connection" {
        catalog(config)?;
        return Ok(Reply::json(json!(true)));
    }
    if !METHODS.contains(&method) {
        return Err(Error::Unsupported(format!(
            "Device instances do not support {method}. Expose Pipes on the device."
        )));
    }
    let key = key_of(context)?;
    if key.0 == "mrsm" {
        return Ok(match method {
            "pipe_exists" => Reply::json(json!(false)),
            "get_pipe_id" | "get_sync_time" => Reply::json(Value::Null),
            "get_pipe_attributes" | "get_pipe_columns_types" => Reply::json(json!({})),
            "get_pipe_rowcount" => Reply::json(json!(0)),
            "get_pipe_data" => Reply::data(None),
            _ => Reply::json(json!([
                false,
                "Host metadata cannot be stored on a device."
            ])),
        });
    }
    match method {
        "get_pipe_data" => Ok(Reply::data(Some(frame(read(config, &key, query)?, query)?))),
        "get_pipe_rowcount" => Ok(Reply::json(rowcount(config, &key, query)?)),
        "get_sync_time" => Ok(Reply::json(sync_time(config, &key, context)?)),
        "get_pipe_columns_types" => {
            let dtypes = catalog_entry(config, &label, &key)?
                .and_then(|e| e.dtypes)
                .unwrap_or_default();
            Ok(Reply::json(Value::Object(dtypes)))
        }
        "sync_pipe" => Ok(Reply::json(match write(config, &label, &key, data) {
            Ok(reply) => reply,
            Err(reason) => json!([false, reason.to_string()]),
        })),
        _ => {
            let e = catalog_entry(config, &label, &key)?;
            Ok(Reply::json(match method {
                "pipe_exists" => json!(e.is_some()),
                "get_pipe_id" => json!(e.map(|e| e.id)),
                "get_pipe_attributes" => e.map(|e| e.attributes()).unwrap_or(json!({})),
                _ => return Err(Error::Unsupported(method.into())),
            }))
        }
    }
}

pub(crate) fn action(request: &Request, command: &str) -> Result<Reply> {
    let kw = &request.context["kwargs"];
    let (instance, config) = resolve_action_target(kw)?;
    let args = kw["action"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .collect::<Vec<_>>();
    if command == "micro_reconcile" {
        let [token] = args.as_slice() else {
            return Err(error(
                "Supply the recovery token from the unresolved write.",
            ));
        };
        let message = exchange(&config, json!({"op":"reconcile","token":token}))?;
        return Ok(Reply::json(json!([true, message])));
    }
    if command == "micro_shell" && args.is_empty() {
        crate::device_shell::run(&config, &instance)?;
        return Ok(Reply::json(json!([true, "Success"])));
    }
    if args.is_empty() {
        return Err(error(
            "Supply a device action, e.g. mrsm micro action show pipes.",
        ));
    }
    let words = args.join(" ");
    let result = exchange(&config, json!({"op":"action","line":words}))?;
    Ok(Reply::json(json!([
        true,
        result
            .as_str()
            .ok_or_else(|| error("Invalid action reply."))?
    ])))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entry_attributes_include_sql_schema_and_omit_empty() {
        let sql_entry: Entry = serde_json::from_value(json!({
            "id": 2,
            "key": ["sensor", "weather", "roof"],
            "instance": "sql:local",
            "read": true,
            "write": true,
            "filters": ["begin", "end", "params"],
            "columns": {"datetime": "dt", "station": "station"},
            "dtypes": {"dt": "datetime", "station": "string", "celsius": "float"},
        }))
        .expect("sql entry");
        let attrs = sql_entry.attributes();
        assert_eq!(
            attrs["parameters"]["columns"],
            json!({"datetime": "dt", "station": "station"})
        );
        assert_eq!(
            attrs["parameters"]["dtypes"],
            json!({"dt": "datetime", "station": "string", "celsius": "float"})
        );
        assert_eq!(
            attrs["parameters"]["locks"],
            json!(["clear", "drop", "delete", "parameters"])
        );

        let legacy_entry: Entry = serde_json::from_value(json!({
            "id": 1,
            "key": ["sensor", "audit", null],
            "instance": "journal:local",
            "read": true,
            "write": false,
        }))
        .expect("legacy entry");
        let legacy_attrs = legacy_entry.attributes();
        let params = legacy_attrs["parameters"].as_object().expect("parameters");
        assert!(!params.contains_key("columns"));
        assert!(!params.contains_key("dtypes"));
        assert_eq!(params["locks"], json!(["all"]));
    }

    #[test]
    fn sync_time_reply_handles_null_int_and_datetime() {
        assert_eq!(parse_sync_time_reply(Value::Null).unwrap(), Value::Null);
        assert_eq!(parse_sync_time_reply(json!(42)).unwrap(), json!(42));
        assert_eq!(parse_sync_time_reply(json!(-7)).unwrap(), json!(-7));
        assert_eq!(
            parse_sync_time_reply(json!({"$us": 1717430701123456_i64})).unwrap(),
            json!("2024-06-03T16:05:01.123456Z")
        );
        assert_eq!(
            parse_sync_time_reply(json!({"$us": -1_000_000_i64})).unwrap(),
            json!("1969-12-31T23:59:59.000000Z")
        );
        assert!(parse_sync_time_reply(json!("unexpected")).is_err());
        assert!(parse_sync_time_reply(json!({"$us": "bad"})).is_err());
        assert!(parse_sync_time_reply(json!({"$us": 1, "extra": 2})).is_err());
    }

    #[test]
    fn wire_datetime_roundtrips_and_op_unavailable_matches_exactly() {
        let encoded = wire_datetime("2024-06-03T16:05:01.123456Z").unwrap();
        assert_eq!(encoded, json!({"$us": 1717430701123456_i64}));
        let decoded = from_wire_datetime(encoded.as_object().unwrap()).unwrap();
        assert_eq!(decoded, "2024-06-03T16:05:01.123456Z");

        assert!(is_op_unavailable(&Error::Message(
            "operation unavailable".into()
        )));
        assert!(is_op_unavailable(&Error::Message(
            "Operation Unavailable".into()
        )));
        assert!(!is_op_unavailable(&Error::Message(
            "Pipe unavailable for sync_time".into()
        )));
    }

    #[test]
    fn serial_port_labels_and_priority_follow_usb_conventions() {
        assert!(is_serial_port_label("/dev/ttyACM0"));
        assert!(is_serial_port_label(
            "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_20:6E:F1:DC:CA:28-if00"
        ));
        assert!(is_serial_port_label("COM3"));
        assert!(is_serial_port_label("com12"));
        assert!(!is_serial_port_label("bench"));
        assert!(!is_serial_port_label("auto"));
        assert!(!is_serial_port_label("COM"));
        assert!(!is_serial_port_label("COMabc"));

        assert!(is_auto_label("auto"));
        assert!(is_auto_label("usb"));
        assert!(!is_auto_label("bench"));

        assert!(
            port_priority("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial-if00")
                < port_priority("/dev/ttyACM0")
        );
        assert!(port_priority("/dev/ttyACM0") < port_priority("/dev/ttyUSB0"));
        assert!(port_priority("/dev/ttyUSB0") < port_priority("/dev/pts/4"));
    }

    #[test]
    fn discover_usb_ports_deduplicates_by_id_and_orders_mcu_first() {
        let dir = tempfile::tempdir().expect("tempdir");
        let dev = dir.path().join("dev");
        let by_id = dev.join("serial/by-id");
        fs::create_dir_all(&by_id).expect("mkdir");
        let acm0 = dev.join("ttyACM0");
        let usb0 = dev.join("ttyUSB0");
        let ttys0 = dev.join("ttyS0");
        fs::write(&acm0, b"").expect("acm0");
        fs::write(&usb0, b"").expect("usb0");
        fs::write(&ttys0, b"").expect("ttys0");
        #[cfg(unix)]
        std::os::unix::fs::symlink(
            &acm0,
            by_id.join("usb-Espressif_USB_JTAG_serial_debug_unit-if00"),
        )
        .expect("symlink");

        let ports = discover_usb_ports_in(&by_id, &dev);
        assert_eq!(ports.len(), 2, "{ports:?}");
        assert!(ports[0].contains("Espressif"), "{ports:?}");
        assert!(ports[1].ends_with("ttyUSB0"), "{ports:?}");
        assert!(!ports.iter().any(|p| p.ends_with("ttyS0")), "{ports:?}");
    }

    #[test]
    fn merge_defaults_preserves_overrides_and_sets_serial_port() {
        let defaults = json!({"baudrate": 9600, "timeout": 3, "python": "/opt/py"});
        let cfg = serial_config(&defaults, "/dev/ttyACM0");
        assert_eq!(cfg["transport"], json!("serial"));
        assert_eq!(cfg["port"], json!("/dev/ttyACM0"));
        assert_eq!(cfg["baudrate"], json!(9600));
        assert_eq!(cfg["timeout"], json!(3));
        assert_eq!(cfg["python"], json!("/opt/py"));
    }
}
