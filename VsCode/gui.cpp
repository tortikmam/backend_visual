#include "telemetry.hpp"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <imgui.h>
#include <implot.h>
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <cmath>
#include <map>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <cpr/cpr.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
using namespace std;

static std::map<std::string, GLuint> tile_cache;

double lon2tile(double lon, int zoom) { return (lon + 180.0) / 360.0 * std::pow(2.0, zoom); }
double lat2tile(double lat, int zoom) { return (1.0 - std::log(std::tan(lat * M_PI / 180.0) + 1.0 / std::cos(lat * M_PI / 180.0)) / M_PI) / 2.0 * std::pow(2.0, zoom); }
double tilex2lon(int x, int z) { return x / std::pow(2.0, z) * 360.0 - 180.0; }
double tiley2lat(int y, int z) { double n = M_PI - 2.0 * M_PI * y / std::pow(2.0, z); return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n))); }

void download_tile_cpr(int z, int x, int y) {
    int max_tile = std::pow(2, z) - 1;
    if (x < 0 || x > max_tile || y < 0 || y > max_tile) return;
    std::string folder = "tiles/" + std::to_string(z) + "/" + std::to_string(x);
    std::string path = folder + "/" + std::to_string(y) + ".png";
    if (std::filesystem::exists(path)) return;
    std::filesystem::create_directories(folder);
    cpr::Response r = cpr::Get(cpr::Url{"https://tile.openstreetmap.org/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png"}, cpr::Header{{"User-Agent", "TelemetryApp/1.0"}}, cpr::VerifySsl{false});
    if (r.status_code == 200) { std::ofstream ofs(path, std::ios::binary); ofs << r.text; ofs.close(); }
}
void thread_loader(int z, int x, int y) { download_tile_cpr(z, x, y); }

GLuint LoadTexture(const char* filename) {
    int width, height, channels; stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) return 0;
    GLuint textureID; glGenTextures(1, &textureID); glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data); return textureID;
}

