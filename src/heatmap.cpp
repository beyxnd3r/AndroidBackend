#include "../include/heatmap.h"
#include "../include/gps_tracking.h"
#include "../include/config.h"
#include "../include/types.h"
#include <mutex>
#include <thread>
#include <iostream>
#include <cmath>
#include <GL/gl.h>

// Global variables
std::vector<HeatmapPoint> heatmap_points;
std::mutex heatmap_points_mutex;
HeatmapCriterion selected_criterion = HeatmapCriterion::RSRP;
int selected_earfcn = -1;
bool heatmap_enabled = true;
bool gps_track_enabled = false;
float heatmap_alpha = 0.65f;
float idw_radius_m = 50.0f;
float idw_power = 2.0f;

// Definition of heatmap criterion names
const char* heatmap_criterion_names[] = { "RSRP", "RSRQ", "RSSI", "Altitude" };

// Local cache variables
static std::mutex tile_cache_mutex;
static std::map<std::string, HeatmapTileEntry> heatmap_tex_cache;

// Color maps
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

void colormap_lookup(const std::vector<ColorStop>& cmap, float val,
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

void compute_heatmap_tile(int z, int tx, int ty,
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

void invalidate_heatmap_cache()
{
    std::lock_guard<std::mutex> lk(tile_cache_mutex);
    for (auto& kv : heatmap_tex_cache) { kv.second.dirty=true; kv.second.img.clear(); }
}

void request_heatmap_tile(int z, int tx, int ty,
                          HeatmapCriterion crit, int earfcn_filter,
                          float radius_m, float power, float alpha)
{
    std::string key = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);
    { std::lock_guard<std::mutex> lk(tile_cache_mutex);
      auto& e = heatmap_tex_cache[key];
      if (e.computing) return;
      e.computing = true; }

    std::vector<HeatmapPoint> pts_copy;
    { std::lock_guard<std::mutex> lk(heatmap_points_mutex); pts_copy = heatmap_points; }

    unsigned char alpha_byte = (unsigned char)(alpha*255.0f);
    std::thread([z,tx,ty,key,pts_copy,crit,earfcn_filter,radius_m,power,alpha_byte]() {
        std::vector<unsigned char> img;
        compute_heatmap_tile(z,tx,ty,pts_copy,crit,earfcn_filter,radius_m,power,alpha_byte,img);
        std::lock_guard<std::mutex> lk(tile_cache_mutex);
        auto& e = heatmap_tex_cache[key];
        e.img = std::move(img); e.computing = false; e.dirty = false;
    }).detach();
}

GLuint get_heatmap_tile_texture(int z, int tx, int ty,
                                HeatmapCriterion crit, int earfcn_filter,
                                float radius_m, float power, float alpha)
{
    std::string key = std::to_string(z)+"/"+std::to_string(tx)+"/"+std::to_string(ty);
    bool should_request = false;
    GLuint result = 0;
    {
        std::lock_guard<std::mutex> lk(tile_cache_mutex);
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

void draw_heatmap_legend(ImDrawList* draw, ImVec2 legend_start)
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