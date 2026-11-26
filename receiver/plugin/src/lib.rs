// SPDX-License-Identifier: Apache-2.0
//! Authenticated Meerschaum batch routes and independent raw capture.
mod device;
mod device_shell;
mod replay;
mod store;

use meerschaum_plugin::api::{RouteRequest, RouteResponse, RouteSpec};
use meerschaum_plugin::prelude::*;
use micromeerschaum::{
    Batch as LegacyBatch, Hop, Record,
    protocol::{Batch, Receipt, Role, Stream},
};
use serde::Deserialize;

#[derive(Debug)]
pub enum Failure {
    Invalid(micromeerschaum::Error),
    Forbidden,
    Conflict,
    Configuration(String),
    Database(sqlx::Error),
    Output(String),
}
impl From<sqlx::Error> for Failure {
    fn from(value: sqlx::Error) -> Self {
        Self::Database(value)
    }
}
impl From<micromeerschaum::Error> for Failure {
    fn from(value: micromeerschaum::Error) -> Self {
        Self::Invalid(value)
    }
}
type Outcome<T> = std::result::Result<T, Failure>;

// JSON configuration may pass large numbers through a host with signed integer storage.
fn node_id<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> std::result::Result<u64, D::Error> {
    let v = Value::deserialize(deserializer)?;
    v.as_u64()
        .or_else(|| v.as_str()?.parse().ok())
        .ok_or_else(|| {
            serde::de::Error::custom("Expected an unsigned 64-bit integer or decimal string.")
        })
}
#[derive(Deserialize)]
struct Grant {
    token_id: String,
    #[serde(deserialize_with = "node_id")]
    origin: u64,
    key: String,
    #[serde(default)]
    legacy: bool,
}
#[derive(Deserialize)]
pub struct Receiver {
    raw_store: String,
    #[serde(deserialize_with = "node_id")]
    destination: u64,
    grants: Vec<Grant>,
    #[serde(default)]
    outputs: Vec<Value>,
}
/// The namespace includes the protocol version and destination. The exact key may contain NUL.
fn stream_id(version: u8, destination: u64, origin: u64, generation: u64, key: &str) -> Vec<u8> {
    let mut id = vec![version];
    for number in [destination, origin, generation] {
        id.extend_from_slice(&number.to_be_bytes());
    }
    id.extend_from_slice(key.as_bytes());
    id
}
fn route_bytes(hops: &[Hop]) -> Vec<u8> {
    let mut route = Vec::with_capacity(24 * hops.len());
    for hop in hops {
        route.extend_from_slice(&hop.node.to_le_bytes());
        route.extend_from_slice(&hop.received_us.to_le_bytes());
        route.extend_from_slice(&[hop.transport as u8, 0, 0, 0, 0, 0, 0, 0]);
    }
    route
}
impl Receiver {
    pub fn from_config(config: Value) -> Outcome<Self> {
        let receiver: Self =
            serde_json::from_value(config).map_err(|e| Failure::Configuration(e.to_string()))?;
        if !["sqlite:", "postgres:", "postgresql:"]
            .iter()
            .any(|p| receiver.raw_store.starts_with(p))
        {
            return Err(Failure::Configuration(
                "raw_store must be SQLite or PostgreSQL".into(),
            ));
        }
        Ok(receiver)
    }
    pub fn initialize(&self) -> Outcome<()> {
        store::run(store::initialize(&self.raw_store))
    }
    pub fn receive(&self, sender: &str, body: &[u8], legacy: bool) -> Outcome<Vec<u8>> {
        let (stream, hops, records): (_, _, Vec<Record>) = if legacy {
            let batch = LegacyBatch::decode(body)?;
            (
                Stream {
                    origin: batch.header.origin,
                    generation: 0,
                    key: batch.header.key,
                },
                batch.header.hops,
                batch.records,
            )
        } else {
            let batch = Batch::decode(body)?;
            if batch.destination != self.destination {
                return Err(Failure::Forbidden);
            }
            (batch.stream, batch.hops, batch.records)
        };
        if !self.grants.iter().any(|g| {
            g.token_id == sender
                && g.origin == stream.origin
                && g.key == stream.key
                && (!legacy || g.legacy)
        }) {
            return Err(Failure::Forbidden);
        }
        let id = stream_id(
            u8::from(!legacy),
            self.destination,
            stream.origin,
            stream.generation,
            stream.key,
        );
        let records_size: usize = records
            .iter()
            .map(|r| micromeerschaum::RECORD_HEADER_LEN + r.payload.len())
            .sum();
        let originals = &body[body.len() - records_size..];
        let through = store::run(store::capture(
            &self.raw_store,
            &id,
            sender,
            &route_bytes(&hops),
            &records,
            originals,
            legacy,
        ))?;
        if legacy {
            return Ok(
                json!({"origin":stream.origin,"pipe":stream.key,"durable_through":through})
                    .to_string()
                    .into_bytes(),
            );
        }
        let mut reply = Vec::new();
        Receipt {
            stream,
            role: Role::Destination,
            issuer: self.destination,
            destination: self.destination,
            through,
        }
        .encode(&mut reply)?;
        Ok(reply)
    }
}

