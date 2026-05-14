#include "telemetry.hpp"
#include <GL/glew.h>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <cmath>
#include <filesystem>
#include <thread>
#include <queue>
#include <condition_variable>
#include <algorithm>
#include <set>
#include <cstring>
#include <atomic>

// stb_image_write для сохранения тайлов
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ==================== СТРУКТУРЫ ====================

struct Measurement {
    double lat;
    double lon;
    int rsrp;
    int rsrq;
    int rssi;
    double altitude;
    int earfcn;
    double timestamp;
};

// Сетка IDW
typedef std::vector<std::vector<double>> Grid2D;

struct HeatmapResult {
    Grid2D grid;
    double lat_min, lat_max;
    double lon_min, lon_max;
    int earfcn;
    bool ready = false;
};

// ==================== ГЛОБАЛЬНЫЕ ДАННЫЕ ====================

static std::vector<Measurement> g_measurements;
static std::mutex g_measurements_mtx;

static HeatmapCriterion g_criterion = HeatmapCriterion::RSRP;
static double g_radius_meters = 25.0;
static int g_selected_earfcn = -1; // -1 = все
static std::mutex g_params_mtx;

static std::map<int, HeatmapResult> g_results;
static std::mutex g_results_mtx;

static std::atomic<bool> g_worker_running{false};
static std::atomic<bool> g_update_requested{false};
static std::atomic<bool> g_computing{false};
static std::atomic<int> g_progress{0};
static std::atomic<int> g_total{0};
static std::thread g_worker_thread;
static std::condition_variable g_cv;
static std::mutex g_cv_mtx;

// ==================== ГЕОДЕЗИЯ ====================

double HaversineDistance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = std::sin(dlat/2) * std::sin(dlat/2) +
               std::cos(lat1 * M_PI / 180.0) * std::cos(lat2 * M_PI / 180.0) *
               std::sin(dlon/2) * std::sin(dlon/2);
    double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return R * c;
}

// ==================== IDW ====================

double GetCriterionValue(const Measurement& m, HeatmapCriterion c) {
    switch (c) {
        case HeatmapCriterion::RSRP: return (double)m.rsrp;
        case HeatmapCriterion::RSRQ: return (double)m.rsrq;
        case HeatmapCriterion::RSSI: return (double)m.rssi;
        case HeatmapCriterion::ALTITUDE: return m.altitude;
    }
    return 0.0;
}

double IDWInterpolate(double lat, double lon, 
                      const std::vector<Measurement>& points,
                      HeatmapCriterion criterion,
                      double radius_meters) {
    double numerator = 0.0;
    double denominator = 0.0;
    int count = 0;

    for (const auto& pt : points) {
        double dist = HaversineDistance(lat, lon, pt.lat, pt.lon);
        if (dist > radius_meters || dist < 0.1) continue;

        double w = 1.0 / (dist * dist);
        double val = GetCriterionValue(pt, criterion);

        numerator += w * val;
        denominator += w;
        count++;
    }

    if (count == 0 || denominator == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return numerator / denominator;
}

// ==================== ЗАГРУЗКА ДАННЫХ ====================

void LoadMeasurementsFromLog() {
    std::lock_guard<std::mutex> lock(g_measurements_mtx);
    g_measurements.clear();

    std::vector<std::string> paths = {
        "telemetry_log.json", "../telemetry_log.json", "../../telemetry_log.json",
        "VsCode/telemetry_log.json", "../VsCode/telemetry_log.json", "../../VsCode/telemetry_log.json"
    };

    std::string log_path;
    for (const auto& p : paths) {
        if (fs::exists(p)) { log_path = p; break; }
    }
    if (log_path.empty()) { printf("[IDW] telemetry_log.json not found!\n"); return; }

    std::ifstream file("../VsCode/telemetry_log.json");
    if (!file.is_open()) return;

    std::string line;
    int loaded = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);
            Measurement m;
            m.lat = j.value("latitude", 0.0);
            m.lon = j.value("longitude", 0.0);
            m.altitude = j.value("accuracy", 0.0);
            m.timestamp = j.value("timestamp", 0.0);

            // Правильный парсинг телефонии
            if (j.contains("telephony") && j["telephony"].is_object()) {
                auto telephony = j["telephony"];

                // Ищем любую сеть (LTE, NR, GSM и т.д.)
                for (auto& [net_type, net_data] : telephony.items()) {
                    if (net_data.is_object()) {
                        if (net_data.contains("identity") && net_data["identity"].is_object()) {
                            auto identity = net_data["identity"];
                            m.earfcn = identity.value("earfcn", 0);
                            // Если earfcn не задан, пробуем другие поля
                            if (m.earfcn == 0) {
                                m.earfcn = identity.value("band", 0);
                            }
                        }
                        if (net_data.contains("signal") && net_data["signal"].is_object()) {
                            auto sig = net_data["signal"];
                            m.rsrp = sig.value("rsrp", -100);
                            m.rsrq = sig.value("rsrq", -10);
                            m.rssi = sig.value("rssnr", 0);
                        }
                        break; // Берём первую найденную сеть
                    }
                }
            }

            if (m.lat != 0 && m.lon != 0) {
                g_measurements.push_back(m);
                loaded++;
            }
        } catch (...) { continue; }
    }
    printf("[IDW] Loaded %d measurements\n", loaded);
}

