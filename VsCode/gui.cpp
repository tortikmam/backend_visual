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
#include <cpr/cpr.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace std;

// Глобальный кэш текстур для тайлов
static std::map<std::string, GLuint> tile_cache;

// Функции для работы с тайлами
double lon2tile(double lon, int zoom) {
    return (lon + 180.0) / 360.0 * std::pow(2.0, zoom);
}

double lat2tile(double lat, int zoom) {
    return (1.0 - std::log(std::tan(lat * M_PI / 180.0) + 1.0 / std::cos(lat * M_PI / 180.0)) / M_PI) / 2.0 * std::pow(2.0, zoom);
}

double tilex2lon(int x, int z) {
    return x / std::pow(2.0, z) * 360.0 - 180.0;
}

double tiley2lat(int y, int z) {
    double n = M_PI - 2.0 * M_PI * y / std::pow(2.0, z);
    return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

void download_tile_cpr(int z, int x, int y) {
    int max_tile = std::pow(2, z) - 1;
    if (x < 0 || x > max_tile || y < 0 || y > max_tile) return;
    std::string folder = "tiles/" + std::to_string(z) + "/" + std::to_string(x);
    std::string path = folder + "/" + std::to_string(y) + ".png";

    if (std::filesystem::exists(path)) return;
    std::filesystem::create_directories(folder);

    cpr::Response r = cpr::Get(
        cpr::Url{"https://tile.openstreetmap.org/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png"},
        cpr::Header{{"User-Agent", "TelemetryApp/1.0"}},
        cpr::VerifySsl{false}
    );

    if (r.status_code == 200) {
        std::ofstream ofs(path, std::ios::binary);
        ofs << r.text;
        ofs.close();
    }
}

void thread_loader(int z, int x, int y) {
    download_tile_cpr(z, x, y);
}

GLuint LoadTexture(const char* filename) {
    int width, height, channels;
    stbi_set_flip_vertically_on_load(false);
    
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
        return 0;
    }

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);

    return textureID;
}

