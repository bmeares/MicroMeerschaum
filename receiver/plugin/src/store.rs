// SPDX-License-Identifier: Apache-2.0
use super::{Failure, Outcome};
use micromeerschaum::{Kind, Record, protocol::Loss};
use sqlx::{Any, AnyConnection, Connection, Row, Transaction};
use std::{future::Future, sync::OnceLock};
use tokio::runtime::Runtime;

pub(crate) fn run<T>(work: impl Future<Output = T>) -> T {
    static RUNTIME: OnceLock<Runtime> = OnceLock::new();
    let runtime = RUNTIME.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("receiver runtime")
    });
    let _guard = runtime.enter();
    futures::executor::block_on(work)
}

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS mm_progress (
    stream BYTEA NOT NULL, output TEXT NOT NULL, applied BIGINT NOT NULL, PRIMARY KEY(stream,output));
CREATE TABLE IF NOT EXISTS mm_stream (stream BYTEA PRIMARY KEY, through_seq BIGINT NOT NULL);
CREATE TABLE IF NOT EXISTS mm_record (
    stream BYTEA NOT NULL, seq BIGINT NOT NULL, original BYTEA NOT NULL, PRIMARY KEY(stream,seq));
CREATE TABLE IF NOT EXISTS mm_route (
    stream BYTEA NOT NULL, sender TEXT NOT NULL, hops BYTEA NOT NULL, lo BIGINT NOT NULL, hi BIGINT NOT NULL,
    PRIMARY KEY(stream,sender,hops,lo,hi));
CREATE TABLE IF NOT EXISTS mm_range (
    stream BYTEA NOT NULL, lo BIGINT NOT NULL, hi BIGINT NOT NULL, PRIMARY KEY(stream,lo));
";

async fn connection(url: &str) -> Outcome<AnyConnection> {
    sqlx::any::install_default_drivers();
    let mut db = AnyConnection::connect(url).await?;
    if url.starts_with("sqlite:") {
        sqlx::query("PRAGMA busy_timeout = 30000")
            .execute(&mut db)
            .await?;
        sqlx::query("PRAGMA synchronous = FULL")
            .execute(&mut db)
            .await?;
    } else {
        sqlx::query("SET synchronous_commit = on")
            .execute(&mut db)
            .await?;
    }
    Ok(db)
}
pub(crate) async fn initialize(url: &str) -> Outcome<()> {
    let mut db = connection(url).await?;
    let mut tx = db.begin().await?;
    sqlx::raw_sql(SCHEMA).execute(&mut *tx).await?;
    tx.commit().await?;
    Ok(())
}

/// Rotate the sign bit to store every u64 in numeric order on both SQL engines.
pub(crate) fn number(seq: u64) -> i64 {
    (seq ^ (1 << 63)) as i64
}
pub(crate) fn sequence(number: i64) -> u64 {
    number as u64 ^ (1 << 63)
}

async fn lock_stream<'a>(
    url: &str,
    db: &'a mut AnyConnection,
    id: &[u8],
) -> Outcome<Transaction<'a, Any>> {
    let mut tx = db
        .begin_with(if url.starts_with("sqlite:") {
            "BEGIN IMMEDIATE"
        } else {
            "BEGIN"
        })
        .await?;
    sqlx::query("INSERT INTO mm_stream VALUES ($1,$2) ON CONFLICT DO NOTHING")
        .bind(id)
        .bind(number(0))
        .execute(&mut *tx)
        .await?;
    if !url.starts_with("sqlite:") {
        sqlx::query("SELECT through_seq FROM mm_stream WHERE stream=$1 FOR UPDATE")
            .bind(id)
            .fetch_one(&mut *tx)
            .await?;
    }
    Ok(tx)
}

