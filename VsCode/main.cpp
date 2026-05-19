#include "telemetry.hpp"
#include <thread>

std::atomic<bool> should_run(true);
TelemetryData g_data;
bool allow_receiving = true;

TelemetryData::TelemetryData() { 
    start_time = std::chrono::steady_clock::now(); 
}

int main(int argc, char *argv[]) {
    init_database();

    std::thread back_thread(backend);
    back_thread.detach();

    run_gui();
    return 0;
}