double calculate_distance(double lat1, double lon1, double lat2, double lon2) {
    double dy = (lat2 - lat1) * 111139.0;
    double dx = (lon2 - lon1) * 111139.0 * cos(lat1 * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

void run_gui() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_Window* window = SDL_CreateWindow("GPS + CellTower Monitor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) cerr << "GLEW Error" << endl;

    ImGui::CreateContext(); ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    bool running = true, show_map = true, show_legacy_points = false, show_db_points = true;
    // Загружаем точки из БД один раз при старте
    std::vector<DbPoint> db_points = LoadPointsFromDatabase();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) { running = false; should_run = false; }
        }
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

        // ====== SMARTPHONE DATA ======
        ImGui::Begin("Smartphone Data");
        {
            lock_guard<mutex> lock(g_data.mtx);
            if (g_data.db_connected) ImGui::TextColored(ImVec4(0, 1, 0, 1), "● DATABASE: ONLINE");
            else { ImGui::TextColored(ImVec4(1, 0, 0, 1), "○ DATABASE: OFFLINE"); if (ImGui::SmallButton("Retry Connect")) thread([]() { init_database(); }).detach(); }
            ImGui::Text("Source: %s", g_data.data_source.c_str()); ImGui::Separator();
            ImGui::Text("Latitude:  %.6f", g_data.lat); ImGui::Text("Longitude: %.6f", g_data.lon);
            ImGui::Text("Net Type:  %s", g_data.type.c_str()); ImGui::Text("RSRP:      %d dBm", g_data.rsrp); ImGui::Separator();
            ImGui::Text("Time Range Filter (seconds from start):");
            ImGui::SliderFloat("Start Time", &g_data.view_min_time, 0.0f, g_data.max_recorded_time);
            ImGui::SliderFloat("End Time",   &g_data.view_max_time, 0.0f, g_data.max_recorded_time);
            if (g_data.view_min_time > g_data.view_max_time) g_data.view_min_time = g_data.view_max_time;
            ImGui::Separator();
            if (ImGui::Button("Migrate JSON -> PostgreSQL", ImVec2(-1, 40))) thread(migrate_json_to_sql).detach();
            ImGui::Separator();
            if (ImGui::TreeNode("Raw JSON")) { ImGui::TextWrapped("%s", g_data.raw.c_str()); ImGui::TreePop(); }
        }
        ImGui::End();

        // ====== MAP WINDOW ======
        if (show_map) {
            ImGui::Begin("Live Map View", nullptr, ImGuiWindowFlags_NoCollapse);
            if (ImPlot::BeginPlot("##MainMap", ImVec2(-1, -1), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Longitude", "Latitude");
                ImPlot::SetupAxesLimits(82.8, 83.1, 54.9, 55.1, ImGuiCond_FirstUseEver);
                ImPlotRect limits = ImPlot::GetPlotLimits();

                int zoom = 15; double width = std::abs(limits.X.Max - limits.X.Min);
                if (width > 0) { zoom = (int)std::floor(std::log2(360.0 / width * 2)); if (zoom < 1) zoom = 1; if (zoom > 18) zoom = 18; }

                if (tile_cache.size() > 500) {
                    for (auto const& [path, texID] : tile_cache) if (texID > 0) glDeleteTextures(1, (GLuint*)&texID);
                    tile_cache.clear();
                }

                if (limits.X.Min > -180 && limits.X.Max < 180 && limits.Y.Min > -90 && limits.Y.Max < 90) {
                    int x_start = (int)std::floor(lon2tile(limits.X.Min, zoom));
                    int x_end   = (int)std::floor(lon2tile(limits.X.Max, zoom));
                    int y_start = (int)std::floor(lat2tile(limits.Y.Max, zoom));
                    int y_end   = (int)std::floor(lat2tile(limits.Y.Min, zoom));
                    if (std::abs(x_end - x_start) < 20 && std::abs(y_end - y_start) < 20) {
                        for (int x = x_start; x <= x_end; ++x) {
                            for (int y = y_start; y <= y_end; ++y) {
                                double l_lon = tilex2lon(x, zoom), r_lon = tilex2lon(x + 1, zoom);
                                double b_lat = tiley2lat(y + 1, zoom), t_lat = tiley2lat(y, zoom);
                                std::string path = "tiles/" + std::to_string(zoom) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
                                GLuint texID = 0;
                                if (tile_cache.count(path)) { texID = tile_cache[path]; if (texID == 0 && std::filesystem::exists(path) && std::filesystem::file_size(path) > 0) { texID = LoadTexture(path.c_str()); tile_cache[path] = texID; } }
                                else {
                                    if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 0) { texID = LoadTexture(path.c_str()); tile_cache[path] = texID; }
                                    else { tile_cache[path] = 0; std::thread(thread_loader, zoom, x, y).detach(); }
                                }
                                if (texID > 0) ImPlot::PlotImage(path.c_str(), (void*)(intptr_t)texID, ImPlotPoint(l_lon, b_lat), ImPlotPoint(r_lon, t_lat));
                            }
                        }
                    }
                }

                // ====== ТРЕК И ТОЧКИ ======
                {
                    lock_guard<mutex> lock(g_data.mtx);
                    static std::vector<double> fx, fy, ft; fx.clear(); fy.clear(); ft.clear();
                    for (size_t i = 0; i < g_data.history_time.size(); ++i) {
                        if (g_data.history_time[i] >= g_data.view_min_time && g_data.history_time[i] <= g_data.view_max_time) {
                            fx.push_back(g_data.history_lon[i]); fy.push_back(g_data.history_lat[i]); ft.push_back(g_data.history_time[i]);
                        }
                    }
                    if (!fx.empty()) {
                        ImDrawList* dl = ImPlot::GetPlotDrawList();
                        if (fx.size() > 1) {
                            for (size_t i = 0; i < fx.size() - 1; ++i) {
                                double dt = ft[i+1] - ft[i];
                                if (dt > 0 && dt <= 25.0) {
                                    double dist = calculate_distance(fy[i], fx[i], fy[i+1], fx[i+1]);
                                    if (dist / dt < 160.0) dl->AddLine(ImPlot::PlotToPixels(ImPlotPoint(fx[i], fy[i])), ImPlot::PlotToPixels(ImPlotPoint(fx[i+1], fy[i+1])), IM_COL32(50,100,255,200), 3.0f);
                                }
                            }
                        }
                        ImVec2 last = ImPlot::PlotToPixels(ImPlotPoint(fx.back(), fy.back()));
                        dl->AddCircleFilled(last, 6.0f, IM_COL32(255,255,0,255)); dl->AddCircle(last, 6.5f, IM_COL32(0,0,0,255), 0, 1.5f);
                    }
                }

                // ====== ТОЧКИ ИЗ БД ======
                if (show_db_points && !db_points.empty()) {
                    ImDrawList* dl = ImPlot::GetPlotDrawList();
                    for (const auto& pt : db_points) {
                        ImVec2 px = ImPlot::PlotToPixels(ImPlotPoint(pt.longitude, pt.latitude));
                        dl->AddCircleFilled(px, 4.0f, IM_COL32(0,255,0,180)); dl->AddCircle(px, 4.5f, IM_COL32(0,100,0,255), 0, 1.0f);
                    }
                }
                ImPlot::EndPlot();
            }
            ImGui::End();
        }

        // ====== RSRP HISTORY ======
        ImGui::Begin("Signal Level (RSRP) History");
        if (ImPlot::BeginPlot("RSRP over Time", ImVec2(-1, -1))) {
            ImPlot::SetupAxisLimits(ImAxis_X1, g_data.view_min_time, g_data.view_max_time, ImPlotCond_Always);
            ImPlot::SetupAxes("Time (sec)", "RSRP (dBm)"); ImPlot::SetupAxisLimits(ImAxis_Y1, -130, -50, ImPlotCond_Once);
            lock_guard<mutex> lock(g_data.mtx);
            for (auto const& [label, hist] : g_data.cell_logs) if (!hist.x_time.empty()) ImPlot::PlotLine(label.c_str(), hist.x_time.data(), hist.y_rsrp.data(), hist.x_time.size());
            ImPlot::EndPlot();
        }
        ImGui::End();

        // ====== SETTINGS ======
        ImGui::Begin("Settings");
        ImGui::Checkbox("Allow data reception?", &allow_receiving);
        ImGui::Checkbox("Show Map", &show_map); ImGui::Checkbox("Show Legacy Points", &show_legacy_points); ImGui::Checkbox("Show DB Points", &show_db_points);
        ImGui::End();

        // ====== LEGACY WINDOWS ======
        ImGui::Begin("Device Data");
        { lock_guard<mutex> lock(g_data.mtx);
          ImGui::Text("Latitude:  %.6f", g_data.latitude); ImGui::Text("Longitude: %.6f", g_data.longitude); ImGui::Text("Signal:    %d dBm", g_data.dbm);
          if (ImGui::CollapsingHeader("Cell Towers Info")) ImGui::TextWrapped("%s", g_data.telInf.c_str());
          if (ImGui::CollapsingHeader("Raw Data")) ImGui::TextWrapped("%s", g_data.raw.c_str()); }
        ImGui::End();

        ImGui::Begin("Signal Level (dBm) History");
        static bool auto_scroll = true; ImGui::Checkbox("Auto-scroll", &auto_scroll);
        if (ImPlot::BeginPlot("Cell Tower Signal Strength", ImVec2(-1, 200))) {
            ImPlot::SetupAxes("Time (sec)", "dBm"); lock_guard<mutex> lock(g_data.mtx); ImPlot::SetupAxisLimits(ImAxis_Y1, -140, -40, ImPlotCond_Once);
            if (auto_scroll && !g_data.x_time.empty()) ImPlot::SetupAxisLimits(ImAxis_X1, g_data.x_time.back() - 60, g_data.x_time.back() + 2, ImPlotCond_Always);
            else ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Once);
            if (!g_data.x_time.empty()) { ImPlot::PlotLine("Serving Cell dBm", g_data.x_time.data(), g_data.y_dbm.data(), g_data.x_time.size()); ImPlot::PlotScatter("Current", &g_data.x_time.back(), &g_data.y_dbm.back(), 1); }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Render(); glClearColor(0.1, 0.1, 0.1, 1.0); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); SDL_GL_SwapWindow(window);
    }

    for (auto const& [path, texID] : tile_cache) if (texID > 0) glDeleteTextures(1, (GLuint*)&texID);
    tile_cache.clear();

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImPlot::DestroyContext(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context); SDL_DestroyWindow(window); SDL_Quit();
}