// ==================== СОХРАНЕНИЕ ====================

void SaveHeatmapImage(const HeatmapResult& result, HeatmapCriterion criterion, int earfcn) {
    if (!result.ready || result.grid.empty()) return;

    int ny = (int)result.grid.size();
    int nx = (int)result.grid[0].size();

    std::vector<unsigned char> pixels(nx * ny * 4);

    for (int iy = 0; iy < ny; iy++) {
        for (int ix = 0; ix < nx; ix++) {
            double val = result.grid[ny - 1 - iy][ix];
            ImVec4 col;

            if (std::isnan(val)) {
                col = ImVec4(0, 0, 0, 0);
            } else {
                double t = 0.0;
                switch (criterion) {
                    case HeatmapCriterion::RSRP: t = (-50.0 - val) / 90.0; break;
                    case HeatmapCriterion::RSRQ: t = (-3.0 - val) / 17.0; break;
                    case HeatmapCriterion::RSSI: t = (-30.0 - val) / 70.0; break;
                    case HeatmapCriterion::ALTITUDE: t = std::clamp((val - 0.0) / 50.0, 0.0, 1.0); break;
                }
                t = std::clamp(t, 0.0, 1.0);
                col = ImVec4(1.0f - (float)t * 0.3f, (float)t * 0.2f, 0.2f + (float)t * 0.8f, 0.85f);
            }

            int idx = (iy * nx + ix) * 4;
            pixels[idx + 0] = (unsigned char)(col.x * 255);
            pixels[idx + 1] = (unsigned char)(col.y * 255);
            pixels[idx + 2] = (unsigned char)(col.z * 255);
            pixels[idx + 3] = (unsigned char)(col.w * 255);
        }
    }

    std::string out_dir = "build/heatmaps";
    fs::create_directories(out_dir);
    std::string fname = out_dir + "/earfcn_" + std::to_string(earfcn) + "_" + 
                      std::to_string((int)criterion) + ".png";
    stbi_write_png(fname.c_str(), nx, ny, 4, pixels.data(), nx * 4);
    printf("[IDW] Saved: %s (%dx%d)\n", fname.c_str(), nx, ny);
}

// ==================== РАБОЧИЙ ПОТОК ====================

