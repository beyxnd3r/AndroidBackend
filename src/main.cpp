#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <filesystem>
#include <cmath>
#include <curl/curl.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include <libpq-fe.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <SDL.h>
#include <SDL_opengl.h>

struct location
{
    float latitude  = 0.0f;
    float longitude = 0.0f;
    float altitude  = 0.0f;
    float accuracy  = 0.0f;
    std::string time = "no data";

    std::vector<nlohmann::json> lte;
    std::vector<nlohmann::json> gsm;
    std::vector<nlohmann::json> nr;
};

std::mutex loc_mutex;

std::map<int, std::vector<float>> rsrp_values_map;
std::map<int, std::vector<float>> rssi_values_map;
std::map<int, std::vector<float>> sinr_values_map;
std::map<int, std::vector<float>> pci_values_map;
std::map<int, std::vector<float>> time_values_map;

float graph_time = 0.0f;

std::vector<float> lat_values;
std::vector<float> lon_values;
std::mutex gps_mutex;

bool json_loaded = false;

PGconn* conn;


std::mutex tile_cache_mutex;
std::map<std::string, GLuint> tile_texture_cache;
std::map<std::string, bool>   tile_requested;

GLuint load_texture(const std::string& path);




struct HeatmapPoint
{
    double lat;
    double lon;
    float  rsrp;
    float  rsrq;
    float  rssi;
    float  altitude;
    int    earfcn;
};

std::vector<HeatmapPoint> heatmap_points;
std::mutex heatmap_points_mutex;


enum class HeatmapCriterion { RSRP = 0, RSRQ, RSSI, Altitude };
static const char* heatmap_criterion_names[] = { "RSRP", "RSRQ", "RSSI", "Altitude" };


static HeatmapCriterion selected_criterion   = HeatmapCriterion::RSRP;
static int              selected_earfcn      = -1; 
static bool             heatmap_enabled      = true;
static bool             gps_track_enabled    = false;
static float            heatmap_alpha        = 0.65f;
static float            idw_radius_m         = 300.0f; 
static float            idw_power            = 2.0f;


std::mutex heatmap_tex_cache_mutex;
struct HeatmapTileEntry
{
    GLuint tex      = 0;
    bool   dirty    = true;   
    bool   computing= false;  
    std::vector<unsigned char> img;
};
std::map<std::string, HeatmapTileEntry> heatmap_tex_cache;


struct ColorStop { float val; unsigned char r, g, b; };


static const std::vector<ColorStop> rsrp_colormap = {
    { -80.0f,  255,  40,  10 },  
    { -85.0f,  255, 120,   0 },  
    { -90.0f,  255, 215,   0 },  
    { -95.0f,  100, 230,  80 },  
    {-100.0f,   30, 180, 240 },  
    {-105.0f,   30,  80, 255 },  
    {-120.0f,  120,  20, 180 },
    {-130.0f,  180,  30, 180 },
};


static const std::vector<ColorStop> rsrq_colormap = {
    {  -3.0f, 255,  40,  10 },
    {  -7.0f, 255, 140,   0 },
    { -10.0f, 255, 215,   0 },
    { -14.0f, 100, 230,  80 },
    { -17.0f,  30, 180, 240 },
    { -20.0f,  30,  80, 255 },
};


static const std::vector<ColorStop> rssi_colormap = {
    { -50.0f, 255,  40,  10 },
    { -65.0f, 255, 140,   0 },
    { -80.0f, 255, 215,   0 },
    { -95.0f, 100, 230,  80 },
    {-110.0f,  30, 180, 240 },
    {-120.0f,  30,  80, 255 },
};


static const std::vector<ColorStop> altitude_colormap = {
    { 200.0f, 255,  40,  10 },
    { 150.0f, 255, 200,   0 },
    { 100.0f, 100, 230,  80 },
    {  50.0f,  30, 180, 240 },
    {   0.0f,  30,  80, 255 },
};

static void colormap_lookup(const std::vector<ColorStop>& cmap,
                             float val,
                             unsigned char& r, unsigned char& g, unsigned char& b)
{
    if (cmap.empty()) { r = g = b = 128; return; }

    
    if (val >= cmap.front().val) { r = cmap.front().r; g = cmap.front().g; b = cmap.front().b; return; }
    
    if (val <= cmap.back().val)  { r = cmap.back().r;  g = cmap.back().g;  b = cmap.back().b;  return; }

    for (size_t i = 0; i + 1 < cmap.size(); i++)
    {
        if (val <= cmap[i].val && val >= cmap[i+1].val)
        {
            float t = (cmap[i].val - val) / (cmap[i].val - cmap[i+1].val);
            r = (unsigned char)(cmap[i].r + t * (cmap[i+1].r - cmap[i].r));
            g = (unsigned char)(cmap[i].g + t * (cmap[i+1].g - cmap[i].g));
            b = (unsigned char)(cmap[i].b + t * (cmap[i+1].b - cmap[i].b));
            return;
        }
    }
    r = cmap.back().r; g = cmap.back().g; b = cmap.back().b;
}




