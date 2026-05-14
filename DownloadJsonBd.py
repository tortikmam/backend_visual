import json
import psycopg2
from psycopg2.extras import execute_values
from datetime import datetime, timezone


def parse_timestamp(ms: int) -> datetime:
    """Конвертирует Unix milliseconds в datetime с timezone."""
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc)


def read_jsonl(filepath):
    """Генератор для JSONL — каждая строка отдельный JSON."""
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                yield line


def insert_batch(conn, json_lines: list[str]):
    """Массовая вставка пачки записей."""
    records = []
    lte_entries = []
    
    for line in json_lines:
        data = json.loads(line)
        
        records.append((
            data.get("accuracy"),
            data.get("latitude"),
            data.get("longitude"),
            data.get("provider"),
            parse_timestamp(data["recordedTime"]) if "recordedTime" in data else None,
            data.get("source"),
            parse_timestamp(data["timestamp"]) if "timestamp" in data else None
        ))
        
        lte = data.get("telephony", {}).get("LTE")
        if lte:
            identity = lte.get("identity", {})
            signal = lte.get("signal", {})
            lte_entries.append((
                identity.get("band"), identity.get("ci"), identity.get("earfcn"),
                identity.get("mcc"), identity.get("mnc"), identity.get("pci"), identity.get("tac"),
                signal.get("asu"), signal.get("rsrp"), signal.get("rsrq"), signal.get("rssnr")
            ))
        else:
            lte_entries.append(None)
    
    with conn.cursor() as cur:
        execute_values(cur, """
            INSERT INTO location_records 
                (accuracy, latitude, longitude, provider, recorded_time, source, event_timestamp)
            VALUES %s
            RETURNING id
        """, records)
        
        location_ids = [row[0] for row in cur.fetchall()]
        
        lte_to_insert = [
            (loc_id, *lte_data)
            for loc_id, lte_data in zip(location_ids, lte_entries)
            if lte_data is not None
        ]
        
        if lte_to_insert:
            execute_values(cur, """
                INSERT INTO telephony_lte
                    (location_record_id, band, ci, earfcn, mcc, mnc, pci, tac, asu, rsrp, rsrq, rssnr)
                VALUES %s
            """, lte_to_insert)
        
        conn.commit()
    
    return len(records)


def process_file(conn, filepath: str, batch_size: int = 1000):
    """Читает файл пачками и вставляет в БД."""
    total = 0
    batch = []
    
    for line in read_jsonl(filepath): 
        batch.append(line)
        
        if len(batch) >= batch_size:
            inserted = insert_batch(conn, batch)
            total += inserted
            print(f"Вставлено: {total} записей...")
            batch = []
    
    # Остаток
    if batch:
        inserted = insert_batch(conn, batch)
        total += inserted
    
    print(f"Готово! Всего вставлено: {total} записей.")
    return total


# --- Запуск ---

if __name__ == "__main__":
    conn = psycopg2.connect(
        host="localhost",
        database="locations",
        user="postgres",
        password="12345" 
    )
    
    process_file(conn, "VsCode/telemetry_log.json", batch_size=1000)
    
    conn.close()