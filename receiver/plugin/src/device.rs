// SPDX-License-Identifier: Apache-2.0
//! Meerschaum's instance methods backed by one authorized device session.
use meerschaum_plugin::{
    chrono::{DateTime, NaiveDateTime, SecondsFormat, Utc},
    prelude::*,
};
use serde::Deserialize;
use std::{
    collections::{BTreeSet, HashSet},
    io::{BufRead, BufReader, Write},
    process::{Command, Stdio},
    sync::Mutex,
};

const TRANSPORT: &str = concat!(
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

fn python(config: &Value) -> Command {
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
}

impl Entry {
    fn attributes(&self) -> Value {
        let locks = if self.write {
            json!(["clear", "drop", "delete", "parameters"])
        } else {
            json!(["all"])
        };
        json!({"pipe_id": self.id, "connector_keys":self.key.0, "metric_key":self.key.1,
            "location_key":self.key.2, "parameters": {"locks":locks,
            "micro":{"instance":self.instance,"read":self.read,"write":self.write,"filters":self.filters}}})
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

fn write_incident_or(req: &Request, key: &Key, failure: Error) -> Error {
    let query = json!({"op":"pending_write","key":key,"label":req.label});
    match exchange(&req.config, query) {
        Ok(Value::String(incident)) => error(incident),
        _ => failure,
    }
}

fn catalog_entry(req: &Request, key: &Key) -> Result<Option<Entry>> {
    let entries = catalog(&req.config).map_err(|failure| write_incident_or(req, key, failure))?;
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

fn read(config: &Value, key: &Key, query: &Value) -> Result<Vec<Value>> {
    let explicit = query["limit"].as_u64();
    if (!query["limit"].is_null() && explicit.is_none())
        || explicit.is_some_and(|n| n > MAX_ROWS as u64)
    {
        return Err(error("Device reads accept a limit from 0 to 128 rows."));
    }
    if query["order"]
        .as_str()
        .is_some_and(|s| !s.eq_ignore_ascii_case("asc"))
        || query["remote"] == true
    {
        return Err(error(
            "Device reads support local rows in ascending device order.",
        ));
    }
    let limit = explicit.map(|n| n as usize).unwrap_or(MAX_ROWS);
    let mut request = json!({"op":"read","key":key,"limit":limit});
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
                let us = v["$us"]
                    .as_i64()
                    .filter(|_| obj.len() == 1)
                    .and_then(DateTime::<Utc>::from_timestamp_micros)
                    .ok_or_else(|| error("Invalid UTC microseconds in device reply."))?;
                *v = json!(us.to_rfc3339_opts(SecondsFormat::Micros, true));
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

fn write(req: &Request, key: &Key, data: Option<Frame>) -> Result<Value> {
    let config = &req.config;
    if !catalog_entry(req, key)?.is_some_and(|entry| entry.write) {
        return Err(write_incident_or(
            req,
            key,
            error("Pipe is not exposed for writing."),
        ));
    }
    let frame = data.ok_or_else(|| error("Pipe.sync requires rows."))?;
    if frame.height() > MAX_ROWS {
        return Err(error("A device write accepts at most 128 rows."));
    }
    let dtypes = frame.dtypes();
    let mut rows = frame.to_rows();
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
    settings["_micro_label"] = json!(req.label);
    let message = exchange(&settings, json!({"op":"write","key":key,"rows":rows}))?;
    Ok(json!([
        true,
        message
            .as_str()
            .ok_or_else(|| error("Invalid write acknowledgement."))?
    ]))
}

pub(crate) fn call(req: &Request, data: Option<Frame>) -> Result<Reply> {
    let config = &req.config;
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
        "get_pipe_rowcount" => Ok(Reply::json(json!(read(config, &key, query)?.len()))),
        // A device does not publish an indexed datetime axis or a fixed schema.
        "get_sync_time" => Ok(Reply::json(Value::Null)),
        "get_pipe_columns_types" => Ok(Reply::json(json!({}))),
        "sync_pipe" => Ok(Reply::json(match write(req, &key, data) {
            Ok(reply) => reply,
            Err(reason) => json!([false, reason.to_string()]),
        })),
        _ => {
            let e = catalog_entry(req, &key)?;
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
    let instance = if let Some(s) = kw["mrsm_instance"].as_str() {
        s.to_string()
    } else {
        host::config(&["meerschaum", "instance"])?
            .as_str()
            .unwrap_or("")
            .to_owned()
    };
    let label = instance
        .strip_prefix("micro:")
        .filter(|l| !l.is_empty())
        .ok_or_else(|| error("Select a device with -i micro:<label>."))?;
    let config = host::config(&["meerschaum", "connectors", "micro", label])?;
    if !config.is_object() {
        return Err(error(format!(
            "Configure meerschaum.connectors.micro.{label}."
        )));
    }
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
            "Supply a device action, e.g. mrsm micro action show pipes -i micro:bench.",
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