static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dlon/2)*sin(dlon/2);
    return 2.0 * R * atan2(sqrt(a), sqrt(1.0-a));
}


static void tilex2lon(int x, int z, double& lon)
{
    lon = (double)x / (1 << z) * 360.0 - 180.0;
}
static void tiley2lat(int y, int z, double& lat)
{
    double n = M_PI - 2.0 * M_PI * y / (1 << z);
    lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}


static bool idw_at(const std::vector<HeatmapPoint>& pts,
                   double lat, double lon,
                   float radius_m, float power,
                   HeatmapCriterion crit, int earfcn_filter,
                   float& out_val)
{
    double num = 0.0, den = 0.0;
    int    count = 0;

    for (auto& p : pts)
    {
        if (earfcn_filter != -1 && p.earfcn != earfcn_filter) continue;

        float val;
        switch (crit)
        {
            case HeatmapCriterion::RSRP:     val = p.rsrp;     break;
            case HeatmapCriterion::RSRQ:     val = p.rsrq;     break;
            case HeatmapCriterion::RSSI:     val = p.rssi;     break;
            case HeatmapCriterion::Altitude: val = p.altitude; break;
            default:                         val = p.rsrp;
        }

        
        if (crit == HeatmapCriterion::RSRP && val < -140.0f) continue;

        double dist = haversine_m(lat, lon, p.lat, p.lon);
        if (dist > radius_m) continue;
        if (dist < 1e-6)
        {
            out_val = val;
            return true;
        }
        double w = 1.0 / pow(dist, (double)power);
        num += w * val;
        den += w;
        count++;
    }

    if (count == 0 || den < 1e-12) return false;
    out_val = (float)(num / den);
    return true;
}


static void compute_heatmap_tile(int z, int tx, int ty,
                                 const std::vector<HeatmapPoint>& pts,
                                 HeatmapCriterion crit, int earfcn_filter,
                                 float radius_m, float power,
                                 unsigned char alpha_byte,
                                 std::vector<unsigned char>& out_img)
{
    const int SZ = 256;
    out_img.assign(SZ * SZ * 4, 0);

    
    double lon0, lon1, lat0, lat1;
    tilex2lon(tx,   z, lon0);
    tilex2lon(tx+1, z, lon1);
    tiley2lat(ty,   z, lat0); 
    tiley2lat(ty+1, z, lat1);

    const std::vector<ColorStop>* cmap = &rsrp_colormap;
    float no_signal_threshold = -200.0f;
    if (crit == HeatmapCriterion::RSRP)
    {
        cmap = &rsrp_colormap;
        no_signal_threshold = -140.0f;
    }
    else if (crit == HeatmapCriterion::RSRQ) cmap = &rsrq_colormap;
    else if (crit == HeatmapCriterion::RSSI) cmap = &rssi_colormap;
    else cmap = &altitude_colormap;

    double lat_lo = std::min(lat0, lat1);
    double lat_hi = std::max(lat0, lat1);
    double lon_lo = std::min(lon0, lon1);
    double lon_hi = std::max(lon0, lon1);
    double avg_lat = (lat_lo + lat_hi) * 0.5;

    double lat_radius = radius_m / 111320.0;
    double lon_radius = radius_m / (111320.0 * std::cos(avg_lat * M_PI / 180.0));

    lat_lo -= lat_radius;
    lat_hi += lat_radius;
    lon_lo -= lon_radius;
    lon_hi += lon_radius;

    std::vector<const HeatmapPoint*> candidates;
    candidates.reserve(1024);
    for (const auto& p : pts)
    {
        if (earfcn_filter != -1 && p.earfcn != earfcn_filter)
            continue;
        if (p.lat < lat_lo || p.lat > lat_hi || p.lon < lon_lo || p.lon > lon_hi)
            continue;
        candidates.push_back(&p);
    }

    if (candidates.empty())
        return;

    int pixel_count = 0;
    for (int py = 0; py < SZ; py++)
    {
        double lat = lat0 + (lat1 - lat0) * ((double)py / SZ);
        for (int px = 0; px < SZ; px++)
        {
            double lon = lon0 + (lon1 - lon0) * ((double)px / SZ);

            float num = 0.0f;
            float den = 0.0f;
            int count = 0;
            float val = 0.0f;
            for (const HeatmapPoint* p : candidates)
            {
                float pointVal;
                switch (crit)
                {
                    case HeatmapCriterion::RSRP:     pointVal = p->rsrp;     break;
                    case HeatmapCriterion::RSRQ:     pointVal = p->rsrq;     break;
                    case HeatmapCriterion::RSSI:     pointVal = p->rssi;     break;
                    case HeatmapCriterion::Altitude: pointVal = p->altitude; break;
                    default:                         pointVal = p->rsrp;
                }

                if (crit == HeatmapCriterion::RSRP && pointVal < -140.0f)
                    continue;

                double dist = haversine_m(lat, lon, p->lat, p->lon);
                if (dist > radius_m) continue;
                if (dist < 1e-6)
                {
                    val = pointVal;
                    count = 1;
                    break;
                }
                double w = 1.0 / std::pow(dist, (double)power);
                num += (float)(w * pointVal);
                den += (float)w;
                count++;
            }

            if (count == 0 || den < 1e-12f)
                continue;
            if (count > 1)
                val = num / den;

            if (crit == HeatmapCriterion::RSRP && val < no_signal_threshold)
                continue;

            unsigned char r, g, b;
            colormap_lookup(*cmap, val, r, g, b);

            int idx = (py * SZ + px) * 4;
            out_img[idx+0] = r;
            out_img[idx+1] = g;
            out_img[idx+2] = b;
            out_img[idx+3] = alpha_byte;
            pixel_count++;
        }
    }

    std::cout << "Heatmap tile z=" << z << " x=" << tx << " y=" << ty
              << " computed with " << pixel_count << " pixels\n";
}


