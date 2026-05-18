#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <initializer_list>
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

#ifndef PROJECT_SOURCE_DIR
#define PROJECT_SOURCE_DIR "."
#endif



struct location {
    float latitude = 0, longitude = 0, altitude = 0, accuracy = 0;
    std::string time = "no data";
    std::vector<nlohmann::json> lte, gsm, nr;
};

struct HeatmapPoint {
    double lat, lon;
    float  rsrp, rsrq, rssi, altitude;
    int    earfcn;
};

struct HeatmapTileEntry {
    GLuint tex       = 0;
    bool   dirty     = true;
    bool   computing = false;
    std::vector<unsigned char> img;
};

struct ColorStop { float val; unsigned char r, g, b; };

enum class HeatmapCriterion { RSRP = 0, RSRQ, RSSI, Altitude };
static const char* heatmap_criterion_names[] = { "RSRP", "RSRQ", "RSSI", "Altitude" };



static PGconn* conn;

static std::mutex loc_mutex;
static std::mutex gps_mutex;
static std::mutex tile_cache_mutex;
static std::mutex heatmap_points_mutex;
static std::mutex heatmap_tex_cache_mutex;

static std::map<int, std::vector<float>> rsrp_values_map, rssi_values_map,
                                          sinr_values_map, pci_values_map, time_values_map;
static float graph_time = 0.0f;

static std::vector<float> lat_values, lon_values;
static std::vector<HeatmapPoint> heatmap_points;

static std::map<std::string, GLuint>          tile_texture_cache;
static std::map<std::string, bool>            tile_requested;
static std::map<std::string, HeatmapTileEntry> heatmap_tex_cache;

static bool json_loaded = false;

static HeatmapCriterion selected_criterion = HeatmapCriterion::RSRP;
static int   selected_earfcn = -1;
static bool  heatmap_enabled = true;
static bool  gps_track_enabled = false;
static float heatmap_alpha  = 0.65f;
static float idw_radius_m   = 50.0f;
static float idw_power      = 2.0f;

static const char* kArchiveLogPath      = "archive.json";
static const char* kDriveTestLogPath    = "drive_test_log.json";
static const char* kLegacyLocationLogPath = "location_log.json";



static const std::vector<ColorStop> rsrp_colormap = {
    { -80.f, 255, 40, 10 }, { -85.f, 255,120,  0 }, { -90.f, 255,215,  0 },
    { -95.f, 100,230, 80 }, {-100.f,  30,180,240 }, {-105.f,  30, 80,255 },
    {-120.f, 120, 20,180 }, {-130.f, 180, 30,180 },
};
static const std::vector<ColorStop> rsrq_colormap = {
    {  -3.f, 255, 40, 10 }, {  -7.f, 255,140,  0 }, {-10.f, 255,215,  0 },
    { -14.f, 100,230, 80 }, { -17.f,  30,180,240 }, {-20.f,  30, 80,255 },
};
static const std::vector<ColorStop> rssi_colormap = {
    { -50.f, 255, 40, 10 }, { -65.f, 255,140,  0 }, { -80.f, 255,215,  0 },
    { -95.f, 100,230, 80 }, {-110.f,  30,180,240 }, {-120.f,  30, 80,255 },
};
static const std::vector<ColorStop> altitude_colormap = {
    {200.f, 255, 40, 10 }, {150.f, 255,200,  0 },
    {100.f, 100,230, 80 }, { 50.f,  30,180,240 }, {0.f, 30, 80,255 },
};

static void colormap_lookup(const std::vector<ColorStop>& cmap, float val,
                             unsigned char& r, unsigned char& g, unsigned char& b)
{
    if (cmap.empty()) { r = g = b = 128; return; }
    if (val >= cmap.front().val) { r=cmap.front().r; g=cmap.front().g; b=cmap.front().b; return; }
    if (val <= cmap.back().val)  { r=cmap.back().r;  g=cmap.back().g;  b=cmap.back().b;  return; }
    for (size_t i = 0; i+1 < cmap.size(); i++) {
        if (val <= cmap[i].val && val >= cmap[i+1].val) {
            float t = (cmap[i].val - val) / (cmap[i].val - cmap[i+1].val);
            r = (unsigned char)(cmap[i].r + t*(cmap[i+1].r - cmap[i].r));
            g = (unsigned char)(cmap[i].g + t*(cmap[i+1].g - cmap[i].g));
            b = (unsigned char)(cmap[i].b + t*(cmap[i+1].b - cmap[i].b));
            return;
        }
    }
    r=cmap.back().r; g=cmap.back().g; b=cmap.back().b;
}



static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2-lat1)*M_PI/180.0, dlon = (lon2-lon1)*M_PI/180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dlon/2)*sin(dlon/2);
    return 2.0*R*atan2(sqrt(a), sqrt(1.0-a));
}

static double lon_to_tilex(double lon, int z) { return (lon+180.0)/360.0*(1<<z); }
static double lat_to_tiley(double lat, int z) {
    double r = lat*M_PI/180.0;
    return (1.0 - log(tan(r)+1.0/cos(r))/M_PI)/2.0*(1<<z);
}
static void tilex2lon(int x, int z, double& lon) { lon = (double)x/(1<<z)*360.0-180.0; }
static void tiley2lat(int y, int z, double& lat) {
    double n = M_PI - 2.0*M_PI*y/(1<<z);
    lat = 180.0/M_PI*atan(0.5*(exp(n)-exp(-n)));
}
static int long2tilex(double lon, int z) { return (int)lon_to_tilex(lon,z); }
static int lat2tiley(double lat, int z)  { return (int)lat_to_tiley(lat,z); }

static ImVec2 latlon_to_screen(double lat, double lon, int zoom,
                                double tl_tx, double tl_ty, ImVec2 map_start)
{
    return { map_start.x + (float)((lon_to_tilex(lon,zoom)-tl_tx)*256.0),
             map_start.y + (float)((lat_to_tiley(lat,zoom)-tl_ty)*256.0) };
}



