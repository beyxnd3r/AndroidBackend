#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <GL/gl.h>  

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

struct ColorStop { 
    float val; 
    unsigned char r, g, b; 
};

enum class HeatmapCriterion { RSRP = 0, RSRQ, RSSI, Altitude };
extern const char* heatmap_criterion_names[];

#endif 