static void request_heatmap_tile(int z, int tx, int ty,
                                  HeatmapCriterion crit, int earfcn_filter,
                                  float radius_m, float power, float alpha)
{
    std::string key = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);

    {
        std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
        auto& e = heatmap_tex_cache[key];
        if (e.computing) return;
        e.computing = true;
    }

    std::cout << "Heatmap tile request: " << key << "\n";

    
    std::vector<HeatmapPoint> pts_copy;
    {
        std::lock_guard<std::mutex> lk(heatmap_points_mutex);
        pts_copy = heatmap_points;
    }

    unsigned char alpha_byte = (unsigned char)(alpha * 255.0f);

    std::thread([z, tx, ty, key, pts_copy, crit, earfcn_filter, radius_m, power, alpha_byte]()
    {
        std::vector<unsigned char> img;
        compute_heatmap_tile(z, tx, ty, pts_copy, crit, earfcn_filter,
                             radius_m, power, alpha_byte, img);

        std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
        auto& e = heatmap_tex_cache[key];
        e.img = std::move(img);
        e.computing = false;
        e.dirty     = false;
    }).detach();
}



static GLuint get_heatmap_tile_texture(int z, int tx, int ty,
                                        HeatmapCriterion crit, int earfcn_filter,
                                        float radius_m, float power, float alpha)
{
    std::string key  = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);

    bool should_request = false;
    GLuint result = 0;
    {
        std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
        auto& e = heatmap_tex_cache[key];

        if (!e.img.empty())
        {
            int w = 256, h = 256;
            if (e.tex == 0)
                glGenTextures(1, &e.tex);
            glBindTexture(GL_TEXTURE_2D, e.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, e.img.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            e.img.clear();
            e.dirty = false;
            return e.tex;
        }

        if (e.tex != 0 && !e.dirty)
            return e.tex;

        if (!e.computing)
        {
            e.dirty = true;
            should_request = true;
        }

        result = e.dirty ? 0 : e.tex;
    }

    if (should_request)
    {
        std::cout << "Scheduling heatmap generation for " << key << "\n";
        request_heatmap_tile(z, tx, ty, crit, earfcn_filter, radius_m, power, alpha);
    }

    return result;
}


static void invalidate_heatmap_cache()
{
    std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
    for (auto& kv : heatmap_tex_cache)
    {
        kv.second.dirty = true;
        kv.second.img.clear();
    }
}