static void compute_heatmap_tile(int z, int tx, int ty,
                                  const std::vector<HeatmapPoint>& pts,
                                  HeatmapCriterion crit, int earfcn_filter,
                                  float radius_m, float power, unsigned char alpha_byte,
                                  std::vector<unsigned char>& out_img)
{
    const int SZ = 256;
    out_img.assign(SZ*SZ*4, 0);

    double lon0, lon1, lat0, lat1;
    tilex2lon(tx,   z, lon0); tilex2lon(tx+1, z, lon1);
    tiley2lat(ty,   z, lat0); tiley2lat(ty+1, z, lat1);

    const std::vector<ColorStop>* cmap = &rsrp_colormap;
    float no_signal_threshold = -200.0f;
    if      (crit == HeatmapCriterion::RSRP) { cmap = &rsrp_colormap; no_signal_threshold = -140.0f; }
    else if (crit == HeatmapCriterion::RSRQ)   cmap = &rsrq_colormap;
    else if (crit == HeatmapCriterion::RSSI)   cmap = &rssi_colormap;
    else                                        cmap = &altitude_colormap;

    double lat_lo = std::min(lat0,lat1), lat_hi = std::max(lat0,lat1);
    double lon_lo = std::min(lon0,lon1), lon_hi = std::max(lon0,lon1);
    double avg_lat = (lat_lo+lat_hi)*0.5;
    double lat_rad = radius_m/111320.0;
    double lon_rad = radius_m/(111320.0*std::cos(avg_lat*M_PI/180.0));
    lat_lo -= lat_rad; lat_hi += lat_rad; lon_lo -= lon_rad; lon_hi += lon_rad;

    std::vector<const HeatmapPoint*> candidates;
    for (const auto& p : pts) {
        if (earfcn_filter != -1 && p.earfcn != earfcn_filter) continue;
        if (p.lat < lat_lo || p.lat > lat_hi || p.lon < lon_lo || p.lon > lon_hi) continue;
        candidates.push_back(&p);
    }
    if (candidates.empty()) return;

    int pixel_count = 0;
    for (int py = 0; py < SZ; py++) {
        double lat = lat0 + (lat1-lat0)*((double)py/SZ);
        for (int px = 0; px < SZ; px++) {
            double lon = lon0 + (lon1-lon0)*((double)px/SZ);
            float num = 0, den = 0, val = 0; int count = 0;

            for (const HeatmapPoint* p : candidates) {
                float pv = (crit==HeatmapCriterion::RSRP) ? p->rsrp :
                           (crit==HeatmapCriterion::RSRQ) ? p->rsrq :
                           (crit==HeatmapCriterion::RSSI) ? p->rssi : p->altitude;
                if (crit == HeatmapCriterion::RSRP && pv < -140.0f) continue;
                double dist = haversine_m(lat,lon,p->lat,p->lon);
                if (dist > radius_m) continue;
                if (dist < 1e-6) { val=pv; count=1; break; }
                double w = 1.0/std::pow(dist,(double)power);
                num += (float)(w*pv); den += (float)w; count++;
            }
            if (count == 0 || den < 1e-12f) continue;
            if (count > 1) val = num/den;
            if (crit == HeatmapCriterion::RSRP && val < no_signal_threshold) continue;

            unsigned char r,g,b;
            colormap_lookup(*cmap, val, r, g, b);
            int idx = (py*SZ+px)*4;
            out_img[idx]=r; out_img[idx+1]=g; out_img[idx+2]=b; out_img[idx+3]=alpha_byte;
            pixel_count++;
        }
    }
    std::cout << "Heatmap tile z=" << z << " x=" << tx << " y=" << ty
              << " computed with " << pixel_count << " pixels\n";
}

static void invalidate_heatmap_cache()
{
    std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
    for (auto& kv : heatmap_tex_cache) { kv.second.dirty=true; kv.second.img.clear(); }
}

static void request_heatmap_tile(int z, int tx, int ty,
                                  HeatmapCriterion crit, int earfcn_filter,
                                  float radius_m, float power, float alpha)
{
    std::string key = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);
    { std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
      auto& e = heatmap_tex_cache[key];
      if (e.computing) return;
      e.computing = true; }

    std::vector<HeatmapPoint> pts_copy;
    { std::lock_guard<std::mutex> lk(heatmap_points_mutex); pts_copy = heatmap_points; }

    unsigned char alpha_byte = (unsigned char)(alpha*255.0f);
    std::thread([z,tx,ty,key,pts_copy,crit,earfcn_filter,radius_m,power,alpha_byte]() {
        std::vector<unsigned char> img;
        compute_heatmap_tile(z,tx,ty,pts_copy,crit,earfcn_filter,radius_m,power,alpha_byte,img);
        std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
        auto& e = heatmap_tex_cache[key];
        e.img = std::move(img); e.computing = false; e.dirty = false;
    }).detach();
}

static GLuint get_heatmap_tile_texture(int z, int tx, int ty,
                                        HeatmapCriterion crit, int earfcn_filter,
                                        float radius_m, float power, float alpha)
{
    std::string key = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);
    bool should_request = false;
    GLuint result = 0;
    {
        std::lock_guard<std::mutex> lk(heatmap_tex_cache_mutex);
        auto& e = heatmap_tex_cache[key];

        if (!e.img.empty()) {
            if (e.tex == 0) glGenTextures(1, &e.tex);
            glBindTexture(GL_TEXTURE_2D, e.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, e.img.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            e.img.clear(); e.dirty = false;
            return e.tex;
        }
        if (e.tex != 0 && !e.dirty) return e.tex;
        if (!e.computing) { e.dirty = true; should_request = true; }
        result = e.dirty ? 0 : e.tex;
    }
    if (should_request)
        request_heatmap_tile(z,tx,ty,crit,earfcn_filter,radius_m,power,alpha);
    return result;
}