void ComputeHeatmapForEarfcn(int earfcn, 
                             const std::vector<Measurement>& all_points,
                             HeatmapCriterion criterion,
                             double radius) {
    std::vector<Measurement> points;
    for (const auto& m : all_points) {
        if (earfcn == -1 || m.earfcn == earfcn) {
            points.push_back(m);
        }
    }

    if (points.empty()) {
        printf("[IDW] No points for EARFCN %d\n", earfcn);
        return;
    }

    // Границы
    double lat_min = 90.0, lat_max = -90.0;
    double lon_min = 180.0, lon_max = -180.0;
    for (const auto& p : points) {
        lat_min = std::min(lat_min, p.lat);
        lat_max = std::max(lat_max, p.lat);
        lon_min = std::min(lon_min, p.lon);
        lon_max = std::max(lon_max, p.lon);
    }

    // Если все точки в одном месте - добавляем отступ
    if (lat_max - lat_min < 0.0001) { lat_min -= 0.001; lat_max += 0.001; }
    if (lon_max - lon_min < 0.0001) { lon_min -= 0.001; lon_max += 0.001; }

    double dlat = (lat_max - lat_min) * 0.05;
    double dlon = (lon_max - lon_min) * 0.05;
    lat_min -= dlat; lat_max += dlat;
    lon_min -= dlon; lon_max += dlon;

    // Сетка: шаг ~10 метров, макс 200x200
    double step_deg = 10.0 / 111000.0;
    int nx = (int)((lon_max - lon_min) / step_deg) + 1;
    int ny = (int)((lat_max - lat_min) / step_deg) + 1;

    if (nx > 200) nx = 200;
    if (ny > 200) ny = 200;
    if (nx < 20) nx = 20;
    if (ny < 20) ny = 20;

    printf("[IDW] EARFCN %d: grid %dx%d, %zu points, radius %.1fm\n", 
           earfcn, nx, ny, points.size(), radius);

    Grid2D grid(ny, std::vector<double>(nx));
    g_total = ny;

    for (int iy = 0; iy < ny && g_worker_running; iy++) {
        for (int ix = 0; ix < nx && g_worker_running; ix++) {
            double lat = lat_min + iy * (lat_max - lat_min) / (ny - 1);
            double lon = lon_min + ix * (lon_max - lon_min) / (nx - 1);
            grid[iy][ix] = IDWInterpolate(lat, lon, points, criterion, radius);
        }
        g_progress = iy;
        if (iy % 20 == 0) printf("[IDW] EARFCN %d: %d/%d\n", earfcn, iy, ny);
    }

    if (!g_worker_running) return;

    HeatmapResult result;
    result.grid = std::move(grid);
    result.lat_min = lat_min;
    result.lat_max = lat_max;
    result.lon_min = lon_min;
    result.lon_max = lon_max;
    result.earfcn = earfcn;
    result.ready = true;

    {
        std::lock_guard<std::mutex> lock(g_results_mtx);
        g_results[earfcn] = std::move(result);
    }

    SaveHeatmapImage(g_results[earfcn], criterion, earfcn);
    printf("[IDW] EARFCN %d complete!\n", earfcn);
}

void HeatmapWorker() {
    printf("[IDW] Worker started\n");

    while (g_worker_running) {
        std::unique_lock<std::mutex> lock(g_cv_mtx);
        g_cv.wait(lock, [] { return g_update_requested.load() || !g_worker_running.load(); });
        if (!g_worker_running) break;
        g_update_requested = false;
        lock.unlock();

        g_computing = true;
        g_progress = 0;
        printf("[IDW] Starting computation...\n");

        std::vector<Measurement> points;
        HeatmapCriterion criterion;
        double radius;
        int selected_earfcn;

        {
            std::lock_guard<std::mutex> lock(g_measurements_mtx);
            points = g_measurements;
            std::lock_guard<std::mutex> plock(g_params_mtx);
            criterion = g_criterion;
            radius = g_radius_meters;
            selected_earfcn = g_selected_earfcn;
        }

        if (points.empty()) { 
            g_computing = false;
            printf("[IDW] No data\n"); 
            continue; 
        }

        // Находим уникальные EARFCN
        std::set<int> earfcns;
        for (const auto& p : points) earfcns.insert(p.earfcn);

        if (selected_earfcn != -1) {
            ComputeHeatmapForEarfcn(selected_earfcn, points, criterion, radius);
        } else {
            for (int earfcn : earfcns) {
                if (!g_worker_running) break;
                ComputeHeatmapForEarfcn(earfcn, points, criterion, radius);
            }
        }

        g_computing = false;
        printf("[IDW] All computations complete!\n");
    }

    g_computing = false;
    printf("[IDW] Worker stopped\n");
}

// ==================== ПУБЛИЧНЫЙ API ====================

void StartHeatmapWorker() {
    if (g_worker_running) return;
    g_worker_running = true;
    LoadMeasurementsFromLog();
    g_worker_thread = std::thread(HeatmapWorker);
}