static void draw_heatmap_legend(ImDrawList* draw, ImVec2 map_start, ImVec2 map_size)
{
    const float LW = 24.0f, LH = 160.0f;
    float x0 = map_start.x + map_size.x - LW - 36.0f;
    float y0 = map_start.y + map_size.y - LH - 50.0f;

    const std::vector<ColorStop>* cmap = &rsrp_colormap;
    const char* unit = "dBm";
    if      (selected_criterion == HeatmapCriterion::RSRP) cmap = &rsrp_colormap;
    else if (selected_criterion == HeatmapCriterion::RSRQ) cmap = &rsrq_colormap;
    else if (selected_criterion == HeatmapCriterion::RSSI) cmap = &rssi_colormap;
    else { cmap = &altitude_colormap; unit = "m"; }

    
    int N = 64;
    float vmin = cmap->back().val;
    float vmax = cmap->front().val;
    for (int i = 0; i < N; i++)
    {
        float t  = (float)i / N;
        float v  = vmax + t * (vmin - vmax);
        float fy = y0 + t * LH;

        unsigned char r, g, b;
        colormap_lookup(*cmap, v, r, g, b);

        draw->AddRectFilled(
            ImVec2(x0, fy),
            ImVec2(x0 + LW, fy + LH / N + 1.0f),
            IM_COL32(r, g, b, 210));
    }

    
    draw->AddRect(ImVec2(x0, y0), ImVec2(x0+LW, y0+LH), IM_COL32(200,200,200,200));

    
    for (auto& cs : *cmap)
    {
        float t  = (cs.val - vmax) / (vmin - vmax);
        float fy = y0 + t * LH;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", cs.val);
        draw->AddLine(ImVec2(x0 + LW, fy), ImVec2(x0 + LW + 4, fy), IM_COL32(200,200,200,220));
        draw->AddText(ImVec2(x0 + LW + 6, fy - 6.0f), IM_COL32(220,220,220,255), buf);
    }

    
    char title[64];
    snprintf(title, sizeof(title), "%s (%s)", heatmap_criterion_names[(int)selected_criterion], unit);
    draw->AddText(ImVec2(x0 - 4.0f, y0 - 18.0f), IM_COL32(255,255,200,255), title);
}



void init_db()
{
    conn = PQconnectdb("host=localhost port=5432 dbname=network_monitor user=postgres password=1234");
    if (PQstatus(conn) != CONNECTION_OK)
    {
        std::cerr << "DB connection failed: " << PQerrorMessage(conn) << std::endl;
        exit(1);
    }
    std::cout << "Connected to PostgreSQL\n";
}