static void draw_heatmap_legend(ImDrawList* draw, ImVec2 legend_start)
{
    const float LW=24.0f, LH=160.0f;
    float x0=legend_start.x, y0=legend_start.y+24.0f;

    const std::vector<ColorStop>* cmap = &rsrp_colormap;
    const char* unit = "dBm";
    if      (selected_criterion == HeatmapCriterion::RSRQ) cmap = &rsrq_colormap;
    else if (selected_criterion == HeatmapCriterion::RSSI) cmap = &rssi_colormap;
    else if (selected_criterion == HeatmapCriterion::Altitude) { cmap = &altitude_colormap; unit = "m"; }

    float vmin = cmap->back().val, vmax = cmap->front().val;
    int N = 64;
    for (int i = 0; i < N; i++) {
        float t=((float)i/N), v=vmax+t*(vmin-vmax), fy=y0+t*LH;
        unsigned char r,g,b; colormap_lookup(*cmap,v,r,g,b);
        draw->AddRectFilled(ImVec2(x0,fy), ImVec2(x0+LW, fy+LH/N+1.0f), IM_COL32(r,g,b,210));
    }
    draw->AddRect(ImVec2(x0,y0), ImVec2(x0+LW,y0+LH), IM_COL32(200,200,200,200));
    for (auto& cs : *cmap) {
        float fy = y0 + (cs.val-vmax)/(vmin-vmax)*LH;
        char buf[32]; snprintf(buf,sizeof(buf),"%.0f",cs.val);
        draw->AddLine(ImVec2(x0+LW,fy), ImVec2(x0+LW+4,fy), IM_COL32(200,200,200,220));
        draw->AddText(ImVec2(x0+LW+6,fy-6.0f), IM_COL32(220,220,220,255), buf);
    }
    char title[64];
    snprintf(title,sizeof(title),"%s (%s)",heatmap_criterion_names[(int)selected_criterion],unit);
    draw->AddText(ImVec2(x0, legend_start.y), IM_COL32(255,255,200,255), title);
}



void init_db()
{
    conn = PQconnectdb("host=localhost port=5432 dbname=network_monitor user=postgres password=1234");
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connection failed: " << PQerrorMessage(conn) << std::endl;
        exit(1);
    }
    std::cout << "Connected to PostgreSQL\n";
}

bool database_has_data()
{
    PGresult* res = PQexec(conn, "SELECT COUNT(*) FROM measurements");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) { PQclear(res); return false; }
    int count = atoi(PQgetvalue(res,0,0)); PQclear(res);
    return count > 0;
}



static const nlohmann::json* find_json_key(const nlohmann::json& obj,
                                            std::initializer_list<const char*> keys)
{
    if (!obj.is_object()) return nullptr;
    for (const char* k : keys) { auto it = obj.find(k); if (it!=obj.end()) return &(*it); }
    return nullptr;
}

static int int_value_any(const nlohmann::json& obj,
                          std::initializer_list<const char*> keys, int def=0)
{
    const nlohmann::json* v = find_json_key(obj,keys); if (!v) return def;
    try {
        if (v->is_number_integer()) return v->get<int>();
        if (v->is_number())         return (int)v->get<double>();
        if (v->is_string())         return std::stoi(v->get<std::string>());
    } catch(...) {}
    return def;
}

static double double_value_any(const nlohmann::json& obj,
                                std::initializer_list<const char*> keys, double def=0.0)
{
    const nlohmann::json* v = find_json_key(obj,keys); if (!v) return def;
    try {
        if (v->is_number()) return v->get<double>();
        if (v->is_string()) return std::stod(v->get<std::string>());
    } catch(...) {}
    return def;
}

static std::string string_value_any(const nlohmann::json& obj,
                                     std::initializer_list<const char*> keys,
                                     const std::string& def="")
{
    const nlohmann::json* v = find_json_key(obj,keys); if (!v) return def;
    try {
        if (v->is_string())          return v->get<std::string>();
        if (v->is_number_integer())  return std::to_string(v->get<long long>());
        if (v->is_number_float())    return std::to_string(v->get<double>());
    } catch(...) {}
    return def;
}



