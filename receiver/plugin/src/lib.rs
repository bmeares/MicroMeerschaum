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
                Ok(Reply::json(json!([true, json!(rows).to_string()])))
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
meerschaum_plugin::export_plugin!(ReceiverPlugin);