async fn account(db: &mut AnyConnection, id: &[u8], lo: u64, hi: u64) -> Outcome<()> {
    // Adjacent or overlapping intervals become one; a declared range can span the entire u64 space.
    let rows = sqlx::query("SELECT lo,hi FROM mm_range WHERE stream=$1 AND hi >= $2 AND lo <= $3")
        .bind(id)
        .bind(number(lo.saturating_sub(1)))
        .bind(number(hi.saturating_add(1)))
        .fetch_all(&mut *db)
        .await?;
    let mut merged = (number(lo), number(hi));
    for row in rows {
        let old_lo = row.try_get::<i64, _>(0)?;
        merged.0 = merged.0.min(old_lo);
        merged.1 = merged.1.max(row.try_get(1)?);
        sqlx::query("DELETE FROM mm_range WHERE stream=$1 AND lo=$2")
            .bind(id)
            .bind(old_lo)
            .execute(&mut *db)
            .await?;
    }
    sqlx::query("INSERT INTO mm_range VALUES ($1,$2,$3)")
        .bind(id)
        .bind(merged.0)
        .bind(merged.1)
        .execute(&mut *db)
        .await?;
    Ok(())
}

pub(crate) async fn capture(
    url: &str,
    id: &[u8],
    sender: &str,
    hops: &[u8],
    records: &[Record<'_>],
    mut originals: &[u8],
    legacy: bool,
) -> Outcome<u64> {
    let mut db = connection(url).await?;
    let mut tx = lock_stream(url, &mut db, id).await?;
    for record in records {
        let (original, remaining) =
            originals.split_at(micromeerschaum::RECORD_HEADER_LEN + record.payload.len());
        originals = remaining;
        let seq = number(record.seq);
        let inserted =
            sqlx::query("INSERT INTO mm_record VALUES ($1,$2,$3) ON CONFLICT DO NOTHING")
                .bind(id)
                .bind(seq)
                .bind(original)
                .execute(&mut *tx)
                .await?
                .rows_affected();
        if inserted == 0 {
            let stored: Vec<u8> =
                sqlx::query_scalar("SELECT original FROM mm_record WHERE stream=$1 AND seq=$2")
                    .bind(id)
                    .bind(seq)
                    .fetch_one(&mut *tx)
                    .await?;
            if stored != original {
                return Err(Failure::Conflict);
            }
        } else {
            // A replica may restore an original previously accounted for by an eviction.
            // Repeat that record and its successors to preserve correction order.
            sqlx::query("UPDATE mm_progress SET applied=$1 WHERE stream=$2 AND applied >= $3")
                .bind(number(record.seq - 1))
                .bind(id)
                .bind(seq)
                .execute(&mut *tx)
                .await?;
        }
        if inserted != 0
            && !legacy
            && record.kind == Kind::Loss
            && let Loss::Evicted { first, last } = Loss::decode(record.seq, record.payload)?
        {
            account(&mut tx, id, first, last).await?;
        }
    }
    if let (Some(first), Some(last)) = (records.first(), records.last()) {
        sqlx::query("INSERT INTO mm_route VALUES ($1,$2,$3,$4,$5) ON CONFLICT DO NOTHING")
            .bind(id)
            .bind(sender)
            .bind(hops)
            .bind(number(first.seq))
            .bind(number(last.seq))
            .execute(&mut *tx)
            .await?;
        account(&mut tx, id, first.seq, last.seq).await?;
    }
    let through = sqlx::query_scalar::<_, i64>("SELECT hi FROM mm_range WHERE stream=$1 AND lo=$2")
        .bind(id)
        .bind(number(1))
        .fetch_optional(&mut *tx)
        .await?
        .unwrap_or(number(0));
    sqlx::query("UPDATE mm_stream SET through_seq=$1 WHERE stream=$2")
        .bind(through)
        .bind(id)
        .execute(&mut *tx)
        .await?;
    tx.commit().await?;
    Ok(sequence(through))
}

pub(crate) async fn replay(
    url: &str,
    id: &[u8],
    output: &str,
    mut publish: impl FnMut(Record) -> Outcome<()>,
) -> Outcome<()> {
    let mut db = connection(url).await?;
    let snapshot: Option<i64> =
        sqlx::query_scalar("SELECT through_seq FROM mm_stream WHERE stream=$1")
            .bind(id)
            .fetch_optional(&mut db)
            .await?;
    let Some(through) = snapshot else {
        return Ok(());
    };
    loop {
        // Serialize publishers of this stream across processes. Release between writes so
        // ingest can progress. Use a separate SQLite database for the ordinary Pipe.
        let mut tx = lock_stream(url, &mut db, id).await?;
        let applied: i64 =
            sqlx::query_scalar("SELECT applied FROM mm_progress WHERE stream=$1 AND output=$2")
                .bind(id)
                .bind(output)
                .fetch_optional(&mut *tx)
                .await?
                .unwrap_or(number(0));
        if applied >= through {
            tx.commit().await?;
            return Ok(());
        }
        let row = sqlx::query("SELECT seq,original FROM mm_record WHERE stream=$1 AND seq>$2 AND seq<=$3 ORDER BY seq LIMIT 1")
            .bind(id).bind(applied).bind(through).fetch_optional(&mut *tx).await?;
        let next = match row {
            Some(row) => {
                let seq: i64 = row.try_get(0)?;
                let original: Vec<u8> = row.try_get(1)?;
                let (record, used) = Record::decode(&original)?;
                if record.seq != sequence(seq) || used != original.len() {
                    return Err(Failure::Conflict);
                }
                publish(record).map_err(|e| {
                    Failure::Output(format!("{output}, sequence {}: {e:?}", record.seq))
                })?;
                seq
            }
            None => through, // Coverage through this position consists entirely of declared loss.
        };
        sqlx::query("INSERT INTO mm_progress VALUES ($1,$2,$3) ON CONFLICT(stream,output) DO UPDATE SET applied=excluded.applied")
            .bind(id).bind(output).bind(next).execute(&mut *tx).await?;
        tx.commit().await?;
    }
}

/// Local operator readout. Show outstanding materialization independently from delivery.
pub(crate) async fn status(url: &str) -> Outcome<Vec<serde_json::Value>> {
    let mut db = connection(url).await?;
    let mut tx = db.begin().await?;
    let streams = sqlx::query(
        "SELECT stream,through_seq,
        (SELECT count(*) FROM mm_record r WHERE r.stream=s.stream) AS records,
        (SELECT count(*) FROM mm_route r WHERE r.stream=s.stream) AS route_ranges
        FROM mm_stream s ORDER BY stream",
    )
    .fetch_all(&mut *tx)
    .await?;
    let mut result = Vec::new();
    for row in streams {
        let stream: Vec<u8> = row.try_get(0)?;
        if stream.len() < 25 {
            return Err(Failure::Conflict);
        }
        let id = |offset| {
            u64::from_be_bytes(
                stream[offset..offset + 8]
                    .try_into()
                    .expect("validated stream prefix"),
            )
        };
        let key = micromeerschaum::check_key(&stream[25..])?;
        let progress =
            sqlx::query("SELECT output,applied FROM mm_progress WHERE stream=$1 ORDER BY output")
                .bind(&stream)
                .fetch_all(&mut *tx)
                .await?;
        let mut outputs = Vec::new();
        for checkpoint in progress {
            outputs.push(serde_json::json!({
                "output":checkpoint.try_get::<String,_>(0)?, "applied":sequence(checkpoint.try_get(1)?),
            }));
        }
        result.push(serde_json::json!({
            "version":stream[0], "destination":id(1), "origin":id(9), "generation":id(17), "key":key,
            "durable_through":sequence(row.try_get(1)?), "records":row.try_get::<i64,_>(2)?,
            "route_ranges":row.try_get::<i64,_>(3)?, "outputs":outputs,
        }));
    }
    tx.commit().await?;
    Ok(result)
}