void insert_to_db(const nlohmann::json& json)
{
    if (!json.contains("networks")) return;

    for (auto& net : json["networks"])
    {
        std::string time = json.value("time", "");
        std::string lat  = std::to_string(json.value("latitude",  0.0));
        std::string lon  = std::to_string(json.value("longitude", 0.0));
        std::string alt  = std::to_string(json.value("altitude",  0.0));
        std::string acc  = std::to_string(json.value("accuracy",  0.0));
        std::string pci  = std::to_string(net.value("pci",   0));
        std::string rsrp = std::to_string(net.value("rsrp",  0));
        std::string rsrq = std::to_string(net.value("rsrq",  0));
        std::string rssi = std::to_string(net.value("rssi",  0));
        std::string sinr = std::to_string(net.value("rssnr", 0));

        const char* params[10] = {
            time.c_str(), lat.c_str(), lon.c_str(), alt.c_str(), acc.c_str(),
            pci.c_str(), rsrp.c_str(), rsrq.c_str(), rssi.c_str(), sinr.c_str()
        };

        PGresult* res = PQexecParams(conn,
            "INSERT INTO measurements(time, latitude, longitude, altitude, accuracy, pci, rsrp, rsrq, rssi, rssnr) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
            10, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "Insert error: " << PQerrorMessage(conn) << std::endl;

        PQclear(res);
    }
}

void load_from_json()
{
    std::ifstream file("location_log.json");
    if (!file.is_open()) return;

    std::string line;
    float history_time = 0.0f;

    while (std::getline(file, line))
    {
        if (line.empty()) continue;
        try
        {
            auto json = nlohmann::json::parse(line);

            if (json.contains("latitude") && json.contains("longitude"))
            {
                std::lock_guard<std::mutex> lock(gps_mutex);
                lat_values.push_back(json["latitude"]);
                lon_values.push_back(json["longitude"]);
            }

            if (json.contains("networks") && json["networks"].is_array())
            {
                bool has_data = false;
                for (auto& net : json["networks"])
                {
                    if (net.value("type", "") == "LTE")
                    {
                        int pci = net.value("pci", 0);
                        rsrp_values_map[pci].push_back((float)net.value("rsrp",  0));
                        rssi_values_map[pci].push_back((float)net.value("rssi",  0));
                        sinr_values_map[pci].push_back((float)net.value("rssnr", 0));
                        pci_values_map[pci].push_back((float)pci);
                        time_values_map[pci].push_back(history_time);
                        has_data = true;

                        
                        if (json.contains("latitude") && json.contains("longitude"))
                        {
                            HeatmapPoint hp;
                            hp.lat      = json["latitude"];
                            hp.lon      = json["longitude"];
                            hp.rsrp     = (float)net.value("rsrp",    0);
                            hp.rsrq     = (float)net.value("rsrq",    0);
                            hp.rssi     = (float)net.value("rssi",    0);
                            hp.altitude = (float)json.value("altitude",0.0);
                            hp.earfcn   = net.value("earfcn", 0);
                            std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                            heatmap_points.push_back(hp);
                        }
                    }
                }
                if (has_data) history_time += 1.0f;
            }
        }
        catch (...) {}
    }

    graph_time = history_time;
    std::cout << "Loaded " << lat_values.size() << " GPS points\n";
}


int long2tilex(double lon, int z) {
    return (int)((lon + 180.0) / 360.0 * (1 << z));
}
int lat2tiley(double lat, int z) {
    double r = lat * M_PI / 180.0;
    return (int)((1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (1 << z));
}

ImVec2 latlon_to_screen(double lat, double lon, int zoom,
                         int origin_tx, int origin_ty, ImVec2 map_start)
{
    double n  = (double)(1 << zoom);
    double xf = (lon + 180.0) / 360.0 * n;
    double r  = lat * M_PI / 180.0;
    double yf = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n;

    float px = (float)((xf - origin_tx) * 256.0);
    float py = (float)((yf - origin_ty) * 256.0);

    return ImVec2(map_start.x + px, map_start.y + py);
}


size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

std::string get_tile_path(int z, int x, int y) {
    return "tiles/" + std::to_string(z) + "/" +
           std::to_string(x) + "/" + std::to_string(y) + ".png";
}

void download_tile_async(int z, int x, int y)
{
    std::string path = get_tile_path(z, x, y);
    if (std::filesystem::exists(path)) return;

    std::string dir = "tiles/" + std::to_string(z) + "/" + std::to_string(x);
    std::filesystem::create_directories(dir);

    const char* servers[] = {"a", "b", "c"};
    std::string s   = servers[rand() % 3];
    std::string url = "https://" + s + ".basemaps.cartocdn.com/rastertiles/voyager/" +
                      std::to_string(z) + "/" + std::to_string(x) + "/" +
                      std::to_string(y) + ".png";

    CURL* curl = curl_easy_init();
    if (!curl) return;

    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); return; }

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,     "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        std::cerr << "CURL ERROR: " << curl_easy_strerror(res) << "\n";

    curl_easy_cleanup(curl);
    fclose(fp);
}

GLuint load_texture(const std::string& path)
{
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) 
    {
        std::cerr << "Failed to load texture: " << path << "\n";
        return 0;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    
    std::cout << "Loaded texture " << tex << " from " << path << " (size: " << w << "x" << h << ")\n";
    return tex;
}


GLuint get_or_request_tile(int z, int x, int y)
{
    std::string key  = std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
    std::string path = get_tile_path(z, x, y);

    std::lock_guard<std::mutex> lk(tile_cache_mutex);

    auto it = tile_texture_cache.find(key);
    if (it != tile_texture_cache.end() && it->second != 0)
        return it->second;

    if (std::filesystem::exists(path))
    {
        GLuint tex = load_texture(path);
        tile_texture_cache[key] = tex;
        return tex;
    }

    if (!tile_requested[key])
    {
        tile_requested[key]     = true;
        tile_texture_cache[key] = 0;
        std::thread([z, x, y](){ download_tile_async(z, x, y); }).detach();
    }

    return 0;
}

