#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <string>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <zmq.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "implot.h"

using namespace std;

using json = nlohmann::json;
atomic<bool> should_run(true);

struct TelemetryData {
    float latitude = 0, longitude = 0;
    int dbm = 0;
    int rssi = 0;
    int rsrq = 0;
    string telInf = "";
    string raw = "";
    mutex mtx;
    vector<double> x_time;
    vector<double> y_dbm;
    chrono::steady_clock::time_point start_time;

    TelemetryData() { start_time = chrono::steady_clock::now(); }
};

TelemetryData g_data;
bool allow_receiving = true;

int parse_dbm(const string& text) {
    size_t dbm_pos = text.find("DBM:");
    if (dbm_pos == string::npos) {
        return 0;
    }

    size_t num_start = text.find_first_not_of(" \t", dbm_pos + 4);
    if (num_start == string::npos) {
        return 0;
    }

    size_t num_end = text.find_first_of(" \t\n\r", num_start);
    if (num_end == string::npos) {
        num_end = text.size();
    }

    string dbm_str = text.substr(num_start, num_end - num_start);
    
    try {
        return stoi(dbm_str);
    } catch (...) {
        return 0;
    }
}

int parse_rsrq(const string& text) {
    size_t rsrq_pos = text.find("RSRQ:");
    if (rsrq_pos == string::npos) return 0;
    
    size_t num_start = text.find_first_not_of(" \t", rsrq_pos + 5);
    if (num_start == string::npos) return 0;
    
    size_t num_end = text.find_first_of(" \t\n\r", num_start);
    if (num_end == string::npos) num_end = text.size();
    
    string rsrq_str = text.substr(num_start, num_end - num_start);
    
    try {
        return stoi(rsrq_str);
    } catch (...) {
        return 0;
    }
}

int parse_rssi(const string& text) {
    size_t rssi_pos = text.find("RSSI:");
    if (rssi_pos == string::npos) return 0;
    
    size_t num_start = text.find_first_not_of(" \t", rssi_pos + 5);
    if (num_start == string::npos) return 0;
    
    size_t num_end = text.find_first_of(" \t\n\r", num_start);
    if (num_end == string::npos) num_end = text.size();
    
    string rssi_str = text.substr(num_start, num_end - num_start);
    
    try {
        return stoi(rssi_str);
    } catch (...) {
        return 0;
    }
}

void save_to_json(const string& raw_msg, float lat, float lon, int dbm, int rsrq, int rssi) {
    json entry;
    
    auto now = chrono::system_clock::now();
    auto in_time_t = chrono::system_clock::to_time_t(now);
    
    entry["timestamp"] = in_time_t;
    entry["lat"] = lat;
    entry["lon"] = lon;
    entry["rsrp"] = dbm;
    entry["rsrq"] = rsrq;
    entry["rssi"] = rssi;
    entry["raw_data"] = raw_msg;

    ofstream file("telemetry_log.json", ios::app);
    if (file.is_open()) {
        file << entry.dump() << endl; 
        file.close();
    }
}

void backend() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://0.0.0.0:8888");

    int timeout = 100;
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    cout << "Начал слушать на порту 8888..." << endl;

    while (should_run) {
        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);
        
        if (res) {
            if (!allow_receiving) {
                string reply_str = "DISABLED";
                zmq::message_t reply(reply_str.size());
                memcpy(reply.data(), reply_str.data(), reply_str.size());
                socket.send(reply, zmq::send_flags::none);
                continue;
            }

            string msg_str(static_cast<char*>(request.data()), request.size());

            cout << "\n--- Новое сообщение ---" << endl;
            cout << msg_str << endl;
            cout << "----------------------" << endl;

            float lat = 0.0f;
            float lon = 0.0f;

            if (sscanf(msg_str.c_str(), "LAT: %f, LON: %f", &lat, &lon) == 2) {

                int current_dbm = parse_dbm(msg_str);

                size_t pos = msg_str.find("CellInfo");
                string inf;
                if (pos != string::npos) {
                    inf = msg_str.substr(pos);
                }

                auto now = chrono::steady_clock::now();
                double elapsed = chrono::duration_cast<chrono::milliseconds>(now - g_data.start_time).count() / 1000.0;

                lock_guard<mutex> lock(g_data.mtx);
                g_data.latitude = lat;
                g_data.longitude = lon;
                g_data.telInf = inf;
                g_data.raw = msg_str;
                g_data.dbm = parse_dbm(msg_str); 
                g_data.rssi = parse_rssi(msg_str);
                g_data.rsrq = parse_rsrq(msg_str);


                g_data.x_time.push_back(elapsed);
                g_data.y_dbm.push_back((double)g_data.dbm);

                if (g_data.x_time.size() > 200) {
                    g_data.x_time.erase(g_data.x_time.begin());
                    g_data.y_dbm.erase(g_data.y_dbm.begin());
                }
                save_to_json(msg_str, lat, lon, current_dbm, g_data.rsrq, g_data.rssi);
            }

            string reply_str = "OK";
            zmq::message_t reply(reply_str.size());
            memcpy(reply.data(), reply_str.data(), reply_str.size());
            socket.send(reply, zmq::send_flags::none);
        }
    }
    cout << "Backend остановлен..." << endl;
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

        if (ImPlot::BeginPlot("Cell Tower Signal Strength", ImVec2(-1, -1))) {
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

        ImGui::Render();
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, char *argv[]) {
    thread back_thread(backend);
    run_gui();

    if (back_thread.joinable()) {
        back_thread.join();
    }
    return 0;
}
