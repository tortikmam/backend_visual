#include "telemetry.hpp"
#include <GL/glew.h>
#include <imgui.h>
#include <implot.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <cmath>

using json = nlohmann::json;

struct SignalPoint {
    double lat;
    double lon;
    int rsrp;
    double timestamp;
};

std::vector<SignalPoint> heatmap_points;
std::mutex heatmap_mtx;

// Градиент: -50 (красный, хорошо) -> -140 (голубой, плохо)
ImVec4 GetColorForRsrp(int rsrp) {
    float t = (float)(-50 - rsrp) / 90.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float r = 1.0f - t * 0.2f;
    float g = t * 0.3f;
    float b = 0.2f + t * 0.8f;

    return ImVec4(r, g, b, 0.85f);
}

void LoadHeatmapFromLog() {
    std::lock_guard<std::mutex> lock(heatmap_mtx);
    heatmap_points.clear();

    std::ifstream file("../VsCode/telemetry_log.json");
    if (!file.is_open()) {
        printf("[HEATMAP] Failed to open telemetry_log.json\n");
        return;
    }

    std::string line;
    int loaded = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);

            SignalPoint pt;
            pt.lat = j.value("latitude", 0.0);
            pt.lon = j.value("longitude", 0.0);
            pt.timestamp = j.value("timestamp", 0.0);

            if (j.contains("telephony") && j["telephony"].contains("LTE") && 
                j["telephony"]["LTE"].contains("signal") && 
                j["telephony"]["LTE"]["signal"].contains("rsrp")) {
                pt.rsrp = j["telephony"]["LTE"]["signal"]["rsrp"].get<int>();
            } else {
                pt.rsrp = -100;
            }

            if (pt.lat != 0 && pt.lon != 0) {
                heatmap_points.push_back(pt);
                loaded++;
            }
        } catch (...) {
            continue;
        }
    }
    printf("[HEATMAP] Loaded %d points from log\n", loaded);
}

void AddHeatmapPoint(double lat, double lon, int rsrp, double timestamp) {
    std::lock_guard<std::mutex> lock(heatmap_mtx);
    heatmap_points.push_back({lat, lon, rsrp, timestamp});
    printf("[HEATMAP] Added point: lat=%.6f lon=%.6f rsrp=%d (total: %zu)\n", 
           lat, lon, rsrp, heatmap_points.size());
}

void DrawHeatmapOverlay() {
    std::lock_guard<std::mutex> lock(heatmap_mtx);

    if (heatmap_points.empty()) {
        ImPlot::PlotText("[HEATMAP] No data points", 0, 0);
        return;
    }

    // Отрисовываем точки группами по цвету для оптимизации
    // Сначала собираем данные в массивы
    static std::vector<double> xs;
    static std::vector<double> ys;
    xs.clear();
    ys.clear();

    for (const auto& pt : heatmap_points) {
        xs.push_back(pt.lon);
        ys.push_back(pt.lat);
    }

    // Рисуем все точки одним вызовом с дефолтным цветом (жёлтый для видимости)
    ImPlot::SetNextMarkerStyle(ImPlotMarker_Square, 8.0f, ImVec4(1, 1, 0, 0.8f), 1.0f, ImVec4(0,0,0,1));
    ImPlot::PlotScatter("Signal Points", xs.data(), ys.data(), (int)xs.size());

    // Рисуем цветные точки поверх (по одной для разных цветов)
    for (const auto& pt : heatmap_points) {
        ImVec4 color = GetColorForRsrp(pt.rsrp);
        double x = pt.lon;
        double y = pt.lat;
        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 6.0f, color, 0.0f);
        ImPlot::PlotScatter("##heat", &x, &y, 1);
    }
}

void DrawHeatmapLegend() {
    ImGui::SetNextWindowSize(ImVec2(280, 140), ImGuiCond_FirstUseEver);
    ImGui::Begin("Signal Heatmap Legend", nullptr, ImGuiWindowFlags_NoCollapse);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = 250.0f;
    float height = 20.0f;

    for (int i = 0; i < (int)width; ++i) {
        float t = (float)i / width;
        int rsrp = (int)(-50 - t * 90.0f);
        ImVec4 col = GetColorForRsrp(rsrp);
        draw_list->AddRectFilled(
            ImVec2(pos.x + i, pos.y), 
            ImVec2(pos.x + i + 1, pos.y + height),
            ImGui::ColorConvertFloat4ToU32(col)
        );
    }

    ImGui::Dummy(ImVec2(width, height));
    ImGui::Text("-50 dBm (excellent)");
    ImGui::SameLine();
    ImGui::SetCursorPosX(pos.x + width - 90);
    ImGui::Text("-140 dBm (poor)");

    ImGui::Separator();
    {
        std::lock_guard<std::mutex> lock(heatmap_mtx);
        ImGui::Text("Total points: %zu", heatmap_points.size());
    }

    ImGui::End();
}