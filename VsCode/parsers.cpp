#include "telemetry.hpp"

using namespace std;

int parse_dbm(const string& text) {
    size_t dbm_pos = text.find("DBM:");
    if (dbm_pos == string::npos) return 0;

    size_t num_start = text.find_first_not_of(" \t", dbm_pos + 4);
    if (num_start == string::npos) return 0;

    size_t num_end = text.find_first_of(" \t\n\r", num_start);
    if (num_end == string::npos) num_end = text.size();

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