#[derive(Default)]
struct ReceiverPlugin;
const SCOPE: &str = "mm:ingest";
fn configured() -> Result<Receiver> {
    Receiver::from_config(host::plugin_config()?)
        .map_err(|_| Error::Message("Configure the receiver in plugins.micro.".into()))
}
impl Plugin for ReceiverPlugin {
    fn manifest(&self) -> Value {
        Manifest::new()
            .connector(ConnectorType::new("micro", device::METHODS.iter().copied()).instance())
            .action("micro_shell", "Open the selected device shell.")
            .action("micro_action", "Run one action on the selected device.")
            .action(
                "micro_reconcile",
                "Permit a new write after inspecting an unresolved device Pipe.",
            )
            .api_route(
                RouteSpec::new("ingest", "POST", "/micromeerschaum/v1/batch").scopes([SCOPE]),
            )
            .api_route(
                RouteSpec::new("legacy", "POST", "/micromeerschaum/v0/batch").scopes([SCOPE]),
            )
            .action(
                "micro_receiver_status",
                "Show raw delivery prefixes, route counts and applied checkpoints.",
            )
            .action(
                "micro_receiver_replay",
                "Publish the durable prefix to configured ordinary Pipes.",
            )
            .action(
                "micro_receiver_init",
                "Initialize the configured raw store.",
            )
            .into()
    }
    fn api_route(&self, request: &RouteRequest) -> Result<RouteResponse> {
        let Some(sender) = request
            .principal
            .as_ref()
            .filter(|p| p.is_token())
            .and_then(|p| p.token_id.as_deref())
        else {
            return Ok(RouteResponse::error(401, "A device token is required."));
        };
        let legacy = request.name == "legacy";
        Ok(match configured()?.receive(sender, &request.body, legacy) {
            Ok(body) => RouteResponse::bytes(
                if legacy {
                    "application/json"
                } else {
                    "application/octet-stream"
                },
                body,
            ),
            Err(Failure::Invalid(error)) => RouteResponse::error(400, error.name()),
            Err(Failure::Forbidden) => {
                RouteResponse::error(403, "Unauthorized stream or destination.")
            }
            Err(Failure::Conflict) => {
                RouteResponse::error(409, "Original bytes conflict with stored identity.")
            }
            Err(_) => RouteResponse::error(503, "Raw store unavailable."),
        })
    }
    fn call(&self, request: &Request, data: Option<Frame>) -> Result<Reply> {
        match (request.method.as_str(), request.context["name"].as_str()) {
            ("action", Some(command @ ("micro_shell" | "micro_action" | "micro_reconcile"))) => {
                device::action(request, command)
            }
            ("api_route", _) => meerschaum_plugin::sdk::call_server_extension(self, request),
            ("action", Some("micro_receiver_status")) => {
                let rows = store::run(store::status(&configured()?.raw_store))
                    .map_err(|e| Error::Message(format!("{e:?}")))?;
                let nopretty = request.context["kwargs"]["nopretty"]
                    .as_bool()
                    .unwrap_or(false);
                let text = if nopretty {
                    json!(rows).to_string()
                } else {
                    let unicode = host::config(&["formatting", "unicode"])
                        .ok()
                        .and_then(|v| v.as_bool())
                        .unwrap_or(true);
                    format_status_table(&rows, unicode)
                };
                println!("{text}");
                let _ = std::io::Write::flush(&mut std::io::stdout());
                Ok(Reply::json(json!([true, "Success"])))
            }
            ("action", Some("micro_receiver_replay")) => {
                let result = configured()?.replay();
                Ok(Reply::json(match result {
                    Ok(()) => json!([true, "Replayed receiver outputs."]),
                    Err(error) => json!([false, format!("Replay stopped: {error:?}")]),
                }))
            }
            ("action", Some("micro_receiver_init")) => {
                let receiver = configured()?;
                receiver
                    .initialize()
                    .map_err(|e| Error::Message(format!("{e:?}")))?;
                Ok(Reply::json(json!([
                    true,
                    "Initialized receiver raw store."
                ])))
            }
            _ if request.connector_type == "micro" => device::call(request, data),
            _ => Err(Error::Unsupported(request.method.clone())),
        }
    }
}