void run_server(location* loc)
{
    zmq::context_t ctx(1);
    zmq::socket_t  socket(ctx, ZMQ_REP);
    socket.bind("tcp://0.0.0.0:5555");
    std::cout << "SERVER STARTED ON PORT 5555\n";

    while (true)
    {
        zmq::message_t request;
        auto result = socket.recv(request, zmq::recv_flags::none);
        if (!result) continue;

        std::string data(static_cast<char*>(request.data()), request.size());
        std::cout << "RECEIVED: " << data << std::endl;

        try
        {
            auto json = nlohmann::json::parse(data);
            insert_to_db(json);

            {
                std::lock_guard<std::mutex> lock(loc_mutex);
                if (json.contains("latitude"))  loc->latitude  = json["latitude"];
                if (json.contains("longitude")) loc->longitude = json["longitude"];
                if (json.contains("altitude"))  loc->altitude  = json["altitude"];
                if (json.contains("accuracy"))  loc->accuracy  = json["accuracy"];
                if (json.contains("time"))      loc->time      = json["time"];

                loc->lte.clear(); loc->gsm.clear(); loc->nr.clear();
                if (json.contains("networks") && json["networks"].is_array())
                {
                    for (auto& net : json["networks"])
                    {
                        std::string type = net.value("type", "");
                        if      (type == "LTE") loc->lte.push_back(net);
                        else if (type == "GSM") loc->gsm.push_back(net);
                        else if (type == "NR")  loc->nr.push_back(net);

                        
                        if (type == "LTE" &&
                            json.contains("latitude") && json.contains("longitude"))
                        {
                            HeatmapPoint hp;
                            hp.lat      = json["latitude"];
                            hp.lon      = json["longitude"];
                            hp.rsrp     = (float)net.value("rsrp",    0);
                            hp.rsrq     = (float)net.value("rsrq",    0);
                            hp.rssi     = (float)net.value("rssi",    0);
                            hp.altitude = (float)json.value("altitude",0.0);
                            hp.earfcn   = net.value("earfcn", 0);
                            {
                                std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                                heatmap_points.push_back(hp);
                            }
                            invalidate_heatmap_cache();
                        }
                    }
                }
            }

            std::ofstream file("location_log.json", std::ios::app);
            file << json.dump() << std::endl;
            socket.send(zmq::buffer("OK"),    zmq::send_flags::none);
        }
        catch (...)
        {
            socket.send(zmq::buffer("ERROR"), zmq::send_flags::none);
        }
    }
}