size_t insert_to_db(const nlohmann::json& json)
{
    if (!json.contains("networks")) return 0;
    size_t inserted = 0;

    for (auto& net : json["networks"]) {
        std::string type      = net.value("type","");
        std::string time      = json.value("time","");
        std::string latitude  = std::to_string(json.value("latitude",0.0));
        std::string longitude = std::to_string(json.value("longitude",0.0));
        std::string altitude  = std::to_string(json.value("altitude",0.0));
        std::string accuracy  = std::to_string(json.value("accuracy",0.0));

        auto si = [&](std::initializer_list<const char*> k){ return std::to_string(int_value_any(net,k)); };
        auto ss = [&](std::initializer_list<const char*> k){ return string_value_any(net,k); };

        const char* params[36] = {
            time.c_str(), latitude.c_str(), longitude.c_str(), altitude.c_str(), accuracy.c_str(),
            type.c_str(),
            ss({"band"}).c_str(), si({"ci"}).c_str(), si({"earfcn"}).c_str(),
            ss({"mcc"}).c_str(), ss({"mnc"}).c_str(), si({"pci"}).c_str(),
            si({"tac"}).c_str(), si({"asu"}).c_str(), si({"cqi"}).c_str(),
            si({"rsrp"}).c_str(), si({"rsrq"}).c_str(), si({"rssi"}).c_str(),
            si({"rssnr"}).c_str(), si({"timingAdvance"}).c_str(),
            si({"cid"}).c_str(), si({"bsic"}).c_str(), si({"arfcn"}).c_str(),
            si({"lac"}).c_str(), ss({"mcc"}).c_str(), ss({"mnc"}).c_str(),
            si({"psc"}).c_str(), si({"dbm"}).c_str(), si({"rssi"}).c_str(),
            si({"timingAdvance"}).c_str(),
            ss({"band"}).c_str(), si({"nci"}).c_str(), si({"pci"}).c_str(),
            si({"nrarfcn"}).c_str(), si({"tac"}).c_str(), ss({"mcc"}).c_str()
        };

        
        std::string p[36];
        p[0]=time; p[1]=latitude; p[2]=longitude; p[3]=altitude; p[4]=accuracy;
        p[5]=type;
        p[6]=ss({"band"});    p[7]=si({"ci"});           p[8]=si({"earfcn"});
        p[9]=ss({"mcc"});     p[10]=ss({"mnc"});          p[11]=si({"pci"});
        p[12]=si({"tac"});    p[13]=si({"asu"});           p[14]=si({"cqi"});
        p[15]=si({"rsrp"});   p[16]=si({"rsrq"});          p[17]=si({"rssi"});
        p[18]=si({"rssnr"});  p[19]=si({"timingAdvance"});
        p[20]=si({"cid"});    p[21]=si({"bsic"});          p[22]=si({"arfcn"});
        p[23]=si({"lac"});    p[24]=ss({"mcc"});            p[25]=ss({"mnc"});
        p[26]=si({"psc"});    p[27]=si({"dbm"});            p[28]=si({"rssi"});
        p[29]=si({"timingAdvance"});
        p[30]=ss({"band"});   p[31]=si({"nci"});            p[32]=si({"pci"});
        p[33]=si({"nrarfcn"});p[34]=si({"tac"});            p[35]=ss({"mcc"});

        const char* cparams[36];
        for (int i=0;i<36;i++) cparams[i]=p[i].c_str();

        PGresult* res = PQexecParams(conn,
            "INSERT INTO measurements ("
            "time,latitude,longitude,altitude,accuracy,network_type,"
            "lte_band,lte_ci,lte_earfcn,lte_mcc,lte_mnc,lte_pci,lte_tac,"
            "lte_asu_level,lte_cqi,lte_rsrp,lte_rsrq,lte_rssi,lte_rssnr,lte_timing_advance,"
            "gsm_cid,gsm_bsic,gsm_arfcn,gsm_lac,gsm_mcc,gsm_mnc,gsm_psc,"
            "gsm_dbm,gsm_rssi,gsm_timing_advance,"
            "nr_band,nr_nci,nr_pci,nr_nrarfcn,nr_tac,nr_mcc"
            ") VALUES ("
            "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,"
            "$16,$17,$18,$19,$20,$21,$22,$23,$24,$25,$26,$27,$28,$29,$30,"
            "$31,$32,$33,$34,$35,$36)",
            36, NULL, cparams, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "Insert error: " << PQerrorMessage(conn) << std::endl;
        else
            inserted++;
        PQclear(res);
    }
    return inserted;
}



static nlohmann::json normalize_measurement_json(const nlohmann::json& raw)
{
    if (raw.contains("latitude") || raw.contains("networks")) return raw;

    nlohmann::json norm;

    if (raw.contains("location") && raw["location"].is_object()) {
        const auto& loc = raw["location"];
        norm["latitude"]  = loc.value("Latitude",  loc.value("latitude",  0.0));
        norm["longitude"] = loc.value("Longitude", loc.value("longitude", 0.0));
        norm["altitude"]  = loc.value("Altitude",  loc.value("altitude",  0.0));
        norm["accuracy"]  = loc.value("Accuracy",  loc.value("accuracy",  0.0));
        norm["time"]      = loc.value("Current Time", loc.value("time", ""));
    }
    if (!norm.contains("time") && raw.contains("timestamp"))
        norm["time"] = raw["timestamp"];

    nlohmann::json networks = nlohmann::json::array();

    if (raw.contains("telephony")) {
        nlohmann::json tel_arr;
        if (raw["telephony"].is_array())       tel_arr = raw["telephony"];
        else if (raw["telephony"].is_object()) tel_arr.push_back(raw["telephony"]);

        for (const auto& cell : tel_arr) {
            std::string type = cell.value("Type", cell.value("type",""));
            if      (type == "CellInfoLte") type = "LTE";
            else if (type == "CellInfoGsm") type = "GSM";
            else if (type == "CellInfoNr")  type = "NR";

            nlohmann::json net; net["type"] = type;

            if (type == "LTE") {
                const auto id  = cell.value("CellIdentityLte", nlohmann::json::object());
                const auto sig = cell.value("CellSignalStrengthLte", nlohmann::json::object());
                bool nested = cell.contains("CellIdentityLte");
                auto gi = [&](const char* k1, const char* k2, int d=0)
                    { return nested ? id.value(k1,d) : cell.value(k2,d); };
                auto gs = [&](const char* k1, const char* k2, const std::string& d="")
                    { return nested ? id.value(k1,d) : cell.value(k2,d); };

                net["ci"]           = nested ? id.value("CellIdentity",0) : cell.value("CellIdentity",0);
                net["earfcn"]       = gi("EARFCN","EARFCN");
                net["mcc"]          = gs("MCC","MCC");
                net["mnc"]          = gs("MNC","MNC");
                net["pci"]          = gi("PCI","PCI");
                net["tac"]          = gi("TAC","TAC");
                net["band"]         = gs("Band","Band");
                net["asu"]          = nested ? sig.value("ASU Level",0) : cell.value("ASU Level",0);
                net["cqi"]          = nested ? sig.value("CQI",0)       : cell.value("CQI",0);
                net["rsrp"]         = nested ? sig.value("RSRP",0)      : cell.value("RSRP",0);
                net["rsrq"]         = nested ? sig.value("RSRQ",0)      : cell.value("RSRQ",0);
                net["rssi"]         = nested ? sig.value("RSSI",0)      : cell.value("RSSI",0);
                net["rssnr"]        = nested ? sig.value("RSSNR",0)     : cell.value("RSSNR",0);
                net["timingAdvance"]= nested ? sig.value("Timing Advance",0): cell.value("Timing Advance",0);
            } else if (type == "GSM") {
                net["cid"]          = cell.value("CellIdentity",0);
                net["arfcn"]        = cell.value("ARFCN",0);
                net["lac"]          = cell.value("LAC",0);
                net["mcc"]          = cell.value("MCC","");
                net["mnc"]          = cell.value("MNC","");
                net["dbm"]          = cell.value("Dbm",0);
                net["rssi"]         = cell.value("RSSI",0);
                net["timingAdvance"]= cell.value("Timing Advance",0);
            } else if (type == "NR") {
                net["nci"]     = cell.value("NCI",0);
                net["pci"]     = cell.value("PCI",0);
                net["tac"]     = cell.value("TAC",0);
                net["mcc"]     = cell.value("MCC","");
                net["mnc"]     = cell.value("MNC","");
                net["nrarfcn"] = cell.value("Nrarfcn",0);
                net["ssRsrp"]  = cell.value("SS-RSRP",0);
                net["ssRsrq"]  = cell.value("SS-RSRQ",0);
                net["rssnr"]   = cell.value("SS-SINR",0);
            }
            networks.push_back(net);
        }
    }
    norm["networks"] = networks;
    return norm;
}



static void apply_measurement_to_location(const nlohmann::json& json, location* loc)
{
    if (!loc) return;
    std::lock_guard<std::mutex> lock(loc_mutex);
    if (json.contains("latitude"))  loc->latitude  = json["latitude"];
    if (json.contains("longitude")) loc->longitude = json["longitude"];
    if (json.contains("altitude"))  loc->altitude  = json["altitude"];
    if (json.contains("accuracy"))  loc->accuracy  = json["accuracy"];
    if (json.contains("time"))
        loc->time = json["time"].is_string() ? json["time"].get<std::string>() : json["time"].dump();

    loc->lte.clear(); loc->gsm.clear(); loc->nr.clear();
    if (json.contains("networks") && json["networks"].is_array())
        for (auto& net : json["networks"]) {
            std::string t = net.value("type","");
            if      (t=="LTE") loc->lte.push_back(net);
            else if (t=="GSM") loc->gsm.push_back(net);
            else if (t=="NR")  loc->nr.push_back(net);
        }
}



void load_from_json(location* loc, bool insert_into_db = true)
{
    std::ifstream file;
    std::string source;
    for (const auto& dir : { std::filesystem::current_path(),
                              std::filesystem::current_path().parent_path(),
                              std::filesystem::path(PROJECT_SOURCE_DIR) })
        for (const char* name : { kArchiveLogPath, kDriveTestLogPath, kLegacyLocationLogPath }) {
            file.open(dir / name);
            if (file.is_open()) { source = (dir/name).string(); goto found; }
            file.clear();
        }
found:
    if (!file.is_open()) {
        std::cerr << "JSON log not found\n"; return;
    }

    std::string line; float history_time=0; size_t loaded=0, inserted=0;
    nlohmann::json last;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto json = normalize_measurement_json(nlohmann::json::parse(line));
            last = json; loaded++;
            if (insert_into_db) inserted += insert_to_db(json);

            if (json.contains("latitude") && json.contains("longitude")) {
                std::lock_guard<std::mutex> lk(gps_mutex);
                lat_values.push_back(json["latitude"]);
                lon_values.push_back(json["longitude"]);
            }
            if (json.contains("networks") && json["networks"].is_array()) {
                bool has_data = false;
                for (auto& net : json["networks"]) {
                    if (net.value("type","") != "LTE") continue;
                    int pci = net.value("pci",0);
                    rsrp_values_map[pci].push_back((float)net.value("rsrp",0));
                    rssi_values_map[pci].push_back((float)net.value("rssi",0));
                    sinr_values_map[pci].push_back((float)net.value("rssnr",0));
                    pci_values_map[pci].push_back((float)pci);
                    time_values_map[pci].push_back(history_time);
                    has_data = true;
                    if (json.contains("latitude") && json.contains("longitude")) {
                        HeatmapPoint hp{json["latitude"],json["longitude"],
                            (float)net.value("rsrp",0),(float)net.value("rsrq",0),
                            (float)net.value("rssi",0),(float)json.value("altitude",0.0),
                            net.value("earfcn",0)};
                        std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                        heatmap_points.push_back(hp);
                    }
                }
                if (has_data) history_time++;
            }
        } catch(...) {}
    }
    if (!last.is_null()) apply_measurement_to_location(last, loc);
    graph_time = history_time;
    invalidate_heatmap_cache();
    std::cout << "Loaded " << loaded << " records from " << source << "\n";
    std::cout << "Inserted " << inserted << " rows into PostgreSQL\n";
    std::cout << "Loaded " << lat_values.size() << " GPS points\n";
}



