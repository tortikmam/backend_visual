#include "telemetry.hpp"
#include <iostream>
#include <fstream>
#include <pqxx/pqxx>

using namespace std;

// Измени под свои реальные параметры подключения
const string DB_CONN = "host=127.0.0.1 port=5432 dbname=locations user=postgres password=12345";

bool check_db_alive() {
    try {
        pqxx::connection c(DB_CONN);
        return c.is_open();
    } catch (...) {
        return false;
    }
}

void init_database() {
    g_data.db_connected = check_db_alive();
    sync_all_data();
}

void sync_all_data() {
    g_data.clear_all();

    if (g_data.db_connected) {
        try {
            pqxx::connection c(DB_CONN);
            pqxx::nontransaction N(c);

            // Загружаем location_records с временем из event_timestamp
            pqxx::result res = N.exec(
                "SELECT latitude, longitude, event_timestamp, accuracy "
                "FROM location_records "
                "ORDER BY event_timestamp ASC"
            );
            
            for (auto const &row : res) {
                // Используем event_timestamp как время (в секундах с epoch)
                double ts = 0;
                if (!row["event_timestamp"].is_null()) {
                    string ts_str = row["event_timestamp"].as<string>();
                    // Парсим timestamp, убираем timezone если есть
                    size_t pos = ts_str.find('+');
                    if (pos != string::npos) ts_str = ts_str.substr(0, pos);
                    pos = ts_str.find(' ');
                    if (pos != string::npos) {
                        // Формат "YYYY-MM-DD HH:MM:SS"
                        struct tm tm = {};
                        sscanf(ts_str.c_str(), "%d-%d-%d %d:%d:%d", 
                               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                               &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
                        tm.tm_year -= 1900;
                        tm.tm_mon -= 1;
                        ts = (double)mktime(&tm);
                    }
                }
                
                if (g_data.base_timestamp == 0) g_data.base_timestamp = ts;
                double elapsed = ts - g_data.base_timestamp;

                double lat = row["latitude"].as<double>();
                double lon = row["longitude"].as<double>();

                if (lat != 0.0 && lon != 0.0) {
                    g_data.history_lat.push_back(lat);
                    g_data.history_lon.push_back(lon);
                    g_data.history_time.push_back(elapsed);
                }
                g_data.max_recorded_time = (float)elapsed;
            }

            // Загружаем cell data из telephony_lte
            pqxx::result cell_res = N.exec(
                "SELECT lr.event_timestamp, tl.earfcn, tl.pci, tl.rsrp, tl.rsrq, tl.band "
                "FROM telephony_lte tl "
                "JOIN location_records lr ON tl.location_record_id = lr.id "
                "ORDER BY lr.event_timestamp ASC"
            );

            for (auto const &row : cell_res) {
                double ts = 0;
                if (!row["event_timestamp"].is_null()) {
                    string ts_str = row["event_timestamp"].as<string>();
                    size_t pos = ts_str.find('+');
                    if (pos != string::npos) ts_str = ts_str.substr(0, pos);
                    pos = ts_str.find(' ');
                    if (pos != string::npos) {
                        struct tm tm = {};
                        sscanf(ts_str.c_str(), "%d-%d-%d %d:%d:%d", 
                               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                               &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
                        tm.tm_year -= 1900;
                        tm.tm_mon -= 1;
                        ts = (double)mktime(&tm);
                    }
                }
                
                double elapsed = ts - g_data.base_timestamp;
                
                string earfcn = row["earfcn"].is_null() ? "0" : row["earfcn"].as<string>();
                string pci = row["pci"].is_null() ? "0" : row["pci"].as<string>();
                string band = row["band"].is_null() ? "0" : row["band"].as<string>();
                
                string label = "LTE_B" + band + "_P" + pci + "_E" + earfcn;
                
                int val = row["rsrp"].is_null() ? 0 : row["rsrp"].as<int>();
                if (val != 0) {
                    g_data.cell_logs[label].x_time.push_back(elapsed);
                    g_data.cell_logs[label].y_rsrp.push_back((double)val);
                }
            }

            g_data.data_source = "PostgreSQL";
            g_data.view_max_time = g_data.max_recorded_time;
            cout << "[DB] Synced from SQL. Points: " << res.size() << endl;
            return;
        } catch (const exception &e) {
            cerr << "[DB] SQL sync failed: " << e.what() << endl;
            g_data.db_connected = false;
        }
    }

    // Fallback на JSON
    g_data.data_source = "Local JSON (Backup)";
    ifstream log_file("telemetry_log.json");
    string line;
    while (getline(log_file, line)) {
        if (!line.empty()) parse_json_to_data(line);
    }
    g_data.view_max_time = g_data.max_recorded_time;
    cout << "[DB] Synced from JSON. Max time: " << g_data.max_recorded_time << endl;
}

std::vector<DbPoint> LoadPointsFromDatabase() {
    std::vector<DbPoint> points;
    try {
        pqxx::connection c(DB_CONN);
        pqxx::nontransaction N(c);
        
        // Просто latitude/longitude из location_records
        pqxx::result res = N.exec(
            "SELECT latitude, longitude FROM location_records "
            "WHERE latitude != 0 AND longitude != 0"
        );
        
        for (auto const &row : res) {
            DbPoint pt;
            pt.latitude = row["latitude"].as<double>();
            pt.longitude = row["longitude"].as<double>();
            pt.rsrp = -100; // Нет rsrp в location_records напрямую
            points.push_back(pt);
        }
        cout << "[DB] Loaded " << points.size() << " points for map" << endl;
    } catch (const exception &e) {
        cerr << "[DB] LoadPoints failed: " << e.what() << endl;
    }
    return points;
}

void save_packet(const string& raw_json) {
    // Сохраняем в JSON как backup
    {
        ofstream log_file("telemetry_log.json", ios::app);
        if (log_file.is_open()) log_file << raw_json << endl;
    }

    if (g_data.db_connected) {
        try {
            pqxx::connection c(DB_CONN);
            pqxx::work W(c);

            // Парсим JSON и вставляем в location_records
            // Примечание: твой JSON формат отличается от формата друга
            // Нужно адаптировать под структуру твоих таблиц
            
            double lat = stod(find_val(raw_json, "latitude"));
            double lon = stod(find_val(raw_json, "longitude"));
            double accuracy = stod(find_val(raw_json, "accuracy"));
            string provider = find_val(raw_json, "provider");
            string source = find_val(raw_json, "source");
            
            // Вставляем location_records
            pqxx::result res = W.exec_params(
                "INSERT INTO location_records (latitude, longitude, accuracy, provider, source, recorded_time, event_timestamp) "
                "VALUES ($1, $2, $3, $4, $5, NOW(), NOW()) RETURNING id",
                lat, lon, accuracy, provider, source
            );

            int location_id = res[0][0].as<int>();

            // Вставляем telephony_lte если есть
            if (raw_json.find("\"telephony\"") != string::npos) {
                // Парсим telephony.LTE
                size_t lte_start = raw_json.find("\"LTE\"");
                if (lte_start != string::npos) {
                    size_t lte_end = raw_json.find("}", lte_start);
                    string lte_json = raw_json.substr(lte_start, lte_end - lte_start + 1);
                    
                    // Парсим identity и signal
                    size_t id_start = raw_json.find("\"identity\"", lte_start);
                    size_t sig_start = raw_json.find("\"signal\"", lte_start);
                    
                    if (id_start != string::npos && sig_start != string::npos) {
                        size_t id_end = raw_json.find("}", id_start);
                        size_t sig_end = raw_json.find("}", sig_start);
                        
                        string id_json = raw_json.substr(id_start, id_end - id_start + 1);
                        string sig_json = raw_json.substr(sig_start, sig_end - sig_start + 1);
                        
                        int band = stoi(find_val(id_json, "band"));
                        int64_t ci = stoll(find_val(id_json, "ci"));
                        int earfcn = stoi(find_val(id_json, "earfcn"));
                        string mcc = find_val(id_json, "mcc");
                        string mnc = find_val(id_json, "mnc");
                        int pci = stoi(find_val(id_json, "pci"));
                        int tac = stoi(find_val(id_json, "tac"));
                        
                        int asu = stoi(find_val(sig_json, "asu"));
                        int rsrp = stoi(find_val(sig_json, "rsrp"));
                        int rsrq = stoi(find_val(sig_json, "rsrq"));
                        int rssnr = stoi(find_val(sig_json, "rssnr"));

                        W.exec_params(
                            "INSERT INTO telephony_lte (location_record_id, band, ci, earfcn, mcc, mnc, pci, tac, asu, rsrp, rsrq, rssnr) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
                            location_id, band, ci, earfcn, mcc, mnc, pci, tac, asu, rsrp, rsrq, rssnr
                        );
                    }
                }
            }
            
            W.commit();
        } catch (const exception &e) {
            cerr << "[DB] Save to SQL failed: " << e.what() << endl;
            g_data.db_connected = false;
        }
    }

    parse_json_to_data(raw_json);
}

void migrate_json_to_sql() {
    if (!check_db_alive()) {
        cerr << "[DB] Cannot migrate: DB unreachable" << endl;
        return;
    }

    try {
        pqxx::connection c(DB_CONN);
        pqxx::work W(c);
        W.exec("TRUNCATE location_records CASCADE;");

        ifstream log_file("telemetry_log.json");
        string line;
        int count = 0;
        while (getline(log_file, line)) {
            if (line.empty() || line.find('{') == string::npos) continue;

            double lat = stod(find_val(line, "latitude"));
            double lon = stod(find_val(line, "longitude"));
            double accuracy = stod(find_val(line, "accuracy"));
            string provider = find_val(line, "provider");
            string source = find_val(line, "source");
            
            pqxx::result res = W.exec_params(
                "INSERT INTO location_records (latitude, longitude, accuracy, provider, source, recorded_time, event_timestamp) "
                "VALUES ($1, $2, $3, $4, $5, NOW(), NOW()) RETURNING id",
                lat, lon, accuracy, provider, source
            );

            int location_id = res[0][0].as<int>();

            if (line.find("\"telephony\"") != string::npos) {
                size_t lte_start = line.find("\"LTE\"");
                if (lte_start != string::npos) {
                    size_t id_start = line.find("\"identity\"", lte_start);
                    size_t sig_start = line.find("\"signal\"", lte_start);
                    
                    if (id_start != string::npos && sig_start != string::npos) {
                        size_t id_end = line.find("}", id_start);
                        size_t sig_end = line.find("}", sig_start);
                        
                        string id_json = line.substr(id_start, id_end - id_start + 1);
                        string sig_json = line.substr(sig_start, sig_end - sig_start + 1);
                        
                        int band = stoi(find_val(id_json, "band"));
                        int64_t ci = stoll(find_val(id_json, "ci"));
                        int earfcn = stoi(find_val(id_json, "earfcn"));
                        string mcc = find_val(id_json, "mcc");
                        string mnc = find_val(id_json, "mnc");
                        int pci = stoi(find_val(id_json, "pci"));
                        int tac = stoi(find_val(id_json, "tac"));
                        
                        int asu = stoi(find_val(sig_json, "asu"));
                        int rsrp = stoi(find_val(sig_json, "rsrp"));
                        int rsrq = stoi(find_val(sig_json, "rsrq"));
                        int rssnr = stoi(find_val(sig_json, "rssnr"));

                        W.exec_params(
                            "INSERT INTO telephony_lte (location_record_id, band, ci, earfcn, mcc, mnc, pci, tac, asu, rsrp, rsrq, rssnr) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
                            location_id, band, ci, earfcn, mcc, mnc, pci, tac, asu, rsrp, rsrq, rssnr
                        );
                    }
                }
            }
            count++;
        }
        W.commit();
        cout << "[DB] Migration complete. Records: " << count << endl;

        g_data.db_connected = true;
        sync_all_data();

    } catch (const exception &e) {
        cerr << "[DB] Migration failed: " << e.what() << endl;
    }
}