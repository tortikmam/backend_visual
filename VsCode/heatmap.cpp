#include "telemetry.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <deque>
#include <functional>
#include <pqxx/pqxx>
#include <filesystem>
#include <GL/glew.h>
#include <algorithm>
#include <future>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace std;

class ThreadPool {
public:
    ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condition.wait(lock, [this] { return stop || !tasks.empty(); });
                        if (stop && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            });
        }
    }
    
    template<class F>
    void enqueue(F&& f) {
        {
            unique_lock<mutex> lock(queueMutex);
            tasks.emplace_back(forward<F>(f));
        }
        condition.notify_one();
    }
    
    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (thread& worker : workers) worker.join();
    }

private:
    vector<thread> workers;
    deque<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condition;
    bool stop;
};

static unique_ptr<ThreadPool> g_threadPool = nullptr;
static mutex g_poolMutex;

ThreadPool& getThreadPool() {
    lock_guard<mutex> lock(g_poolMutex);
    if (!g_threadPool) g_threadPool = make_unique<ThreadPool>(thread::hardware_concurrency());
    return *g_threadPool;
}

HeatmapStatus g_heatmap_status;
static unordered_map<string, GLuint> g_heatTile_cache;
static mutex g_heatCacheMtx;

struct DataPoint { 
    double lat, lon, value; 
};

double lon_to_tile_x(double lon, int z) { 
    return (lon + 180.0) / 360.0 * (1 << z); 
}

double lat_to_tile_y(double lat, int z) { 
    return (1.0 - asinh(tan(lat * M_PI / 180.0)) / M_PI) / 2.0 * (1 << z); 
}

double tile_x_to_lon(double x, int z) { 
    return x / (double)(1 << z) * 360.0 - 180.0; 
}

double tile_y_to_lat(double y, int z) { 
    double n = M_PI - 2.0 * M_PI * y / (double)(1 << z); 
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n))); 
}

double haversine(double lat1, double lon1, double lat2, double lon2) {
    double R = 6371000.0;
    double dLat = (lat2-lat1)*M_PI/180.0;
    double dLon = (lon2-lon1)*M_PI/180.0;
    double a = sin(dLat/2)*sin(dLat/2) + cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dLon/2)*sin(dLon/2);
    return R * 2 * atan2(sqrt(a), sqrt(1-a));
}

void get_color(double val, double minVal, double maxVal, const string& criterion, 
               float alphaScale, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (isnan(val) || val < -110.0) { 
        r=0; g=0; b=0; a=0; 
        return; 
    }
    
    struct CP { 
        double v; 
        uint8_t r, g, b; 
    };
    
    static const CP cps_rsrp[] = {
        {-110, 0, 0, 139},
        {-100, 0, 255, 255},
        {-90, 255, 255, 0},
        {-80, 255, 140, 0},
        {-60, 255, 0, 0}
    };
    
    const auto& cps = cps_rsrp;
    const int N = 5;
    
    if (val <= cps[0].v) { 
        r=cps[0].r; g=cps[0].g; b=cps[0].b; 
        a=(uint8_t)(220*alphaScale); 
        return; 
    }
    
    if (val >= cps[N-1].v) { 
        r=cps[N-1].r; g=cps[N-1].g; b=cps[N-1].b; 
        a=(uint8_t)(245*alphaScale); 
        return; 
    }
    
    for(int i=1; i<N; ++i) {
        if(val <= cps[i].v) { 
            double t = (val-cps[i-1].v)/(cps[i].v-cps[i-1].v); 
            r = cps[i-1].r + t*(cps[i].r - cps[i-1].r); 
            g = cps[i-1].g + t*(cps[i].g - cps[i-1].g); 
            b = cps[i-1].b + t*(cps[i].b - cps[i-1].b); 
            a = (uint8_t)(245*alphaScale); 
            return; 
        }
    }
    
    r=255; g=0; b=0; a=(uint8_t)(245*alphaScale);
}

double idw_interpolate(double lat, double lon, const vector<DataPoint>& pts, 
                       double searchRadius, double power) {
    double num = 0.0, den = 0.0;
    bool has = false;
    
    for (const auto& p : pts) {
        double dist = haversine(lat, lon, p.lat, p.lon);
        if (dist > searchRadius) continue;
        if (dist < 1.0) return p.value;
        double w = 1.0 / pow(dist, power);
        num += w * p.value;
        den += w;
        has = true;
    }
    
    return (has && den > 1e-9) ? num / den : numeric_limits<double>::quiet_NaN();
}