fn sanitize_cell(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        if ch.is_control() {
            out.extend(ch.escape_default());
        } else {
            out.push(ch);
        }
    }
    out
}

fn format_stream_key(key: &str) -> String {
    let mut parts = key.split('\x1f');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some(connector), Some(metric), Some(location), None) => {
            let connector = sanitize_cell(connector);
            let metric = sanitize_cell(metric);
            if location.is_empty() {
                format!("Pipe('{connector}', '{metric}')")
            } else {
                let location = sanitize_cell(location);
                format!("Pipe('{connector}', '{metric}', '{location}')")
            }
        }
        _ => sanitize_cell(&key.replace('\x1f', " / ")),
    }
}

fn format_output_label(raw: &str) -> String {
    if let Ok((instance, connector, metric, location)) =
        serde_json::from_str::<(String, String, String, Option<String>)>(raw)
    {
        let instance = sanitize_cell(&instance);
        let connector = sanitize_cell(&connector);
        let metric = sanitize_cell(&metric);
        match location.as_deref().filter(|l| !l.is_empty()) {
            Some(location) => {
                let location = sanitize_cell(location);
                format!("Pipe('{connector}', '{metric}', '{location}', instance='{instance}')")
            }
            None => format!("Pipe('{connector}', '{metric}', instance='{instance}')"),
        }
    } else {
        sanitize_cell(raw)
    }
}

fn format_num(value: &Value) -> String {
    if let Some(n) = value.as_u64() {
        n.to_string()
    } else if let Some(n) = value.as_i64() {
        n.to_string()
    } else if let Some(s) = value.as_str() {
        sanitize_cell(s)
    } else {
        "-".to_string()
    }
}

fn format_outputs(outputs: &Value) -> Vec<String> {
    let Some(items) = outputs.as_array() else {
        return vec!["-".to_string()];
    };
    if items.is_empty() {
        return vec!["-".to_string()];
    }
    items
        .iter()
        .map(|item| {
            let label = item["output"]
                .as_str()
                .map(format_output_label)
                .unwrap_or_else(|| "?".to_string());
            let applied = format_num(&item["applied"]);
            format!("{label}: {applied}")
        })
        .collect()
}