void StopHeatmapWorker() {
    g_worker_running = false;
    g_update_requested = true;
    g_cv.notify_all();
    if (g_worker_thread.joinable()) g_worker_thread.join();
}

void RequestHeatmapUpdate() {
    g_update_requested = true;
    g_cv.notify_one();
}

bool IsHeatmapReady() {
    std::lock_guard<std::mutex> lock(g_results_mtx);
    for (const auto& [k, v] : g_results) {
        if (v.ready) return true;
    }
    return false;
}

bool IsHeatmapComputing() {
    return g_computing.load();
}

int GetHeatmapProgress() {
    return g_progress.load();
}

int GetHeatmapTotal() {
    return g_total.load();
}

void SetHeatmapCriterion(HeatmapCriterion c) {
    { std::lock_guard<std::mutex> lock(g_params_mtx); g_criterion = c; }
    RequestHeatmapUpdate();
}

HeatmapCriterion GetHeatmapCriterion() {
    std::lock_guard<std::mutex> lock(g_params_mtx);
    return g_criterion;
}

void SetHeatmapRadius(double meters) {
    { std::lock_guard<std::mutex> lock(g_params_mtx); g_radius_meters = std::clamp(meters, 10.0, 40.0); }
    RequestHeatmapUpdate();
}

double GetHeatmapRadius() {
    std::lock_guard<std::mutex> lock(g_params_mtx);
    return g_radius_meters;
}

void SetSelectedEarfcn(int earfcn) {
    { std::lock_guard<std::mutex> lock(g_params_mtx); g_selected_earfcn = earfcn; }
    RequestHeatmapUpdate();
}

int GetSelectedEarfcn() {
    std::lock_guard<std::mutex> lock(g_params_mtx);
    return g_selected_earfcn;
}

std::vector<int> GetAvailableEarfcns() {
    std::lock_guard<std::mutex> lock(g_measurements_mtx);
    std::set<int> s;
    for (const auto& m : g_measurements) s.insert(m.earfcn);
    return std::vector<int>(s.begin(), s.end());
}

// ==================== ОТРИСОВКА ====================

ImVec4 GetIDWColor(double val, HeatmapCriterion c) {
    if (std::isnan(val)) return ImVec4(0, 0, 0, 0);

    double t = 0.0;
    switch (c) {
        case HeatmapCriterion::RSRP: t = (-50.0 - val) / 90.0; break;
        case HeatmapCriterion::RSRQ: t = (-3.0 - val) / 17.0; break;
        case HeatmapCriterion::RSSI: t = (-30.0 - val) / 70.0; break;
        case HeatmapCriterion::ALTITUDE: t = std::clamp((val - 0.0) / 50.0, 0.0, 1.0); break;
    }
    t = std::clamp(t, 0.0, 1.0);
    return ImVec4(1.0f - (float)t * 0.3f, (float)t * 0.2f, 0.2f + (float)t * 0.8f, 0.85f);
}