vector<DataPoint> collect_points(const string& db_conn, const HeatmapConfig& config) {
    vector<DataPoint> points;
    
    try {
        pqxx::connection c(db_conn);
        pqxx::nontransaction N(c);
        string field = "tl.rsrp";
        
        if (config.criterion == "RSRQ") field = "tl.rsrq";
        else if (config.criterion == "RSSI") field = "tl.rssi";
        else if (config.criterion == "Altitude") field = "lr.altitude";
        
        string query = "SELECT lr.latitude, lr.longitude, " + field + " as val, tl.pci "
                       "FROM telephony_lte tl "
                       "JOIN location_records lr ON tl.location_record_id = lr.id "
                       "WHERE lr.latitude != 0 AND lr.longitude != 0 AND tl.rsrp IS NOT NULL";
        
        if (!config.useAllPCIs && !config.selectedPCIs.empty()) {
            query += " AND tl.pci IN (";
            for (size_t i=0; i<config.selectedPCIs.size(); ++i) { 
                if(i>0) query+=","; 
                query += to_string(config.selectedPCIs[i]); 
            }
            query += ")";
        }
        
        if (!config.earfcn.empty()) {
            query += " AND tl.earfcn = " + config.earfcn;
        }

        pqxx::result res = N.exec(query);
        
        for (auto const &row : res) {
            double v = row["val"].is_null() ? 0.0 : row["val"].as<double>();
            double lat = row["latitude"].as<double>();
            double lon = row["longitude"].as<double>();
            if (v != 0.0 && lat != 0.0 && lon != 0.0) {
                points.push_back({lat, lon, v});
            }
        }
    } catch (const exception& e) { 
        cerr << "[HEATMAP] DB ERROR: " << e.what() << endl; 
    }
    
    return points;
}

vector<int> get_available_pcis(const string& db_conn, const string&) {
    vector<int> pcis;
    
    try {
        pqxx::connection c(db_conn);
        pqxx::nontransaction N(c);
        pqxx::result res = N.exec("SELECT DISTINCT pci FROM telephony_lte "
                                  "WHERE pci IS NOT NULL AND pci > 0 ORDER BY pci");
        for (auto const& row : res) pcis.push_back(row[0].as<int>());
    } catch (...) {}
    
    return pcis;
}

bool save_png(const string& path, const vector<unsigned char>& img, int w, int h) {
    filesystem::create_directories(filesystem::path(path).parent_path());
    return stbi_write_png(path.c_str(), w, h, 4, img.data(), w * 4) != 0;
}

