#ifndef GPS_TRACKING_H
#define GPS_TRACKING_H

#include <vector>
#include <mutex>
#include "imgui.h"

extern std::mutex gps_mutex;
extern std::vector<float> lat_values;
extern std::vector<float> lon_values;
extern std::mutex loc_mutex;

double haversine_m(double lat1, double lon1, double lat2, double lon2);
double lon_to_tilex(double lon, int z);
double lat_to_tiley(double lat, int z);
void tilex2lon(int x, int z, double& lon);
void tiley2lat(int y, int z, double& lat);
int long2tilex(double lon, int z);
int lat2tiley(double lat, int z);
ImVec2 latlon_to_screen(double lat, double lon, int zoom,
                        double tl_tx, double tl_ty, ImVec2 map_start);

#endif 