GLuint load_texture(const std::string& path)
{
    int w,h,ch;
    unsigned char* data = stbi_load(path.c_str(),&w,&h,&ch,4);
    if (!data) { std::cerr << "Failed to load texture: " << path << "\n"; return 0; }
    GLuint tex; glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);
    stbi_image_free(data);
    return tex;
}

std::string get_tile_path(int z, int x, int y) {
    return "tiles/"+std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y)+".png";
}

size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr,size,nmemb,stream);
}

void download_tile_async(int z, int x, int y)
{
    std::string path = get_tile_path(z,x,y);
    if (std::filesystem::exists(path)) return;
    std::filesystem::create_directories("tiles/"+std::to_string(z)+"/"+std::to_string(x));
    const char* servers[]={"a","b","c"};
    std::string url = "https://"+std::string(servers[rand()%3])+".basemaps.cartocdn.com/rastertiles/voyager/"
                      +std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y)+".png";
    CURL* curl = curl_easy_init(); if (!curl) return;
    FILE* fp = fopen(path.c_str(),"wb"); if (!fp) { curl_easy_cleanup(curl); return; }
    curl_easy_setopt(curl,CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,    fp);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,    "Mozilla/5.0");
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,      10L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) std::cerr << "CURL ERROR: " << curl_easy_strerror(res) << "\n";
    curl_easy_cleanup(curl); fclose(fp);
}