bool generate_heatmap_tiles(const string& db_conn, const HeatmapConfig& config) {
    g_heatmap_status.generating.store(true);
    g_heatmap_status.progress.store(0);
    { 
        lock_guard<mutex> lock(g_heatmap_status.mtx); 
        g_heatmap_status.message = "Сбор точек..."; 
    }
    
    auto points = collect_points(db_conn, config);
    
    if (points.empty()) {
        g_heatmap_status.generating.store(false);
        { 
            lock_guard<mutex> lock(g_heatmap_status.mtx); 
            g_heatmap_status.message = "Нет данных"; 
        }
        return false;
    }

    double minVal = 1e9, maxVal = -1e9;
    double minLat=90, maxLat=-90, minLon=180, maxLon=-180;
    
    for (const auto& p : points) {
        minVal = min(minVal, p.value);
        maxVal = max(maxVal, p.value);
        minLat = min(minLat, p.lat);
        maxLat = max(maxLat, p.lat);
        minLon = min(minLon, p.lon);
        maxLon = max(maxLon, p.lon);
    }
    
    if (config.criterion == "RSRP") { 
        minVal = -120.0; 
        maxVal = -60.0; 
    }
    
    double cLat = 0.5*(minLat+maxLat);
    double latPad = config.searchRadiusMeters/111320.0;
    double lonPad = config.searchRadiusMeters/max(1.0, 111320.0*cos(cLat*M_PI/180.0));
    double bboxMinLon = minLon-lonPad;
    double bboxMaxLon = maxLon+lonPad;
    double bboxMinLat = minLat-latPad;
    double bboxMaxLat = maxLat+latPad;
    
    int zoom = config.zoom;
    int worldTiles = (1<<zoom);
    int minTX = max(0, (int)floor(lon_to_tile_x(bboxMinLon, zoom)));
    int maxTX = min(worldTiles-1, (int)floor(lon_to_tile_x(bboxMaxLon, zoom)));
    int minTY = max(0, (int)floor(lat_to_tile_y(bboxMaxLat, zoom)));
    int maxTY = min(worldTiles-1, (int)floor(lat_to_tile_y(bboxMinLat, zoom)));
    int tileTotal = max(1, (maxTX-minTX+1)*(maxTY-minTY+1));
    
    { 
        lock_guard<mutex> lock(g_heatmap_status.mtx); 
        g_heatmap_status.message = "Генерация "+to_string(tileTotal)+" тайлов..."; 
    }

    atomic<bool> canceled{false};
    atomic<int> tilesGenerated{0};
    mutex localMutex;
    bool savedAny = false;
    
    auto processTile = [&](int tx, int ty) {
        if(canceled.load()) { 
            tilesGenerated++; 
            return; 
        }
        
        double tMinLon = tile_x_to_lon(tx, zoom);
        double tMaxLon = tile_x_to_lon(tx+1, zoom);
        double tMaxLat = tile_y_to_lat(ty, zoom);
        double tMinLat = tile_y_to_lat(ty+1, zoom);
        double midLat = 0.5*(tMinLat+tMaxLat);
        double latRad = (config.searchRadiusMeters*1.5)/111320.0;
        double lonRad = (config.searchRadiusMeters*1.5)/max(1.0, 111320.0*cos(midLat*M_PI/180.0));
        
        vector<DataPoint> localPts;
        localPts.reserve(2000);
        
        for(const auto& p : points) {
            if((int)localPts.size() < 2000 && 
               p.lat >= tMinLat-latRad && p.lat <= tMaxLat+latRad && 
               p.lon >= tMinLon-lonRad && p.lon <= tMaxLon+lonRad) {
                localPts.push_back(p);
            }
        }
        
        if(localPts.empty()) { 
            tilesGenerated++; 
            return; 
        }

        const int W=256, H=256;
        vector<unsigned char> out(W*H*4, 0);
        
        for(int py=0; py<H; ++py) {
            int globalPct = (int)(100.0*(tilesGenerated.load() + py/(double)H)/tileTotal);
            g_heatmap_status.progress.store(std::clamp(globalPct, 0, 99));
            
            if(canceled.load()) continue;
            
            double p_lat = tMaxLat - (py/(double)H)*(tMaxLat-tMinLat);
            
            for(int px=0; px<W; ++px) {
                double p_lon = tMinLon + (px/(double)W)*(tMaxLon-tMinLon);
                double val = idw_interpolate(p_lat, p_lon, localPts, 
                                             config.searchRadiusMeters, config.idwPower);
                uint8_t r, g, b, a;
                get_color(val, minVal, maxVal, config.criterion, config.alpha, r, g, b, a);
                
                int idx = (py*W+px)*4;
                out[idx] = r;
                out[idx+1] = g;
                out[idx+2] = b;
                out[idx+3] = a;
            }
        }
        
        string path = "tiles/heatmap/" + to_string(zoom) + "/" + 
                      to_string(tx) + "/" + to_string(ty) + ".png";
        { 
            lock_guard<mutex> lock(localMutex); 
            if(save_png(path, out, W, H)) savedAny = true; 
        }
        
        tilesGenerated++;
    };

    for(int tx=minTX; tx<=maxTX; ++tx) {
        for(int ty=minTY; ty<=maxTY; ++ty) {
            getThreadPool().enqueue([&, tx, ty]() { 
                processTile(tx, ty); 
            });
        }
    }
    
    while(tilesGenerated.load() < tileTotal && !canceled.load()) {
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    
    this_thread::sleep_for(chrono::milliseconds(200));
    
    g_heatmap_status.generating.store(false);
    g_heatmap_status.progress.store(100);
    { 
        lock_guard<mutex> lock(g_heatmap_status.mtx); 
        g_heatmap_status.message = savedAny ? "Готово!" : "Ошибка"; 
    }
    
    { 
        lock_guard<mutex> lock(g_data.mtx);
        g_data.heatmap_min_lat = bboxMinLat;
        g_data.heatmap_max_lat = bboxMaxLat;
        g_data.heatmap_min_lon = bboxMinLon;
        g_data.heatmap_max_lon = bboxMaxLon;
        g_data.heatmap_earfcn = config.earfcn;
        g_data.heatmap_criterion = config.criterion;
        g_data.heatmap_zoom = zoom;
        g_data.heatmap_ready = true;
    }
    
    return savedAny;
}

void generate_heatmap_async(const string& db_conn, const HeatmapConfig& config) {
    thread([db_conn, config]() { 
        generate_heatmap_tiles(db_conn, config); 
    }).detach();
}

bool is_heatmap_generating() { 
    return g_heatmap_status.generating.load(); 
}

int get_heatmap_progress() { 
    return g_heatmap_status.progress.load(); 
}

string get_heatmap_message() { 
    lock_guard<mutex> lock(g_heatmap_status.mtx); 
    return g_heatmap_status.message; 
}

void clear_heatmap_cache() {
    lock_guard<mutex> lock(g_heatCacheMtx);
    for(auto const& [path, texID] : g_heatTile_cache) {
        if(texID > 0) glDeleteTextures(1, (GLuint*)&texID);
    }
    g_heatTile_cache.clear();
    g_data.heatmap_ready = false;
}