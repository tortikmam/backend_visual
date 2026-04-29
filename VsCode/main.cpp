#include "telemetry.hpp"
#include <thread>

std::atomic<bool> should_run(true);
TelemetryData g_data;
bool allow_receiving = true;

TelemetryData::TelemetryData() { 
    start_time = std::chrono::steady_clock::now(); 
}

int main(int argc, char *argv[]) {
    std::thread back_thread(backend);
    run_gui();

    if (back_thread.joinable()) {
        back_thread.join();
    }
    return 0;
}