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
#include <future>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
using namespace std;

static std::map<std::string, GLuint> tile_cache;
static std::map<std::string, GLuint> heat_tile_cache;
static std::mutex heat_cache_mtx;

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
    
    if (!data) return 0;
    
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

double calculate_distance(double lat1, double lon1, double lat2, double lon2) {
    double dy = (lat2 - lat1) * 111139.0;
    double dx = (lon2 - lon1) * 111139.0 * cos(lat1 * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

void run_gui() {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_Window* window = SDL_CreateWindow("GPS + CellTower Monitor", 
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                          1280, 800, 
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewExperimental = GL_TRUE; 
    if (glewInit() != GLEW_OK) cerr << "GLEW Error" << endl;

    ImGui::CreateContext(); 
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); 
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context); 
    ImGui_ImplOpenGL3_Init("#version 330");

    bool running = true, show_map = true, show_db_points = true;
    std::vector<DbPoint> db_points = LoadPointsFromDatabase();
    HeatmapConfig hm_cfg;
    static std::future<bool> hm_future;
    static bool hm_result_ready = false;
    static vector<int> available_pcis, selected_pcis;
    static bool use_all_pcis = true, need_pci_refresh = true, use_all_earfcns = true;
    static char target_earfcn[32] = "38100";
    static string last_earfcn_key = "ALL";

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

        // ====== SMARTPHONE DATA ======
        ImGui::Begin("Smartphone Data");
        { 
            lock_guard<mutex> lock(g_data.mtx);
            
            if (g_data.db_connected) {
                ImGui::TextColored(ImVec4(0,1,0,1), "● DATABASE: ONLINE");
            } else { 
                ImGui::TextColored(ImVec4(1,0,0,1), "○ DATABASE: OFFLINE"); 
                if(ImGui::SmallButton("Retry Connect")) {
                    thread([](){init_database();}).detach();
                } 
            }
            
            ImGui::Text("Source: %s", g_data.data_source.c_str()); 
            ImGui::Separator();
            ImGui::Text("Latitude:  %.6f", g_data.lat); 
            ImGui::Text("Longitude: %.6f", g_data.lon);
            ImGui::Text("Net Type:  %s", g_data.type.c_str()); 
            ImGui::Text("RSRP:      %d dBm", g_data.rsrp); 
            ImGui::Separator();
            ImGui::Text("Time Range Filter (seconds from start):");
            ImGui::SliderFloat("Start Time", &g_data.view_min_time, 0.0f, g_data.max_recorded_time);
            ImGui::SliderFloat("End Time",   &g_data.view_max_time, 0.0f, g_data.max_recorded_time);
            if(g_data.view_min_time > g_data.view_max_time) {
                g_data.view_min_time = g_data.view_max_time;
            }
            ImGui::Separator();
            
            if(ImGui::Button("Migrate JSON -> PostgreSQL", ImVec2(-1,40))) {
                thread(migrate_json_to_sql).detach();
            }
            
            if(ImGui::TreeNode("Raw JSON")) { 
                ImGui::TextWrapped("%s", g_data.raw.c_str()); 
                ImGui::TreePop(); 
            }
        } 
        ImGui::End();

        // ====== MAP WINDOW ======
        if (show_map) {
            ImGui::Begin("Live Map View", nullptr, ImGuiWindowFlags_NoCollapse);
            
            if (ImPlot::BeginPlot("##MainMap", ImVec2(-1,-1), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Longitude", "Latitude"); 
                ImPlot::SetupAxesLimits(82.8, 83.1, 54.9, 55.1, ImGuiCond_FirstUseEver);
                
                ImPlotRect limits = ImPlot::GetPlotLimits();
                int zoom = 15; 
                double width = std::abs(limits.X.Max - limits.X.Min);
                
                if(width > 0) { 
                    zoom = (int)std::floor(std::log2(360.0/width*2)); 
                    if(zoom < 1) zoom = 1; 
                    if(zoom > 18) zoom = 18; 
                }

                if(tile_cache.size() > 500) { 
                    for(auto const& [p,t] : tile_cache) {
                        if(t > 0) glDeleteTextures(1, (GLuint*)&t);
                    } 
                    tile_cache.clear(); 
                }
                
                if(heat_tile_cache.size() > 500) { 
                    lock_guard<mutex> lock(heat_cache_mtx); 
                    for(auto const& [p,t] : heat_tile_cache) {
                        if(t > 0) glDeleteTextures(1, (GLuint*)&t);
                    } 
                    heat_tile_cache.clear(); 
                }

                if(limits.X.Min > -180 && limits.X.Max < 180 && 
                   limits.Y.Min > -90 && limits.Y.Max < 90) {
                    
                    int xs = (int)std::floor(lon2tile(limits.X.Min, zoom));
                    int xe = (int)std::floor(lon2tile(limits.X.Max, zoom));
                    int ys = (int)std::floor(lat2tile(limits.Y.Max, zoom));
                    int ye = (int)std::floor(lat2tile(limits.Y.Min, zoom));
                    
                    if(std::abs(xe-xs) < 20 && std::abs(ye-ys) < 20) {
                        for(int x=xs; x<=xe; ++x) {
                            for(int y=ys; y<=ye; ++y) {
                                double l_lon = tilex2lon(x, zoom);
                                double r_lon = tilex2lon(x+1, zoom);
                                double b_lat = tiley2lat(y+1, zoom);
                                double t_lat = tiley2lat(y, zoom);
                                string path = "tiles/"+to_string(zoom)+"/"+to_string(x)+"/"+to_string(y)+".png";
                                GLuint texID = 0;
                                
                                if(tile_cache.count(path)) { 
                                    texID = tile_cache[path]; 
                                    if(texID == 0 && std::filesystem::exists(path) && 
                                       std::filesystem::file_size(path) > 0) { 
                                        texID = LoadTexture(path.c_str()); 
                                        tile_cache[path] = texID; 
                                    } 
                                } else { 
                                    if(std::filesystem::exists(path) && 
                                       std::filesystem::file_size(path) > 0) { 
                                        texID = LoadTexture(path.c_str()); 
                                        tile_cache[path] = texID; 
                                    } else { 
                                        tile_cache[path] = 0; 
                                        std::thread(thread_loader, zoom, x, y).detach(); 
                                    } 
                                }
                                
                                if(texID > 0) {
                                    ImPlot::PlotImage(path.c_str(), (void*)(intptr_t)texID, 
                                                     ImPlotPoint(l_lon, b_lat), 
                                                     ImPlotPoint(r_lon, t_lat));
                                }

                                // HEATMAP OVERLAY
                                if(g_data.heatmap_ready && zoom == g_data.heatmap_zoom) {
                                    string hp = "tiles/heatmap/"+to_string(zoom)+"/"+to_string(x)+"/"+to_string(y)+".png";
                                    GLuint ht = 0;
                                    { 
                                        lock_guard<mutex> lock(heat_cache_mtx);
                                        if(heat_tile_cache.count(hp)) { 
                                            ht = heat_tile_cache[hp]; 
                                            if(ht == 0 && std::filesystem::exists(hp) && 
                                               std::filesystem::file_size(hp) > 0) { 
                                                ht = LoadTexture(hp.c_str()); 
                                                heat_tile_cache[hp] = ht; 
                                            } 
                                        } else { 
                                            if(std::filesystem::exists(hp) && 
                                               std::filesystem::file_size(hp) > 0) { 
                                                ht = LoadTexture(hp.c_str()); 
                                                heat_tile_cache[hp] = ht; 
                                            } else {
                                                heat_tile_cache[hp] = 0; 
                                            }
                                        }
                                    }
                                    
                                    if(ht > 0) {
                                        ImPlot::PlotImage(("##hm_"+to_string(x)+"_"+to_string(y)).c_str(), 
                                                         (void*)(intptr_t)ht, 
                                                         ImPlotPoint(l_lon, b_lat), 
                                                         ImPlotPoint(r_lon, t_lat));
                                    }
                                }
                            }
                        }
                    }
                }

                // ====== ТРЕК И ТОЧКИ ======
                if (show_db_points) {
                    lock_guard<mutex> lock(g_data.mtx);
                    static std::vector<double> fx, fy, ft; 
                    fx.clear(); fy.clear(); ft.clear();
                    
                    for(size_t i=0; i<g_data.history_time.size(); ++i) {
                        if(g_data.history_time[i] >= g_data.view_min_time && 
                           g_data.history_time[i] <= g_data.view_max_time) { 
                            fx.push_back(g_data.history_lon[i]); 
                            fy.push_back(g_data.history_lat[i]); 
                            ft.push_back(g_data.history_time[i]); 
                        }
                    }
                    
                    if(!fx.empty()) { 
                        ImDrawList* dl = ImPlot::GetPlotDrawList(); 
                        if(fx.size() > 1) { 
                            for(size_t i=0; i<fx.size()-1; ++i) { 
                                double dt = ft[i+1] - ft[i]; 
                                if(dt > 0 && dt <= 25.0) { 
                                    double d = calculate_distance(fy[i], fx[i], fy[i+1], fx[i+1]); 
                                    if(d/dt < 160.0) {
                                        dl->AddLine(ImPlot::PlotToPixels({fx[i], fy[i]}), 
                                                   ImPlot::PlotToPixels({fx[i+1], fy[i+1]}), 
                                                   IM_COL32(50, 100, 255, 200), 3.0f); 
                                    }
                                } 
                            } 
                        } 
                        
                        ImVec2 last = ImPlot::PlotToPixels({fx.back(), fy.back()}); 
                        dl->AddCircleFilled(last, 6.0f, IM_COL32(255, 255, 0, 255)); 
                        dl->AddCircle(last, 6.5f, IM_COL32(0, 0, 0, 255), 0, 1.5f); 
                    }
                }
                
                if(show_db_points && !db_points.empty()) { 
                    ImDrawList* dl = ImPlot::GetPlotDrawList(); 
                    for(const auto& pt : db_points) { 
                        ImVec2 px = ImPlot::PlotToPixels({pt.longitude, pt.latitude}); 
                        dl->AddCircleFilled(px, 4.0f, IM_COL32(0, 255, 0, 180)); 
                        dl->AddCircle(px, 4.5f, IM_COL32(0, 100, 0, 255), 0, 1.0f); 
                    } 
                }
                
                ImPlot::EndPlot();
            } 
            ImGui::End();
        }

        // ====== RSRP HISTORY ======
        ImGui::Begin("Signal Level (RSRP) History");
        if(ImPlot::BeginPlot("RSRP over Time", ImVec2(-1,-1))) {
            ImPlot::SetupAxisLimits(ImAxis_X1, g_data.view_min_time, g_data.view_max_time, ImPlotCond_Always);
            ImPlot::SetupAxes("Time (sec)", "RSRP (dBm)"); 
            ImPlot::SetupAxisLimits(ImAxis_Y1, -130, -50, ImPlotCond_Once);
            
            lock_guard<mutex> lock(g_data.mtx); 
            for(auto const& [l,h] : g_data.cell_logs) {
                if(!h.x_time.empty()) {
                    ImPlot::PlotLine(l.c_str(), h.x_time.data(), h.y_rsrp.data(), h.x_time.size());
                }
            }
            
            ImPlot::EndPlot();
        } 
        ImGui::End();

        // ====== SETTINGS + HEATMAP UI ======
        ImGui::Begin("Settings");
        ImGui::Checkbox("Allow data reception?", &allow_receiving);
        ImGui::Checkbox("Show Map", &show_map); 
        ImGui::Checkbox("Show DB Points & Track", &show_db_points);
        ImGui::Separator(); 
        ImGui::Text("Heatmap Generator (IDW)");

        static int sel_crit = 0; 
        const char* crits[] = {"RSRP", "RSRQ", "RSSI", "Altitude"};
        ImGui::Combo("Criterion", &sel_crit, crits, IM_ARRAYSIZE(crits)); 
        hm_cfg.criterion = crits[sel_crit];

        ImGui::Text("EARFCN Filter:");
        if(ImGui::RadioButton("All EARFCNs", use_all_earfcns)) { 
            use_all_earfcns = true; 
            need_pci_refresh = true; 
        }
        ImGui::SameLine(); 
        if(ImGui::RadioButton("Specific", !use_all_earfcns)) { 
            use_all_earfcns = false; 
            need_pci_refresh = true; 
        }
        
        if(!use_all_earfcns) { 
            ImGui::InputText("EARFCN", target_earfcn, 32); 
            hm_cfg.earfcn = target_earfcn; 
        } else { 
            hm_cfg.earfcn = ""; 
            ImGui::BeginDisabled(); 
            ImGui::InputText("EARFCN", target_earfcn, 32); 
            ImGui::EndDisabled(); 
        }

        string cur_ek = use_all_earfcns ? "ALL" : hm_cfg.earfcn;
        if(need_pci_refresh || last_earfcn_key != cur_ek) {
            if(g_data.db_connected) { 
                available_pcis = get_available_pcis(DB_CONN, hm_cfg.earfcn); 
                selected_pcis.clear(); 
                use_all_pcis = true; 
            }
            last_earfcn_key = cur_ek; 
            need_pci_refresh = false;
        }
        
        ImGui::Separator(); 
        ImGui::Text("PCI Filter:");
        if(ImGui::Checkbox("Use all PCIs", &use_all_pcis)) { 
            if(use_all_pcis) selected_pcis.clear(); 
        }
        
        if(!use_all_pcis && !available_pcis.empty()) {
            for(size_t i=0; i<available_pcis.size(); ++i) {
                bool sel = std::find(selected_pcis.begin(), selected_pcis.end(), 
                                    available_pcis[i]) != selected_pcis.end();
                if(ImGui::Checkbox(("PCI "+to_string(available_pcis[i])).c_str(), &sel)) {
                    if(sel) selected_pcis.push_back(available_pcis[i]);
                    else selected_pcis.erase(std::remove(selected_pcis.begin(), 
                                                         selected_pcis.end(), 
                                                         available_pcis[i]), 
                                            selected_pcis.end());
                } 
                if(i%5 != 0 && i < available_pcis.size()-1) ImGui::SameLine();
            }
        }
        hm_cfg.useAllPCIs = use_all_pcis; 
        hm_cfg.selectedPCIs = selected_pcis;

        ImGui::Separator();
        static float sr = 35.0f, pw = 2.0f, al = 0.85f;
        ImGui::SliderFloat("Search radius (m)", &sr, 10.0f, 100.0f, "%.0f"); 
        hm_cfg.searchRadiusMeters = sr;
        ImGui::SliderFloat("IDW power", &pw, 1.0f, 4.0f, "%.1f"); 
        hm_cfg.idwPower = pw;
        ImGui::SliderFloat("Alpha", &al, 0.1f, 1.0f, "%.2f"); 
        hm_cfg.alpha = al;
        ImGui::SliderInt("Zoom", &hm_cfg.zoom, 10, 18);
        ImGui::Separator();

        bool can_gen = g_data.db_connected && 
                      (use_all_pcis || !selected_pcis.empty()) && 
                      (use_all_earfcns || !hm_cfg.earfcn.empty()) && 
                      !is_heatmap_generating();
                      
        if(!can_gen) { 
            ImGui::TextColored(ImVec4(1,0,0,1), 
                              can_gen ? "" : (g_data.db_connected ? "Select PCI/EARFCN" : "DB offline!")); 
            ImGui::BeginDisabled(); 
        }
        
        if(ImGui::Button("Generate Heatmap", ImVec2(-1,30))) {
            clear_heatmap_cache(); 
            std::filesystem::create_directories("tiles/heatmap");
            hm_future = std::async(std::launch::async, [c=hm_cfg]() mutable { 
                return generate_heatmap_tiles(DB_CONN, c); 
            });
        }
        
        if(!can_gen) ImGui::EndDisabled();

        if(is_heatmap_generating()) {
            int pct = get_heatmap_progress(); 
            char buf[32]; 
            snprintf(buf, 32, "%d%%", pct);
            ImGui::ProgressBar(pct/100.0f, ImVec2(-1,18), buf); 
            ImGui::TextColored(ImVec4(1,1,0,1), "%s", get_heatmap_message().c_str());
        } else if(g_data.heatmap_ready) {
            ImGui::TextColored(ImVec4(0,1,0,1), "Heatmap: READY (%s/%s, z=%d)", 
                              g_data.heatmap_criterion.c_str(), 
                              g_data.heatmap_earfcn.c_str(), 
                              g_data.heatmap_zoom);
        }
        
        if(hm_future.valid() && !hm_result_ready && 
           hm_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { 
            hm_result_ready = true; 
        }
        ImGui::End();

        // Legacy Windows
        ImGui::Begin("Device Data"); 
        { 
            lock_guard<mutex> lock(g_data.mtx); 
            ImGui::Text("Lat: %.6f Lon: %.6f DBM: %d", 
                       g_data.latitude, g_data.longitude, g_data.dbm); 
            if(ImGui::CollapsingHeader("Raw")) {
                ImGui::TextWrapped("%s", g_data.raw.c_str()); 
            } 
        } 
        ImGui::End();
        
        ImGui::Begin("Legacy Graph"); 
        static bool as = true; 
        ImGui::Checkbox("Auto", &as); 
        if(ImPlot::BeginPlot("dBm", ImVec2(-1,200))) { 
            ImPlot::SetupAxes("Time", "dBm"); 
            ImPlot::SetupAxisLimits(ImAxis_Y1, -140, -40, ImPlotCond_Once); 
            lock_guard<mutex> lock(g_data.mtx); 
            if(as && !g_data.x_time.empty()) {
                ImPlot::SetupAxisLimits(ImAxis_X1, g_data.x_time.back()-60, 
                                       g_data.x_time.back()+2, ImPlotCond_Always); 
            }
            if(!g_data.x_time.empty()) {
                ImPlot::PlotLine("Serving", g_data.x_time.data(), 
                                g_data.y_dbm.data(), g_data.x_time.size()); 
            }
            ImPlot::EndPlot(); 
        } 
        ImGui::End();

        ImGui::Render(); 
        glClearColor(0.1, 0.1, 0.1, 1.0); 
        glClear(GL_COLOR_BUFFER_BIT); 
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); 
        SDL_GL_SwapWindow(window);
    }

    for(auto const& [p,t] : tile_cache) {
        if(t > 0) glDeleteTextures(1, (GLuint*)&t);
    }
    { 
        lock_guard<mutex> lock(heat_cache_mtx); 
        for(auto const& [p,t] : heat_tile_cache) {
            if(t > 0) glDeleteTextures(1, (GLuint*)&t);
        } 
    }
    
    ImGui_ImplOpenGL3_Shutdown(); 
    ImGui_ImplSDL2_Shutdown(); 
    ImPlot::DestroyContext(); 
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context); 
    SDL_DestroyWindow(window); 
    SDL_Quit();
}