void DrawIDWHeatmap() {
    std::lock_guard<std::mutex> lock(g_results_mtx);

    int earfcn = GetSelectedEarfcn();

    if (earfcn == -1) {
        // Рисуем все доступные
        for (const auto& [e, result] : g_results) {
            if (!result.ready || result.grid.empty()) continue;

            int ny = (int)result.grid.size();
            if (ny == 0) continue;
            int nx = (int)result.grid[0].size();
            if (nx == 0) continue;

            static GLuint tex_id = 0;
            if (tex_id == 0) glGenTextures(1, &tex_id);

            std::vector<unsigned char> pixels(nx * ny * 4);
            for (int iy = 0; iy < ny; iy++) {
                for (int ix = 0; ix < nx; ix++) {
                    double val = result.grid[ny - 1 - iy][ix];
                    ImVec4 col = GetIDWColor(val, GetHeatmapCriterion());
                    int idx = (iy * nx + ix) * 4;
                    pixels[idx + 0] = (unsigned char)(col.x * 255);
                    pixels[idx + 1] = (unsigned char)(col.y * 255);
                    pixels[idx + 2] = (unsigned char)(col.z * 255);
                    pixels[idx + 3] = (unsigned char)(col.w * 255);
                }
            }

            glBindTexture(GL_TEXTURE_2D, tex_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nx, ny, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            std::string label = "IDW EARFCN " + std::to_string(e);
            ImPlot::PlotImage(label.c_str(), (void*)(intptr_t)tex_id,
                              ImPlotPoint(result.lon_min, result.lat_min),
                              ImPlotPoint(result.lon_max, result.lat_max));
        }
    } else {
        auto it = g_results.find(earfcn);
        if (it == g_results.end() || !it->second.ready) {
            ImPlot::PlotText("[IDW] Computing...", 0, 0);
            return;
        }

        const auto& result = it->second;
        int ny = (int)result.grid.size();
        if (ny == 0) return;
        int nx = (int)result.grid[0].size();
        if (nx == 0) return;

        static GLuint tex_id = 0;
        if (tex_id == 0) glGenTextures(1, &tex_id);

        std::vector<unsigned char> pixels(nx * ny * 4);
        for (int iy = 0; iy < ny; iy++) {
            for (int ix = 0; ix < nx; ix++) {
                double val = result.grid[ny - 1 - iy][ix];
                ImVec4 col = GetIDWColor(val, GetHeatmapCriterion());
                int idx = (iy * nx + ix) * 4;
                pixels[idx + 0] = (unsigned char)(col.x * 255);
                pixels[idx + 1] = (unsigned char)(col.y * 255);
                pixels[idx + 2] = (unsigned char)(col.z * 255);
                pixels[idx + 3] = (unsigned char)(col.w * 255);
            }
        }

        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nx, ny, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        ImPlot::PlotImage("IDW Heatmap", (void*)(intptr_t)tex_id,
                          ImPlotPoint(result.lon_min, result.lat_min),
                          ImPlotPoint(result.lon_max, result.lat_max));
    }
}

void DrawIDWHeatmapLegend() {
    ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("IDW Heatmap Legend", nullptr, ImGuiWindowFlags_NoCollapse);

    // Прогресс
    if (IsHeatmapComputing()) {
        int prog = GetHeatmapProgress();
        int tot = GetHeatmapTotal();
        float frac = tot > 0 ? (float)prog / tot : 0.0f;
        ImGui::Text("Computing... %d/%d", prog, tot);
        ImGui::ProgressBar(frac, ImVec2(-1, 0));
        ImGui::Separator();
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = 270.0f;
    float height = 20.0f;

    for (int i = 0; i < (int)width; ++i) {
        float t = (float)i / width;
        int rsrp = (int)(-50 - t * 90.0f);
        ImVec4 col = GetIDWColor((double)rsrp, GetHeatmapCriterion());
        draw_list->AddRectFilled(
            ImVec2(pos.x + i, pos.y), 
            ImVec2(pos.x + i + 1, pos.y + height),
            ImGui::ColorConvertFloat4ToU32(col)
        );
    }

    ImGui::Dummy(ImVec2(width, height));

    auto crit = GetHeatmapCriterion();
    if (crit == HeatmapCriterion::RSRP) {
        ImGui::Text("-50 dBm (excellent)");
        ImGui::SameLine(); ImGui::SetCursorPosX(pos.x + width - 90);
        ImGui::Text("-140 dBm (poor)");
    } else if (crit == HeatmapCriterion::RSRQ) {
        ImGui::Text("-3 dB (excellent)");
        ImGui::SameLine(); ImGui::SetCursorPosX(pos.x + width - 90);
        ImGui::Text("-20 dB (poor)");
    } else if (crit == HeatmapCriterion::RSSI) {
        ImGui::Text("-30 dBm (excellent)");
        ImGui::SameLine(); ImGui::SetCursorPosX(pos.x + width - 90);
        ImGui::Text("-100 dBm (poor)");
    } else {
        ImGui::Text("0 m (low)");
        ImGui::SameLine(); ImGui::SetCursorPosX(pos.x + width - 90);
        ImGui::Text("50 m (high)");
    }

    ImGui::Separator();
    ImGui::Text("Radius: %.1f m", GetHeatmapRadius());

    auto earfcns = GetAvailableEarfcns();
    ImGui::Text("EARFCNs: %zu", earfcns.size());
    for (int e : earfcns) {
        ImGui::SameLine();
        ImGui::Text("%d", e);
    }

    ImGui::End();
}