fn pad_cell(text: &str, width: usize, right: bool) -> String {
    let len = text.chars().count();
    let pad = width.saturating_sub(len);
    if right {
        format!("{}{text}", " ".repeat(pad))
    } else {
        format!("{text}{}", " ".repeat(pad))
    }
}

pub fn format_status_table(rows: &[Value], unicode: bool) -> String {
    if rows.is_empty() {
        return "No captured receiver streams.".to_string();
    }
    const HEADERS: [&str; 8] = [
        "Version",
        "Stream",
        "Origin",
        "Generation",
        "Durable",
        "Records",
        "Routes",
        "Outputs",
    ];
    const RIGHT_ALIGN: [bool; 8] = [false, false, true, true, true, true, true, false];

    let mut table_rows: Vec<[String; 8]> = Vec::new();
    for row in rows {
        let version = row["version"]
            .as_u64()
            .map(|v| format!("v{v}"))
            .unwrap_or_else(|| "-".to_string());
        let stream = row["key"]
            .as_str()
            .map(format_stream_key)
            .unwrap_or_else(|| "-".to_string());
        let origin = format_num(&row["origin"]);
        let generation = format_num(&row["generation"]);
        let durable = format_num(&row["durable_through"]);
        let records = format_num(&row["records"]);
        let routes = format_num(&row["route_ranges"]);
        let outputs = format_outputs(&row["outputs"]);
        table_rows.push([
            version,
            stream,
            origin,
            generation,
            durable,
            records,
            routes,
            outputs[0].clone(),
        ]);
        for extra in &outputs[1..] {
            table_rows.push([
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                String::new(),
                extra.clone(),
            ]);
        }
    }

    let mut widths = [0usize; 8];
    for (i, header) in HEADERS.iter().enumerate() {
        widths[i] = header.chars().count();
    }
    for row in &table_rows {
        for (i, cell) in row.iter().enumerate() {
            widths[i] = widths[i].max(cell.chars().count());
        }
    }

    let (vert, horiz, cross) = if unicode {
        ('│', "─", '┼')
    } else {
        ('|', "-", '+')
    };

    let render_row = |cells: &[&str; 8]| -> String {
        let mut line = String::new();
        for i in 0..8 {
            if i > 0 {
                line.push(vert);
            }
            line.push(' ');
            if i == 7 {
                line.push_str(cells[i]);
            } else {
                line.push_str(&pad_cell(cells[i], widths[i], RIGHT_ALIGN[i]));
                line.push(' ');
            }
        }
        line
    };

    let mut divider = String::new();
    for (i, &w) in widths.iter().enumerate() {
        if i > 0 {
            divider.push(cross);
        }
        divider.push_str(&horiz.repeat(w + 2));
    }

    let mut lines = Vec::with_capacity(table_rows.len() + 2);
    lines.push(render_row(&HEADERS));
    lines.push(divider);
    for row in &table_rows {
        let refs = [
            row[0].as_str(),
            row[1].as_str(),
            row[2].as_str(),
            row[3].as_str(),
            row[4].as_str(),
            row[5].as_str(),
            row[6].as_str(),
            row[7].as_str(),
        ];
        lines.push(render_row(&refs));
    }
    lines.join("\n")
}

