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