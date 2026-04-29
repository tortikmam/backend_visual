import json
import psycopg2
import re

DB_CONFIG = {
    "dbname": "mariadb",
    "user": "postgres",
    "password": "GodOFwar2018",
    "host": "localhost",
    "port": "5432"
}

def parse_raw_data(raw_text):
    """Парсит raw_data и возвращает список вышек"""
    towers = []
    
    # Разбиваем по "CellInfoLte:"
    blocks = raw_text.split('CellInfoLte:')
    
    for block in blocks[1:]:  # Пропускаем первую часть с LAT/LON
        tower = {}
        
        # Registered
        reg_match = re.search(r'Registered:\s*(\w+)', block)
        tower['registered'] = (reg_match.group(1) == 'true') if reg_match else False
        
        # CellIdentityLte параметры
        pci_match = re.search(r'PCI:\s*(\d+)', block)
        tower['pci'] = int(pci_match.group(1)) if pci_match else None
        
        tac_match = re.search(r'TAC:\s*(\d+)', block)
        tac_val = int(tac_match.group(1)) if tac_match else None
        tower['tac'] = None if tac_val == 2147483647 else tac_val
        
        ci_match = re.search(r'CI:\s*(\d+)', block)
        ci_val = int(ci_match.group(1)) if ci_match else None
        tower['ci'] = None if ci_val == 2147483647 else ci_val
        
        earfcn_match = re.search(r'EARFCN:\s*(\d+)', block)
        tower['earfcn'] = int(earfcn_match.group(1)) if earfcn_match else None
        
        mcc_match = re.search(r'MCC:\s*(\d+)', block)
        tower['mcc'] = int(mcc_match.group(1)) if mcc_match else None
        
        mnc_match = re.search(r'MNC:\s*(\d+)', block)
        tower['mnc'] = int(mnc_match.group(1)) if mnc_match else None
        
        # DBM
        dbm_match = re.search(r'DBM:\s*(-?\d+)', block)
        tower['dbm'] = int(dbm_match.group(1)) if dbm_match else None
        
        # CellSignalStrengthLte параметры
        ta_match = re.search(r'Timing Advance:\s*(\d+)', block)
        ta_val = int(ta_match.group(1)) if ta_match else None
        tower['timing_advance'] = None if ta_val == 2147483647 else ta_val
        
        asu_match = re.search(r'ASU Level:\s*(\d+)', block)
        tower['asu_level'] = int(asu_match.group(1)) if asu_match else None
        
        towers.append(tower)
    
    return towers


def migrate_data():
    try:
        conn = psycopg2.connect(**DB_CONFIG)
        cur = conn.cursor()

        with open('../build/telemetry_log.json', 'r') as file:
            for line_num, line in enumerate(file, 1):
                if not line.strip():
                    continue

                try:
                    data = json.loads(line)
                except json.JSONDecodeError:
                    print(f"Строка {line_num}: невалидный JSON, пропускаем")
                    continue

                timestamp = data.get('timestamp')
                if not timestamp:
                    print(f"Строка {line_num}: нет timestamp, пропускаем")
                    continue

                # 1. Вставляем основной замер
                insert_measurement = """
                INSERT INTO measurements (timestamp, lat, lon, rsrp, rsrq, rssi)
                VALUES (%s, %s, %s, %s, %s, %s)
                ON CONFLICT (timestamp) DO NOTHING
                """
                
                cur.execute(insert_measurement, (
                    timestamp,
                    data.get('lat'),
                    data.get('lon'),
                    data.get('rsrp'),
                    data.get('rsrq'),
                    data.get('rssi')
                ))

                # 2. Парсим вышки из raw_data
                raw_data = data.get('raw_data', '')
                if raw_data:
                    towers = parse_raw_data(raw_data)
                    
                    # 3. Вставляем вышки (БЕЗ rsrq и rssi)
                    insert_tower = """
                    INSERT INTO cell_towers (
                        measurement_timestamp, mcc, mnc, pci, tac, ci, earfcn,
                        registered, dbm, timing_advance, asu_level, tower_order
                    ) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
                    """
                    
                    for idx, tower in enumerate(towers, 1):
                        cur.execute(insert_tower, (
                            timestamp,
                            tower.get('mcc'),
                            tower.get('mnc'),
                            tower.get('pci'),
                            tower.get('tac'),
                            tower.get('ci'),
                            tower.get('earfcn'),
                            tower.get('registered'),
                            tower.get('dbm'),
                            tower.get('timing_advance'),
                            tower.get('asu_level'),
                            idx
                        ))
                
                # Коммитим каждые 100 записей чтобы не жрать память
                if line_num % 100 == 0:
                    conn.commit()
                    print(f"Обработано {line_num} строк...")

        conn.commit()
        print(f"Готово! Всего обработано {line_num} строк")

    except Exception as e:
        print(f"Ошибка: {e}")
        conn.rollback()
    finally:
        if conn:
            cur.close()
            conn.close()


if __name__ == "__main__":
    migrate_data()