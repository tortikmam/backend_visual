#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <map>

// Структура истории ячейки
struct CellHistory {
    std::vector<double> x_time;
    std::vector<double> y_rsrp;
};

// Структура для точек из БД (для отображения на карте)
// Оставляем если хочешь зелёные точки на карте, удали если нет
struct DbPoint {
    double latitude;
    double longitude;
    int rsrp;
};

// Структура данных телеметрии
struct TelemetryData {
    // Новые поля (как у друга)
    float lat = 0, lon = 0, alt = 0, acc = 0;
    int rsrp = 0;
    std::string type = "";
    std::string raw = "";
    std::mutex mtx;

    bool db_connected = false;
    std::string data_source = "None";

    std::map<std::string, CellHistory> cell_logs;
    std::vector<double> history_lat;
    std::vector<double> history_lon;
    std::vector<double> history_time;

    double base_timestamp = 0;
    float view_min_time = 0;
    float view_max_time = 100;
    float max_recorded_time = 100;

    void clear_all() {
        cell_logs.clear();
        history_lat.clear();
        history_lon.clear();
        history_time.clear();
        base_timestamp = 0;
        max_recorded_time = 0;
        raw = "";
    }

    // Legacy поля (для совместимости)
    float latitude = 0, longitude = 0;
    int dbm = 0;
    int rssi = 0;
    int rsrq = 0;
    std::string telInf = "";
    std::vector<double> x_time;
    std::vector<double> y_dbm;
    std::chrono::steady_clock::time_point start_time;

    TelemetryData();
};

// Глобальные переменные
extern TelemetryData g_data;
extern bool allow_receiving;
extern std::atomic<bool> should_run;

// Функции парсинга (legacy)
int parse_dbm(const std::string& text);
int parse_rsrq(const std::string& text);
int parse_rssi(const std::string& text);

// Функция сохранения в JSON (legacy)
void save_to_json(const std::string& raw_msg, float lat, float lon, int dbm, int rsrq, int rssi);

// ========== DATABASE ==========
void init_database();
void sync_all_data();
void save_packet(const std::string& raw_json);
void migrate_json_to_sql();
std::vector<DbPoint> LoadPointsFromDatabase();

// ========== PARSER ==========
std::string find_val(std::string json_text, std::string key);
void parse_json_to_data(std::string raw);

// Основные функции потоков
void backend();
void run_gui();