void run_gui() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_Window* window = SDL_CreateWindow(
        "GPS + CellTower Monitor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    bool running = true;
    bool show_map = true;
    
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
                should_run = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::Begin("Settings");
        ImGui::Checkbox("Allow data reception?", &allow_receiving);
        ImGui::Checkbox("Show Map", &show_map);
        ImGui::End();

        ImGui::Begin("Device Data");
        {
            lock_guard<mutex> lock(g_data.mtx);
            ImGui::Text("Latitude:  %.6f", g_data.latitude);
            ImGui::Text("Longitude: %.6f", g_data.longitude);
            ImGui::Text("Signal:    %d dBm", g_data.dbm);
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Cell Towers Info")) {
                ImGui::TextWrapped("%s", g_data.telInf.c_str());
            }
            if (ImGui::CollapsingHeader("Raw Data")) {
                ImGui::TextWrapped("%s", g_data.raw.c_str());
            }
        }
        ImGui::End();

        ImGui::Begin("Signal Level (dBm) History");
        static bool auto_scroll = true;
        ImGui::Checkbox("Auto-scroll to latest data", &auto_scroll);

        if (ImPlot::BeginPlot("Cell Tower Signal Strength", ImVec2(-1, 200))) {
            ImPlot::SetupAxes("Time (sec)", "dBm");

            lock_guard<mutex> lock(g_data.mtx);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -140, -40, ImPlotCond_Once);
            
            if (auto_scroll && !g_data.x_time.empty()) {
                ImPlot::SetupAxisLimits(ImAxis_X1, g_data.x_time.back() - 60, g_data.x_time.back() + 2, ImPlotCond_Always);
            } else {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Once);
            }

            if (!g_data.x_time.empty()) {
                ImPlot::PlotLine("Serving Cell dBm", g_data.x_time.data(), g_data.y_dbm.data(), (int)g_data.x_time.size());
                ImPlot::PlotScatter("Current", &g_data.x_time.back(), &g_data.y_dbm.back(), 1);
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        // Окно с картой
        if (show_map) {
            ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoCollapse);
            
            if (ImPlot::BeginPlot("##Map", ImVec2(-1, 400), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Longitude", "Latitude");
                
                // Установка начальных границ если координаты есть
                {
                    lock_guard<mutex> lock(g_data.mtx);
                    if (g_data.latitude != 0 && g_data.longitude != 0) {
                        ImPlot::SetupAxesLimits(g_data.longitude - 0.05, g_data.longitude + 0.05, 
                                              g_data.latitude - 0.03, g_data.latitude + 0.03, 
                                              ImGuiCond_FirstUseEver);
                    } else {
                        ImPlot::SetupAxesLimits(82.8, 83.1, 54.9, 55.1, ImGuiCond_FirstUseEver);
                    }
                }
                
                ImPlotRect limits = ImPlot::GetPlotLimits();
                
                // Расчет зума
                int zoom = 15;
                double width = std::abs(limits.X.Max - limits.X.Min);
                if (width > 0) {
                    zoom = (int)std::floor(std::log2(360.0 / width * 2));
                    if (zoom < 1) zoom = 1;
                    if (zoom > 18) zoom = 18;
                }

                // Очистка кэша если слишком много текстур
                if (tile_cache.size() > 500) {
                    for (auto const& [path, texID] : tile_cache) {
                        if (texID > 0) {
                            GLuint id = texID;
                            glDeleteTextures(1, &id);
                        }
                    }
                    tile_cache.clear();
                }

                // Отрисовка тайлов
                if (limits.X.Min > -180 && limits.X.Max < 180) {
                    int x_start = (int)std::floor(lon2tile(limits.X.Min, zoom));
                    int x_end   = (int)std::floor(lon2tile(limits.X.Max, zoom));
                    int y_start = (int)std::floor(lat2tile(limits.Y.Max, zoom));
                    int y_end   = (int)std::floor(lat2tile(limits.Y.Min, zoom));

                    if (std::abs(x_end - x_start) < 20 && std::abs(y_end - y_start) < 20) {
                        for (int x = x_start; x <= x_end; ++x) {
                            for (int y = y_start; y <= y_end; ++y) {
                                std::string path = "tiles/" + std::to_string(zoom) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
                                GLuint texID = 0;

                                if (tile_cache.count(path)) {
                                    texID = tile_cache[path];
                                    if (texID == 0 && std::filesystem::exists(path)) {
                                        texID = LoadTexture(path.c_str());
                                        tile_cache[path] = texID;
                                    }
                                } else {
                                    if (std::filesystem::exists(path)) {
                                        texID = LoadTexture(path.c_str());
                                        tile_cache[path] = texID;
                                    } else {
                                        tile_cache[path] = 0;
                                        std::thread(thread_loader, zoom, x, y).detach();
                                    }
                                }

                                if (texID > 0) {
                                    double l_lon = tilex2lon(x, zoom);
                                    double r_lon = tilex2lon(x + 1, zoom);
                                    double t_lat = tiley2lat(y, zoom);
                                    double b_lat = tiley2lat(y + 1, zoom);
                                    
                                    ImPlot::PlotImage(path.c_str(), (void*)(intptr_t)texID, 
                                                    ImPlotPoint(l_lon, b_lat), 
                                                    ImPlotPoint(r_lon, t_lat));
                                }
                            }
                        }
                    }
                }

                // Отрисовка текущей позиции
                {
                    lock_guard<mutex> lock(g_data.mtx);
                    if (g_data.latitude != 0 && g_data.longitude != 0) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 10.0f, ImVec4(1, 0, 0, 1), 2.0f);
                        ImPlot::PlotScatter("Current Position", &g_data.longitude, &g_data.latitude, 1);
                    }
                }

                ImPlot::EndPlot();
            }
            ImGui::End();
        }

        ImGui::Render();
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Очистка кэша текстур
    for (auto const& [path, texID] : tile_cache) {
        if (texID > 0) {
            GLuint id = texID;
            glDeleteTextures(1, &id);
        }
    }
    tile_cache.clear();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}