GLuint get_or_request_tile(int z, int x, int y)
{
    std::string key  = std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y);
    std::string path = get_tile_path(z,x,y);
    std::lock_guard<std::mutex> lk(tile_cache_mutex);
    auto it = tile_texture_cache.find(key);
    if (it != tile_texture_cache.end() && it->second != 0) return it->second;
    if (std::filesystem::exists(path)) { GLuint tex=load_texture(path); tile_texture_cache[key]=tex; return tex; }
    if (!tile_requested[key]) {
        tile_requested[key]=true; tile_texture_cache[key]=0;
        std::thread([z,x,y](){ download_tile_async(z,x,y); }).detach();
    }
    return 0;
}



void run_server(location* loc)
{
    zmq::context_t ctx(1);
    zmq::socket_t  socket(ctx, ZMQ_REP);
    socket.bind("tcp://0.0.0.0:5555");
    std::cout << "SERVER STARTED ON PORT 5555\n";

    while (true) {
        zmq::message_t request;
        if (!socket.recv(request, zmq::recv_flags::none)) continue;
        std::string data(static_cast<char*>(request.data()), request.size());
        std::cout << "RECEIVED: " << data << "\n";

        try {
            auto json = normalize_measurement_json(nlohmann::json::parse(data));
            insert_to_db(json);
            apply_measurement_to_location(json, loc);

            if (json.contains("networks") && json["networks"].is_array())
                for (auto& net : json["networks"])
                    if (net.value("type","")=="LTE" && json.contains("latitude") && json.contains("longitude")) {
                        HeatmapPoint hp{json["latitude"],json["longitude"],
                            (float)net.value("rsrp",0),(float)net.value("rsrq",0),
                            (float)net.value("rssi",0),(float)json.value("altitude",0.0),
                            net.value("earfcn",0)};
                        { std::lock_guard<std::mutex> lk(heatmap_points_mutex); heatmap_points.push_back(hp); }
                        invalidate_heatmap_cache();
                    }

            std::ofstream("location_log.json", std::ios::app) << json.dump() << "\n";
            socket.send(zmq::buffer("OK"),    zmq::send_flags::none);
        } catch (const std::exception& ex) {
            std::cerr << "SERVER EXCEPTION: " << ex.what() << std::endl;
            socket.send(zmq::buffer("ERROR"), zmq::send_flags::none);
        } catch (...) {
            std::cerr << "SERVER EXCEPTION: unknown error" << std::endl;
            socket.send(zmq::buffer("ERROR"), zmq::send_flags::none);
        }
    }
}



void run_gui(location* loc)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Smartphone Network & GPS Monitor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 750,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext(); ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    bool done = false;
    while (!done) {
        if (!json_loaded) { load_from_json(loc); json_loaded = true; }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
        }
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();

        float lat, lon, alt, acc; std::string time_str;
        std::vector<nlohmann::json> lte_copy, gsm_copy, nr_copy;
        { std::lock_guard<std::mutex> lock(loc_mutex);
          lat=loc->latitude; lon=loc->longitude; alt=loc->altitude; acc=loc->accuracy;
          time_str=loc->time; lte_copy=loc->lte; gsm_copy=loc->gsm; nr_copy=loc->nr; }

        
        ImGui::Begin("Smartphone Monitor");
        ImGui::SeparatorText("GPS DATA");
        ImGui::Text("Latitude:  %.6f", lat); ImGui::Text("Longitude: %.6f", lon);
        ImGui::Text("Altitude:  %.2f m", alt); ImGui::Text("Accuracy:  %.2f m", acc);
        ImGui::Text("Time: %s", time_str.c_str());

        static float last_lat=0, last_lon=0;
        if (fabs(lat-last_lat)>0.00001f || fabs(lon-last_lon)>0.00001f) {
            std::lock_guard<std::mutex> lock(gps_mutex);
            lat_values.push_back(lat); lon_values.push_back(lon);
            last_lat=lat; last_lon=lon;
        }

        ImGui::Spacing(); ImGui::SeparatorText("LTE NETWORKS");
        if (!lte_copy.empty()) {
            for (auto& lte : lte_copy) {
                int pci=lte.value("pci",0), rsrp=lte.value("rsrp",0),
                    rssi=lte.value("rssi",0), rssnr=lte.value("rssnr",0);
                ImGui::Text("CI: %d",     lte.value("ci",0));
                ImGui::Text("EARFCN: %d", lte.value("earfcn",0));
                ImGui::Text("PCI: %d",    pci);   ImGui::Text("TAC: %d",   lte.value("tac",0));
                ImGui::Text("RSRP: %d",   rsrp);  ImGui::Text("RSRQ: %d",  lte.value("rsrq",0));
                ImGui::Text("RSSI: %d",   rssi);  ImGui::Text("RSSNR: %d", rssnr);

                rsrp_values_map[pci].push_back((float)rsrp);
                rssi_values_map[pci].push_back((float)rssi);
                sinr_values_map[pci].push_back((float)rssnr);
                pci_values_map[pci].push_back((float)pci);
                time_values_map[pci].push_back(graph_time);

                for (auto* m : {&rsrp_values_map[pci],&rssi_values_map[pci],
                                 &sinr_values_map[pci],&pci_values_map[pci],&time_values_map[pci]})
                    if (m->size()>200) m->erase(m->begin());
                ImGui::Separator();
            }
            graph_time++;
        } else ImGui::Text("No LTE data received");
        ImGui::Spacing();