meerschaum_plugin::export_plugin!(ReceiverPlugin);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_table_handles_empty_store() {
        assert_eq!(
            format_status_table(&[], true),
            "No captured receiver streams."
        );
        assert_eq!(
            format_status_table(&[], false),
            "No captured receiver streams."
        );
    }

    #[test]
    fn stream_key_formats_unit_separators_cleanly() {
        assert_eq!(
            format_stream_key("sensor\x1ftemperature\x1f"),
            "Pipe('sensor', 'temperature')"
        );
        assert_eq!(
            format_stream_key("plugin:sensor\x1flevel\x1fgreenhouse-east"),
            "Pipe('plugin:sensor', 'level', 'greenhouse-east')"
        );
        assert_eq!(
            format_stream_key("sensor\x1fbad\0metric\x1f"),
            "Pipe('sensor', 'bad\\u{0}metric')"
        );
    }

    #[test]
    fn status_table_formats_single_stream_with_outputs_and_unicode_rules() {
        let rows = vec![json!({
            "version": 1,
            "destination": 22,
            "origin": 11,
            "generation": 42,
            "key": "plugin:sensor\x1fnível\x1fgreenhouse-east",
            "durable_through": 8,
            "records": 7,
            "route_ranges": 3,
            "outputs": [
                {"output": "[\"sql:mmtest\",\"weather\",\"temperature\",null]", "applied": 7},
                {"output": "[\"sql:mmtest\",\"weather\",\"humidity\",\"roof\"]", "applied": 5},
            ],
        })];
        let table = format_status_table(&rows, true);
        let lines: Vec<&str> = table.lines().collect();
        assert_eq!(lines.len(), 4, "{table}");
        assert!(!table.contains('\x1f'));
        assert!(lines[0].contains("Version") && lines[0].contains("Outputs"));
        assert!(lines[1].contains('─') && lines[1].contains('┼'));
        assert!(lines[2].contains("v1"));
        assert!(lines[2].contains("Pipe('plugin:sensor', 'nível', 'greenhouse-east')"));
        assert!(lines[2].contains("Pipe('weather', 'temperature', instance='sql:mmtest'): 7"));
        assert!(lines[3].contains("Pipe('weather', 'humidity', 'roof', instance='sql:mmtest'): 5"));

        // Verify every vertical separator aligns with divider intersections by character index.
        let cross_cols: Vec<usize> = lines[1]
            .chars()
            .enumerate()
            .filter_map(|(i, c)| (c == '┼').then_some(i))
            .collect();
        assert_eq!(cross_cols.len(), 7);
        for &line in &[lines[0], lines[2], lines[3]] {
            let bar_cols: Vec<usize> = line
                .chars()
                .enumerate()
                .filter_map(|(i, c)| (c == '│').then_some(i))
                .collect();
            assert_eq!(bar_cols, cross_cols, "misaligned row:\n{table}");
        }
    }

    #[test]
    fn status_table_formats_multiple_streams_in_ascii_mode() {
        let rows = vec![
            json!({
                "version": 0,
                "destination": 4321,
                "origin": 11,
                "generation": 0,
                "key": "sensor\x1ftemperature\x1f",
                "durable_through": 1,
                "records": 1,
                "route_ranges": 1,
                "outputs": [],
            }),
            json!({
                "version": 1,
                "destination": 4321,
                "origin": 81985529216486895u64,
                "generation": 18364758544493064720u64,
                "key": "sensor\x1ftemperature\x1f",
                "durable_through": 8,
                "records": 7,
                "route_ranges": 5,
                "outputs": [
                    {"output": "custom_sink", "applied": 7},
                ],
            }),
        ];
        let table = format_status_table(&rows, false);
        let lines: Vec<&str> = table.lines().collect();
        assert_eq!(lines.len(), 4, "{table}");
        assert!(table.is_ascii(), "expected pure ASCII:\n{table}");
        assert!(lines[1].contains('-') && lines[1].contains('+'));
        assert!(lines[2].ends_with("| -"));
        assert!(lines[3].ends_with("| custom_sink: 7"));

        let cross_cols: Vec<usize> = lines[1]
            .chars()
            .enumerate()
            .filter_map(|(i, c)| (c == '+').then_some(i))
            .collect();
        assert_eq!(cross_cols.len(), 7);
        for &line in &[lines[0], lines[2], lines[3]] {
            let bar_cols: Vec<usize> = line
                .chars()
                .enumerate()
                .filter_map(|(i, c)| (c == '|').then_some(i))
                .collect();
            assert_eq!(bar_cols, cross_cols, "misaligned ASCII row:\n{table}");
        }
    }
}
