// SPDX-License-Identifier: Apache-2.0
use crate::{Failure, Outcome, Receiver, store, stream_id};
use meerschaum_plugin::{
    chrono::{DateTime, SecondsFormat},
    prelude::*,
};
use micromeerschaum::{Kind, Record, document};
use serde::Deserialize;
use std::collections::{BTreeMap, HashSet};

#[derive(Deserialize)]
pub(crate) struct Output {
    #[serde(deserialize_with = "crate::node_id")]
    origin: u64,
    #[serde(deserialize_with = "crate::node_id")]
    generation: u64,
    key: String,
    connector: String,
    metric: String,
    #[serde(default)]
    location: Option<String>,
    instance: String,
    columns: BTreeMap<String, String>,
    dtypes: BTreeMap<String, String>,
}
impl Output {
    fn identity(&self) -> String {
        json!([self.instance, self.connector, self.metric, self.location]).to_string()
    }
    fn validate(&self) -> Outcome<()> {
        micromeerschaum::check_key(self.key.as_bytes())?;
        if self.generation == 0
            || self.connector.is_empty()
            || self.metric.is_empty()
            || self.instance.is_empty()
            || self.columns.is_empty()
            || self
                .columns
                .values()
                .any(|name| !self.dtypes.contains_key(name))
            || matches!(self.location.as_deref(), Some("" | "None" | "[None]"))
            || self
                .dtypes
                .values()
                .any(|t| !matches!(t.as_str(), "int" | "bool" | "float" | "string" | "datetime"))
        {
            return Err(Failure::Configuration("Output needs an exact source generation, destination Pipe, typed keys and portable dtypes.".into()));
        }
        Ok(())
    }
    fn row(&self, record: Record) -> Outcome<Option<Value>> {
        if record.kind == Kind::Loss {
            return Ok(None);
        }
        let mut row = serde_json::Map::new();
        for (name, value) in document::decode(record.payload)? {
            use document::Value as D;
            let value = match (self.dtypes.get(name).map(String::as_str), value) {
                (Some(_), D::Null) => Value::Null,
                (Some("bool"), D::Bool(b)) => json!(b),
                (Some("int"), D::Int(i)) => json!(i),
                (Some("float"), D::Float(f)) => json!(f),
                (Some("string"), D::Text(s)) => json!(s),
                (Some("datetime"), D::Time(us)) => json!(
                    DateTime::from_timestamp_micros(us)
                        .ok_or_else(|| Failure::Output(format!(
                            "UTC timestamp out of scope at field {name}"
                        )))?
                        .to_rfc3339_opts(SecondsFormat::Micros, true)
                ),
                _ => {
                    return Err(Failure::Output(format!(
                        "Unexpected type or unmapped field {name}"
                    )));
                }
            };
            row.insert(name.to_string(), value);
        }
        if self
            .columns
            .values()
            .any(|name| row.get(name).is_none_or(Value::is_null))
        {
            return Err(Failure::Output(
                "Missing or null destination row key.".into(),
            ));
        }
        Ok(Some(Value::Object(row)))
    }

    fn publish(&self, row: Value) -> std::result::Result<(), String> {
        let pipe = host::pipe(
            &self.connector,
            &self.metric,
            self.location.as_deref(),
            Some(&self.instance),
        )
        .map_err(|e| e.to_string())?;
        let parameters = json!({"columns":self.columns, "dtypes":self.dtypes, "upsert":true});
        let pipe = if pipe.is_registered().map_err(|e| e.to_string())? {
            if pipe.parameters()["columns"] != parameters["columns"]
                || pipe.parameters()["upsert"] != true
            {
                return Err("Existing Pipe has incompatible row keys or upsert disabled.".into());
            }
            pipe
        } else {
            pipe.with_parameters(parameters)
        };
        let dtypes: Vec<_> = self
            .dtypes
            .iter()
            .filter(|(k, _)| row.get(k.as_str()).is_some())
            .map(|(k, t)| (k.as_str(), t.as_str()))
            .collect();
        let frame = Frame::from_rows([row], &dtypes).map_err(|e| e.to_string())?;
        let result = pipe.sync(frame).map_err(|e| e.to_string())?;
        if result.success {
            Ok(())
        } else {
            Err(result.message)
        }
    }
}
impl Receiver {
    pub(crate) fn replay(&self) -> Outcome<()> {
        let outputs: Vec<Output> = self
            .outputs
            .iter()
            .map(|v| serde_json::from_value(v.clone()))
            .collect::<std::result::Result<_, _>>()
            .map_err(|e| Failure::Configuration(e.to_string()))?;
        let mut targets = HashSet::new();
        for output in &outputs {
            output.validate()?;
            if !targets.insert(output.identity()) {
                return Err(Failure::Configuration(
                    "Map each output to its own destination Pipe.".into(),
                ));
            }
        }
        let mut failures = Vec::new();
        for output in &outputs {
            let stream = stream_id(
                1,
                self.destination,
                output.origin,
                output.generation,
                &output.key,
            );
            let target = output.identity();
            let result = store::run(store::replay(&self.raw_store, &stream, &target, |record| {
                if let Some(row) = output.row(record)? {
                    output.publish(row).map_err(Failure::Output)?;
                }
                Ok(())
            }));
            if let Err(error) = result {
                failures.push(format!("{target}: {error:?}"));
            }
        }
        if failures.is_empty() {
            Ok(())
        } else {
            Err(Failure::Output(failures.join("\n")))
        }
    }
}