ImGui::SeparatorText("GSM NETWORKS");

if (!gsm_copy.empty()) {

    for (auto& gsm : gsm_copy) {

        ImGui::Text("CID: %d", gsm.value("cid", 0));
        ImGui::Text("ARFCN: %d", gsm.value("arfcn", 0));
        ImGui::Text("LAC: %d", gsm.value("lac", 0));

        ImGui::Text("MCC: %s",
            gsm.value("mcc", "").c_str());

        ImGui::Text("MNC: %s",
            gsm.value("mnc", "").c_str());

        ImGui::Text("RSSI: %d",
            gsm.value("rssi", 0));

        ImGui::Text("DBM: %d",
            gsm.value("dbm", 0));

        ImGui::Separator();
    }

} else {

    ImGui::Text("No GSM data received");
}

ImGui::Spacing();
ImGui::SeparatorText("NR 5G NETWORKS");

if (!nr_copy.empty()) {

    for (auto& nr : nr_copy) {

        ImGui::Text("NCI: %d",
            nr.value("nci", 0));

        ImGui::Text("PCI: %d",
            nr.value("pci", 0));

        ImGui::Text("TAC: %d",
            nr.value("tac", 0));

        ImGui::Text("NRARFCN: %d",
            nr.value("nrarfcn", 0));

        ImGui::Text("MCC: %s",
            nr.value("mcc", "").c_str());

        ImGui::Text("MNC: %s",
            nr.value("mnc", "").c_str());

        ImGui::Text("SS-RSRP: %d",
            nr.value("ssRsrp", 0));

        ImGui::Text("SS-RSRQ: %d",
            nr.value("ssRsrq", 0));

        ImGui::Text("SS-SINR: %d",
            nr.value("rssnr", 0));

        ImGui::Separator();
    }

} else {

    ImGui::Text("No NR 5G data received");
}
        ImGui::End();

        
        ImGui::Begin("Map");
        static int zoom=14; static double cx=0, cy=0; static bool cam_init=false;
        ImGui::SeparatorText("HEATMAP SETTINGS");
        ImGui::Checkbox("Show Heatmap", &heatmap_enabled); ImGui::SameLine();
        ImGui::Checkbox("Show GPS Track", &gps_track_enabled);
        { int ci=(int)selected_criterion;
          if (ImGui::Combo("Criterion",&ci,heatmap_criterion_names,4))
              { selected_criterion=(HeatmapCriterion)ci; invalidate_heatmap_cache(); } }

        { std::vector<int> earfcns={-1};
          { std::lock_guard<std::mutex> lk(heatmap_points_mutex);
            for (auto& p : heatmap_points)
                if (std::find(earfcns.begin(),earfcns.end(),p.earfcn)==earfcns.end())
                    earfcns.push_back(p.earfcn); }
          int sel_idx=0;
          for (int i=0;i<(int)earfcns.size();i++) if (earfcns[i]==selected_earfcn){sel_idx=i;break;}
          std::vector<std::string> labels; std::vector<const char*> cl;
          for (int e:earfcns) labels.push_back(e==-1?"All":"EARFCN "+std::to_string(e));
          for (auto& s:labels) cl.push_back(s.c_str());
          if (ImGui::Combo("EARFCN",&sel_idx,cl.data(),(int)cl.size()))
              { selected_earfcn=earfcns[sel_idx]; invalidate_heatmap_cache(); } }

        if (ImGui::SliderFloat("IDW Radius (m)",&idw_radius_m,50.f,1000.f)) invalidate_heatmap_cache();
        if (ImGui::SliderFloat("Heatmap Alpha",&heatmap_alpha,0.1f,1.0f))   invalidate_heatmap_cache();
        { std::lock_guard<std::mutex> lk(heatmap_points_mutex);
          ImGui::Text("Heatmap points: %zu", heatmap_points.size()); }
        ImGui::Separator();

        float cur_lat, cur_lon; size_t gps_size;
        { std::lock_guard<std::mutex> lock(gps_mutex); gps_size=lat_values.size();
          cur_lat=gps_size>0?lat_values.back():0; cur_lon=gps_size>0?lon_values.back():0; }

        if (gps_size == 0) { ImGui::Text("No GPS data"); }
        else {
            const float TILE_PX=256.f, MAP_PX=768.f;
            if (!cam_init) { cx=lon_to_tilex(cur_lon,zoom); cy=lat_to_tiley(cur_lat,zoom); cam_init=true; }
            ImGui::Text("Position: %.6f  %.6f", cur_lat, cur_lon);
            ImGui::Text("Zoom: %d", zoom);

            ImVec2 map_start=ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("map_canvas",ImVec2(MAP_PX,MAP_PX),ImGuiButtonFlags_MouseButtonLeft);
            bool hovered=ImGui::IsItemHovered(), dragged=ImGui::IsItemActive()&&ImGui::IsMouseDragging(0);

            if (dragged) {
                ImVec2 d=ImGui::GetIO().MouseDelta; cx-=(double)d.x/TILE_PX; cy-=(double)d.y/TILE_PX;
                double mt=(double)(1<<zoom);
                if (cx<0) cx+=mt; if (cx>=mt) cx-=mt;
                cy=std::max(0.0,std::min(mt-1e-6,cy));
            }
            if (hovered && ImGui::GetIO().MouseWheel!=0) {
                int oz=zoom; zoom+=ImGui::GetIO().MouseWheel>0?1:-1; zoom=std::max(3,std::min(18,zoom));
                if (zoom!=oz) {
                    double scale=(double)(1<<zoom)/(double)(1<<oz); cx*=scale; cy*=scale;
                    double mt=(double)(1<<zoom);
                    while(cx<0)cx+=mt; while(cx>=mt)cx-=mt;
                    cy=std::max(0.0,std::min(mt-1e-6,cy));
                }
            }

            double tl_x=cx-MAP_PX/(2.0*TILE_PX), tl_y=cy-MAP_PX/(2.0*TILE_PX);
            double br_x=cx+MAP_PX/(2.0*TILE_PX), br_y=cy+MAP_PX/(2.0*TILE_PX);
            int min_tx=(int)floor(tl_x), max_tx=(int)floor(br_x);
            int min_ty=std::max(0,(int)floor(tl_y)), max_ty=std::min((1<<zoom)-1,(int)floor(br_y));
            int tile_count=1<<zoom;

            ImDrawList* draw=ImGui::GetWindowDrawList();
            draw->PushClipRect(map_start, ImVec2(map_start.x+MAP_PX,map_start.y+MAP_PX), true);

            
            for (int ty=min_ty;ty<=max_ty;ty++) for (int tx=min_tx;tx<=max_tx;tx++) {
                int wtx=((tx%tile_count)+tile_count)%tile_count;
                GLuint tex=get_or_request_tile(zoom,wtx,ty);
                float sx=map_start.x+(float)((tx-tl_x)*TILE_PX), sy=map_start.y+(float)((ty-tl_y)*TILE_PX);
                if (tex) draw->AddImage((void*)(intptr_t)tex,ImVec2(sx,sy),ImVec2(sx+TILE_PX,sy+TILE_PX));
                else     draw->AddRectFilled(ImVec2(sx,sy),ImVec2(sx+TILE_PX,sy+TILE_PX),IM_COL32(50,50,50,255));
            }

            
            if (heatmap_enabled) {
                size_t hp_count; { std::lock_guard<std::mutex> lk(heatmap_points_mutex); hp_count=heatmap_points.size(); }
                if (hp_count>0)
                    for (int ty=min_ty;ty<=max_ty;ty++) for (int tx=min_tx;tx<=max_tx;tx++) {
                        int wtx=((tx%tile_count)+tile_count)%tile_count;
                        GLuint htex=get_heatmap_tile_texture(zoom,wtx,ty,selected_criterion,
                            selected_earfcn,idw_radius_m,idw_power,heatmap_alpha);
                        float sx=map_start.x+(float)((tx-tl_x)*TILE_PX), sy=map_start.y+(float)((ty-tl_y)*TILE_PX);
                        if (htex) draw->AddImage((void*)(intptr_t)htex,ImVec2(sx,sy),ImVec2(sx+TILE_PX,sy+TILE_PX),
                            ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,(unsigned char)(heatmap_alpha*255)));
                    }
            }

            
            { std::lock_guard<std::mutex> lock(gps_mutex);
              if (gps_track_enabled)
                  for (size_t i=1;i<lat_values.size();i++) {
                      ImVec2 p1=latlon_to_screen(lat_values[i-1],lon_values[i-1],zoom,tl_x,tl_y,map_start);
                      ImVec2 p2=latlon_to_screen(lat_values[i],  lon_values[i],  zoom,tl_x,tl_y,map_start);
                      draw->AddLine(p1,p2,IM_COL32(30,140,255,120),1.5f);
                  }
              if (!lat_values.empty()) {
                  ImVec2 sc=latlon_to_screen(cur_lat,cur_lon,zoom,tl_x,tl_y,map_start);
                  draw->AddCircleFilled(sc,9.f,IM_COL32(255,255,255,255));
                  draw->AddCircleFilled(sc,6.f,IM_COL32(220,40,40,255));
              }
            }

            draw->PopClipRect(); ImGui::SameLine();
            draw_heatmap_legend(draw, ImGui::GetCursorScreenPos());
            ImGui::Dummy(ImVec2(96.f,210.f));
        }
        ImGui::End();

        
        ImGui::Begin("Signal Graph");
        auto plot_map = [](const char* title, std::map<int,std::vector<float>>& data_map) {
            if (ImPlot::BeginPlot(title)) {
                for (auto& p : data_map) {
                    std::string lbl="PCI "+std::to_string(p.first);
                    ImPlot::PlotLine(lbl.c_str(),time_values_map[p.first].data(),p.second.data(),(int)p.second.size());
                }
                ImPlot::EndPlot();
            }
        };
        plot_map("LTE RSRP", rsrp_values_map);
        plot_map("LTE RSSI", rssi_values_map);
        plot_map("LTE SINR", sinr_values_map);
        plot_map("LTE PCI Activity", pci_values_map);
        if (ImPlot::BeginPlot("GPS Track")) {
            std::lock_guard<std::mutex> lock(gps_mutex);
            if (!lat_values.empty())
                ImPlot::PlotLine("Path",lon_values.data(),lat_values.data(),(int)lat_values.size());
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Render();
        int w,h; SDL_GetWindowSize(window,&w,&h);
        glViewport(0,0,w,h); glClearColor(0.1f,0.1f,0.1f,1.f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImPlot::DestroyContext(); ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx); SDL_DestroyWindow(window); SDL_Quit();
}



int main()
{
    init_db();
    static location locationInfo;
    std::thread server_thread(run_server, &locationInfo);
    bool db_has_data = database_has_data();
    if (!db_has_data) {
        std::cout << "Database empty -> loading JSON history and inserting into DB\n";
    } else {
        std::cout << "Database already contains data -> loading JSON history without inserting duplicates\n";
    }
    load_from_json(&locationInfo, !db_has_data);
    json_loaded = true;

    
    std::thread gui_thread(run_gui, &locationInfo);
    server_thread.detach(); gui_thread.join();
    PQfinish(conn);
    return 0;
}