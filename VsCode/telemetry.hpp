#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <map>
#include <future>

// ========== Heatmap Structures ==========
struct HeatmapConfig {
    std::string criterion = "RSRP";
    std::string earfcn = "";
    float searchRadiusMeters = 35.0f;
    float idwPower = 2.0f;
    float alpha = 0.85f;
    bool useAllPCIs = true;
    std::vector<int> selectedPCIs;
    int zoom = 15;
};

struct HeatmapStatus {
    std::atomic<bool> generating{false};
    std::atomic<int> progress{0};
    std::string message;
    std::mutex mtx;
};

extern HeatmapStatus g_heatmap_status;
extern const std::string DB_CONN;

// ========== Telemetry Data ==========
struct CellHistory {
    std::vector<double> x_time;
    std::vector<double> y_rsrp;
};

struct DbPoint {
    double latitude;
    double longitude;
    int rsrp;
};

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

    // Heatmap state
    bool heatmap_ready = false;
    std::string heatmap_earfcn = "";
    std::string heatmap_criterion = "";
    double heatmap_min_lat = 0, heatmap_max_lat = 0;
    double heatmap_min_lon = 0, heatmap_max_lon = 0;
    int heatmap_zoom = 15;

    void clear_all() {
        cell_logs.clear();
        history_lat.clear();
        history_lon.clear();
        history_time.clear();
        base_timestamp = 0;
        max_recorded_time = 0;
        raw = " ";
        heatmap_ready = false;
    }

    // Legacy поля
    float latitude = 0, longitude = 0;
    int dbm = 0;
    int rssi = 0;
    int rsrq = 0;
    std::string telInf = " ";
    std::vector<double> x_time;
    std::vector<double> y_dbm;
    std::chrono::steady_clock::time_point start_time;

    TelemetryData();
};

// ========== Globals ==========
extern TelemetryData g_data;
extern bool allow_receiving;
extern std::atomic<bool> should_run;

// ========== Functions ==========
int parse_dbm(const std::string& text);
int parse_rsrq(const std::string& text);
int parse_rssi(const std::string& text);
void save_to_json(const std::string& raw_msg, float lat, float lon, int dbm, int rsrq, int rssi);
void init_database();
void sync_all_data();
void save_packet(const std::string& raw_json);
void migrate_json_to_sql();
std::vector<DbPoint> LoadPointsFromDatabase();
std::string find_val(std::string json_text, std::string key);
void parse_json_to_data(std::string raw);
void backend();
void run_gui();

// ========== Heatmap Functions ==========
void generate_heatmap_async(const std::string& db_conn, const HeatmapConfig& config);
bool generate_heatmap_tiles(const std::string& db_conn, const HeatmapConfig& config);
bool is_heatmap_generating();
int get_heatmap_progress();
std::string get_heatmap_message();
std::vector<int> get_available_pcis(const std::string& db_conn, const std::string& earfcn);
void clear_heatmap_cache();