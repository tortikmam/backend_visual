#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <map>

// Критерии для тепловой карты
enum class HeatmapCriterion {
    RSRP,
    RSRQ,
    RSSI,
    ALTITUDE
};

// Структура данных телеметрии
struct TelemetryData {
    float latitude = 0, longitude = 0;
    int dbm = 0;
    int rssi = 0;
    int rsrq = 0;
    std::string telInf = "";
    std::string raw = "";
    std::mutex mtx;
    std::vector<double> x_time;
    std::vector<double> y_dbm;
    std::chrono::steady_clock::time_point start_time;

    TelemetryData();
};

// Глобальные переменные
extern TelemetryData g_data;
extern bool allow_receiving;
extern std::atomic<bool> should_run;

// Функции парсинга
int parse_dbm(const std::string& text);
int parse_rsrq(const std::string& text);
int parse_rssi(const std::string& text);

// Функция сохранения в JSON
void save_to_json(const std::string& raw_msg, float lat, float lon, int dbm, int rsrq, int rssi);

// Legacy тепловая карта (точки)
void LoadHeatmapFromLog();
void AddHeatmapPoint(double lat, double lon, int rsrp, double timestamp);
void DrawHeatmapOverlay();
void DrawHeatmapLegend();

// ========== IDW ТЕПЛОВАЯ КАРТА ==========

// Запуск/остановка рабочего потока
void StartHeatmapWorker();
void StopHeatmapWorker();

// Запросить пересчет
void RequestHeatmapUpdate();
bool IsHeatmapReady();

// Настройки
void SetHeatmapCriterion(HeatmapCriterion c);
HeatmapCriterion GetHeatmapCriterion();
void SetHeatmapRadius(double meters);
double GetHeatmapRadius();
void SetSelectedEarfcn(int earfcn);
int GetSelectedEarfcn();
std::vector<int> GetAvailableEarfcns();

// Отрисовка
void DrawIDWHeatmap();
void DrawIDWHeatmapLegend();

// Основные функции потоков
void backend();
void run_gui();