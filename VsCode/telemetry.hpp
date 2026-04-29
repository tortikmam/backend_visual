#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>

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

// Основные функции потоков
void backend();
void run_gui();