void run_gui(location* loc)
{
    
    std::filesystem::create_directories("heatmap");
    for (int z = 3; z <= 18; z++)
        std::filesystem::create_directories("heatmap/" + std::to_string(z));

    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "Smartphone Network & GPS Monitor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 750,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    bool done = false;

    
    static HeatmapCriterion prev_criterion = HeatmapCriterion::RSRP;
    static int   prev_earfcn   = -1;
    static float prev_radius   = 150.0f;
    static float prev_alpha    = 0.65f;

    while (!done)
    {
        if (!json_loaded) { load_from_json(); json_loaded = true; }

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        float lat, lon, alt, acc;
        std::string time_str;
        std::vector<nlohmann::json> lte_copy, gsm_copy, nr_copy;

        {
            std::lock_guard<std::mutex> lock(loc_mutex);
            lat      = loc->latitude;
            lon      = loc->longitude;
            alt      = loc->altitude;
            acc      = loc->accuracy;
            time_str = loc->time;
            lte_copy = loc->lte;
            gsm_copy = loc->gsm;
            nr_copy  = loc->nr;
        }

        
        ImGui::Begin("Smartphone Monitor");

        ImGui::SeparatorText("GPS DATA");
        ImGui::Text("Latitude:  %.6f", lat);
        ImGui::Text("Longitude: %.6f", lon);
        ImGui::Text("Altitude:  %.2f m", alt);
        ImGui::Text("Accuracy:  %.2f m", acc);
        ImGui::Text("Time: %s", time_str.c_str());

        static float last_lat = 0.0f, last_lon = 0.0f;
        if (fabs(lat - last_lat) > 0.00001f || fabs(lon - last_lon) > 0.00001f)
        {
            std::lock_guard<std::mutex> lock(gps_mutex);
            lat_values.push_back(lat);
            lon_values.push_back(lon);
            last_lat = lat; last_lon = lon;
        }

        ImGui::Spacing();
        ImGui::SeparatorText("LTE NETWORKS");

        if (!lte_copy.empty())
        {
            for (auto& lte : lte_copy)
            {
                int pci   = lte.value("pci",   0);
                int rsrp  = lte.value("rsrp",  0);
                int rssi  = lte.value("rssi",  0);
                int rssnr = lte.value("rssnr", 0);

                ImGui::Text("CI: %d",     lte.value("ci",    0));
                ImGui::Text("EARFCN: %d", lte.value("earfcn",0));
                ImGui::Text("PCI: %d",    pci);
                ImGui::Text("TAC: %d",    lte.value("tac",   0));
                ImGui::Text("RSRP: %d",   rsrp);
                ImGui::Text("RSRQ: %d",   lte.value("rsrq",  0));
                ImGui::Text("RSSI: %d",   rssi);
                ImGui::Text("RSSNR: %d",  rssnr);

                rsrp_values_map[pci].push_back((float)rsrp);
                rssi_values_map[pci].push_back((float)rssi);
                sinr_values_map[pci].push_back((float)rssnr);
                pci_values_map[pci].push_back((float)pci);
                time_values_map[pci].push_back(graph_time);

                if (rsrp_values_map[pci].size() > 200)
                {
                    rsrp_values_map[pci].erase(rsrp_values_map[pci].begin());
                    rssi_values_map[pci].erase(rssi_values_map[pci].begin());
                    sinr_values_map[pci].erase(sinr_values_map[pci].begin());
                    pci_values_map[pci].erase(pci_values_map[pci].begin());
                    time_values_map[pci].erase(time_values_map[pci].begin());
                }
                ImGui::Separator();
            }
            graph_time++;
        }
        else
            ImGui::Text("No LTE data received");


        ImGui::Spacing();
        ImGui::SeparatorText("HEATMAP SETTINGS");

        ImGui::Checkbox("Show Heatmap", &heatmap_enabled);
        ImGui::Checkbox("Show GPS Track", &gps_track_enabled);

        
        {
            int crit_idx = (int)selected_criterion;
            if (ImGui::Combo("Criterion", &crit_idx, heatmap_criterion_names, 4))
            {
                selected_criterion = (HeatmapCriterion)crit_idx;
                invalidate_heatmap_cache();
            }
        }

        
        {
            
            std::vector<int> earfcns;
            earfcns.push_back(-1);
            {
                std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                for (auto& p : heatmap_points)
                    if (std::find(earfcns.begin(), earfcns.end(), p.earfcn) == earfcns.end())
                        earfcns.push_back(p.earfcn);
            }

            
            int sel_idx = 0;
            for (int i = 0; i < (int)earfcns.size(); i++)
                if (earfcns[i] == selected_earfcn) { sel_idx = i; break; }

            
            std::vector<std::string> labels;
            std::vector<const char*> clabels;
            for (int e : earfcns)
            {
                if (e == -1) labels.push_back("All");
                else         labels.push_back("EARFCN " + std::to_string(e));
            }
            for (auto& s : labels) clabels.push_back(s.c_str());

            if (ImGui::Combo("EARFCN", &sel_idx, clabels.data(), (int)clabels.size()))
            {
                selected_earfcn = earfcns[sel_idx];
                invalidate_heatmap_cache();
            }
        }

        
        if (ImGui::SliderFloat("IDW Radius (m)", &idw_radius_m, 50.0f, 1000.0f))
            invalidate_heatmap_cache();

        
        if (ImGui::SliderFloat("Heatmap Alpha", &heatmap_alpha, 0.1f, 1.0f))
            invalidate_heatmap_cache();

        
        {
            std::lock_guard<std::mutex> lk(heatmap_points_mutex);
            ImGui::Text("Heatmap points: %zu", heatmap_points.size());
        }

        ImGui::End();

        
        ImGui::Begin("Map");

        static int zoom = 14;
        ImGui::SliderInt("Zoom", &zoom, 3, 18);

        float  cur_lat, cur_lon;
        size_t gps_size;
        {
            std::lock_guard<std::mutex> lock(gps_mutex);
            gps_size = lat_values.size();
            cur_lat  = gps_size > 0 ? lat_values.back() : 0.0f;
            cur_lon  = gps_size > 0 ? lon_values.back() : 0.0f;
        }

        if (gps_size == 0)
        {
            ImGui::Text("No GPS data");
        }
        else
        {
            ImGui::Text("Position: %.6f  %.6f", cur_lat, cur_lon);

            int cx = long2tilex(cur_lon, zoom);
            int cy = lat2tiley(cur_lat, zoom);

            const int   HALF    = 1;
            const float TILE_PX = 256.0f;
            const int   GRID    = 2*HALF + 1;

            int origin_tx = cx - HALF;
            int origin_ty = cy - HALF;

            ImVec2 map_start = ImGui::GetCursorScreenPos();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            
            for (int dy = -HALF; dy <= HALF; dy++)
            {
                for (int dx = -HALF; dx <= HALF; dx++)
                {
                    int tx = cx + dx;
                    int ty = cy + dy;

                    GLuint tex = get_or_request_tile(zoom, tx, ty);

                    float sx = map_start.x + (dx + HALF) * TILE_PX;
                    float sy = map_start.y + (dy + HALF) * TILE_PX;

                    if (tex)
                        draw->AddImage((void*)(intptr_t)tex,
                            ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX));
                    else
                        draw->AddRectFilled(
                            ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX),
                            IM_COL32(50, 50, 50, 255));
                }
            }

            
            if (heatmap_enabled)
            {
                size_t hp_count;
                {
                    std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                    hp_count = heatmap_points.size();
                }
                if (hp_count > 0)
                {
                    std::cout << "Rendering heatmap with " << hp_count << " points\n";
                    for (int dy = -HALF; dy <= HALF; dy++)
                    {
                        for (int dx = -HALF; dx <= HALF; dx++)
                        {
                            int tx = cx + dx;
                            int ty = cy + dy;

                            GLuint htex = get_heatmap_tile_texture(
                                zoom, tx, ty,
                                selected_criterion, selected_earfcn,
                                idw_radius_m, idw_power, heatmap_alpha);

                            float sx = map_start.x + (dx + HALF) * TILE_PX;
                            float sy = map_start.y + (dy + HALF) * TILE_PX;

                            if (htex != 0)
                            {
                                draw->AddImage((void*)(intptr_t)htex,
                                    ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX),
                                    ImVec2(0,0), ImVec2(1,1),
                                    IM_COL32(255,255,255,(unsigned char)(heatmap_alpha * 255)));
                            }
                        }
                    }

                    
                    ImVec2 map_size = ImVec2(GRID * TILE_PX, GRID * TILE_PX);
                    draw_heatmap_legend(draw, map_start, map_size);
                }
            }

            
            {
                std::lock_guard<std::mutex> lock(gps_mutex);

                if (gps_track_enabled)
                {
                    for (size_t i = 1; i < lat_values.size(); i++)
                    {
                        ImVec2 p1 = latlon_to_screen(lat_values[i-1], lon_values[i-1],
                                                      zoom, origin_tx, origin_ty, map_start);
                        ImVec2 p2 = latlon_to_screen(lat_values[i],   lon_values[i],
                                                      zoom, origin_tx, origin_ty, map_start);
                        draw->AddLine(p1, p2, IM_COL32(30, 140, 255, 120), 1.5f);
                    }
                }

                if (!lat_values.empty())
                {
                    ImVec2 cur_screen = latlon_to_screen(cur_lat, cur_lon,
                                                          zoom, origin_tx, origin_ty, map_start);
                    draw->AddCircleFilled(cur_screen, 9.0f, IM_COL32(255, 255, 255, 255));
                    draw->AddCircleFilled(cur_screen, 6.0f, IM_COL32(220, 40,  40,  255));
                }
            }

            ImGui::Dummy(ImVec2(GRID * TILE_PX, GRID * TILE_PX));
        }

        ImGui::End();

       
        ImGui::Begin("Signal Graph");

        if (ImPlot::BeginPlot("LTE RSRP")) {
            for (auto& p : rsrp_values_map) {
                std::string lbl = "PCI " + std::to_string(p.first);
                ImPlot::PlotLine(lbl.c_str(), time_values_map[p.first].data(), p.second.data(), (int)p.second.size());
            }
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("LTE RSSI")) {
            for (auto& p : rssi_values_map) {
                std::string lbl = "PCI " + std::to_string(p.first);
                ImPlot::PlotLine(lbl.c_str(), time_values_map[p.first].data(), p.second.data(), (int)p.second.size());
            }
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("LTE SINR")) {
            for (auto& p : sinr_values_map) {
                std::string lbl = "PCI " + std::to_string(p.first);
                ImPlot::PlotLine(lbl.c_str(), time_values_map[p.first].data(), p.second.data(), (int)p.second.size());
            }
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("LTE PCI Activity")) {
            for (auto& p : pci_values_map) {
                std::string lbl = "PCI " + std::to_string(p.first);
                ImPlot::PlotLine(lbl.c_str(), time_values_map[p.first].data(), p.second.data(), (int)p.second.size());
            }
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("GPS Track")) {
            std::lock_guard<std::mutex> lock(gps_mutex);
            if (!lat_values.empty())
                ImPlot::PlotLine("Path", lon_values.data(), lat_values.data(), (int)lat_values.size());
            ImPlot::EndPlot();
        }

        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main()
{
    init_db();
    static location locationInfo;

    std::thread server_thread(run_server, &locationInfo);
    std::thread gui_thread(run_gui, &locationInfo);

    server_thread.detach();
    gui_thread.join();

    PQfinish(conn);
    return 0;
}
