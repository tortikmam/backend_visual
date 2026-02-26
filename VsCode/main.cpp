#include <GL/glew.h>
#include <SDL2/SDL.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <string>
#include <cstring>
#include <zmq.hpp>
#include <atomic>
#include <mutex>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "imgui.h"
#include "implot.h"

using namespace std;

atomic<bool> should_run(true);

struct location
{
    float latitude;
    float longitude;
};

location globalLocation;
mutex locationMutex;

void backend(){

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://0.0.0.0:8888");

    int timeout = 200;
    socket.set(zmq::sockopt::rcvtimeo, timeout);

    cout << "Начал слушать..." << endl;

    while (should_run) {

        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);
        
        if (res) {

            string msg_str(static_cast<char*>(request.data()), request.size());

            float lat = 0.0f;
            float lon = 0.0f;
            
            if (sscanf(msg_str.c_str(), "LAT: %f, LON: %f", &lat, &lon) == 2) {
        
                lock_guard<mutex> lock(locationMutex);
                globalLocation.latitude = lat;
                globalLocation.longitude = lon;

                cout << "Получено: " << lat << " | " << lon << endl;
            }

            string reply_str = "OK";
            zmq::message_t reply(reply_str.size());
            memcpy(reply.data(), reply_str.data(), reply_str.size());
            socket.send(reply, zmq::send_flags::none);
        }
    }
    cout << "Backend thread stopping..." << endl;
}

void run_gui(){
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_Window* window = SDL_CreateWindow(
        "Backend start", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Включить Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Включить Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Включить Docking

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
                should_run = false; // ГОВОРИМ БЭКЕНДУ ВЫКЛЮЧИТЬСЯ
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_None);

        {
            static int counter = 0;

            ImGui::Begin("Hello, world!"); 
            float displayLat = 0.0f;
            float displayLon = 0.0f;

            lock_guard<mutex> lock(locationMutex);
            displayLat = globalLocation.latitude;
            displayLon = globalLocation.longitude;
            
            ImGui::Text("Device data");
            ImGui::Separator();

            ImGui::Text("Lat:  %.6f", displayLat); ImGui::SameLine();

            ImGui::Text("Lon: %.6f", displayLon); ImGui::SameLine();
            ImGui::End();
        }

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
        
    static location locationInfo;

    thread back_thread(backend);

    run_gui();

    if (back_thread.joinable()) {
        back_thread.join();
    }

    return 0;
}