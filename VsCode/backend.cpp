#include "telemetry.hpp"
#include <zmq.hpp>
#include <iostream>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace std;
using json = nlohmann::json;

void save_to_json(const string& raw_msg, float lat, float lon, int dbm, int rsrq, int rssi) {
    json entry;
    auto now = chrono::system_clock::now();
    entry["accuracy"] = 1.0; entry["latitude"] = lat; entry["longitude"] = lon;
    entry["provider"] = "gps"; entry["source"] = "source";
    entry["recordedTime"] = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count();
    entry["timestamp"] = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count();

    json telephony, lte, identity, signal;
    identity["band"] = 3; identity["ci"] = 0; identity["earfcn"] = 0; identity["mcc"] = "250"; identity["mnc"] = "01"; identity["pci"] = 0; identity["tac"] = 0;
    signal["asu"] = 28; signal["rsrp"] = dbm; signal["rsrq"] = rsrq; signal["rssnr"] = 0;
    lte["identity"] = identity; lte["signal"] = signal; telephony["LTE"] = lte;
    entry["telephony"] = telephony;

    ofstream file("telemetry_log.json", ios::app);
    if (file.is_open()) { file << entry.dump() << endl; file.close(); }
}

void backend() {
    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://0.0.0.0:8888");
    socket.set(zmq::sockopt::rcvtimeo, 100);
    cout << "Начал слушать на порту 8888..." << endl;

    while (should_run) {
        zmq::message_t request;
        auto res = socket.recv(request, zmq::recv_flags::none);
        if (res) {
            if (!allow_receiving) {
                string reply_str = "DISABLED";
                socket.send(zmq::buffer(reply_str), zmq::send_flags::none);
                continue;
            }
            string msg_str(static_cast<char*>(request.data()), request.size());
            cout << "--- Новое сообщение ---\n" << msg_str << "\n----------------------" << endl;

            float lat = 0.0f, lon = 0.0f;
            if (sscanf(msg_str.c_str(), "LAT: %f, LON: %f", &lat, &lon) == 2) {
                int current_dbm = parse_dbm(msg_str);
                size_t pos = msg_str.find("CellInfo");
                string inf = (pos != string::npos) ? msg_str.substr(pos) : "";

                double elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - g_data.start_time).count() / 1000.0;

                lock_guard<mutex> lock(g_data.mtx);
                g_data.latitude = lat; g_data.longitude = lon; g_data.telInf = inf; g_data.raw = msg_str;
                g_data.dbm = parse_dbm(msg_str); g_data.rssi = parse_rssi(msg_str); g_data.rsrq = parse_rsrq(msg_str);
                g_data.x_time.push_back(elapsed); g_data.y_dbm.push_back(g_data.dbm);
                if (g_data.x_time.size() > 200) { g_data.x_time.erase(g_data.x_time.begin()); g_data.y_dbm.erase(g_data.y_dbm.begin()); }

                // Сохраняем в JSON
                save_to_json(msg_str, lat, lon, current_dbm, g_data.rsrq, g_data.rssi);
            }
            string reply_str = "OK";
            socket.send(zmq::buffer(reply_str), zmq::send_flags::none);
        }
    }
    cout << "Backend остановлен..." << endl;
}