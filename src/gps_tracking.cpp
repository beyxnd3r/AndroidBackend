#include "../include/gps_tracking.h"
#include <mutex>
#include <cmath>
#include <vector>

std::mutex gps_mutex;
std::vector<float> lat_values;
std::vector<float> lon_values;
std::mutex loc_mutex;

double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2-lat1)*M_PI/180.0, dlon = (lon2-lon1)*M_PI/180.0;
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dlon/2)*sin(dlon/2);
    return 2.0*R*atan2(sqrt(a), sqrt(1.0-a));
}

double lon_to_tilex(double lon, int z) { 
    return (lon+180.0)/360.0*(1<<z); 
}

double lat_to_tiley(double lat, int z) {
    double r = lat*M_PI/180.0;
    return (1.0 - log(tan(r)+1.0/cos(r))/M_PI)/2.0*(1<<z);
}

void tilex2lon(int x, int z, double& lon) { 
    lon = (double)x/(1<<z)*360.0-180.0; 
}

void tiley2lat(int y, int z, double& lat) {
    double n = M_PI - 2.0*M_PI*y/(1<<z);
    lat = 180.0/M_PI*atan(0.5*(exp(n)-exp(-n)));
}

int long2tilex(double lon, int z) { 
    return (int)lon_to_tilex(lon,z); 
}

int lat2tiley(double lat, int z) { 
    return (int)lat_to_tiley(lat,z); 
}

ImVec2 latlon_to_screen(double lat, double lon, int zoom,
                        double tl_tx, double tl_ty, ImVec2 map_start)
{
    return { map_start.x + (float)((lon_to_tilex(lon,zoom)-tl_tx)*256.0),
             map_start.y + (float)((lat_to_tiley(lat,zoom)-tl_ty)*256.0) };
}