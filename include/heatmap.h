#ifndef HEATMAP_H
#define HEATMAP_H

#include <vector>
#include <mutex>
#include <map>
#include <GL/gl.h>
#include "imgui.h"
#include "types.h"

// External declarations
extern std::vector<HeatmapPoint> heatmap_points;
extern std::mutex heatmap_points_mutex;
extern HeatmapCriterion selected_criterion;
extern int selected_earfcn;
extern float heatmap_alpha;
extern float idw_radius_m;
extern float idw_power;
extern bool heatmap_enabled;
extern bool gps_track_enabled;

// Global array for criterion names
extern const char* heatmap_criterion_names[];

// Function declarations
void colormap_lookup(const std::vector<ColorStop>& cmap, float val,
                     unsigned char& r, unsigned char& g, unsigned char& b);
void compute_heatmap_tile(int z, int tx, int ty,
                          const std::vector<HeatmapPoint>& pts,
                          HeatmapCriterion crit, int earfcn_filter,
                          float radius_m, float power, unsigned char alpha_byte,
                          std::vector<unsigned char>& out_img);
void invalidate_heatmap_cache();
void request_heatmap_tile(int z, int tx, int ty,
                          HeatmapCriterion crit, int earfcn_filter,
                          float radius_m, float power, float alpha);
GLuint get_heatmap_tile_texture(int z, int tx, int ty,
                                HeatmapCriterion crit, int earfcn_filter,
                                float radius_m, float power, float alpha);
void draw_heatmap_legend(ImDrawList* draw, ImVec2 legend_start);

#endif 