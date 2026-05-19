#ifndef CONFIG_H
#define CONFIG_H

#include <string>


extern const char* kArchiveLogPath;
extern const char* kDriveTestLogPath;
extern const char* kLegacyLocationLogPath;


extern const char* DB_HOST;
extern const char* DB_PORT;
extern const char* DB_NAME;
extern const char* DB_USER;
extern const char* DB_PASSWORD;


extern const int ZMQ_PORT;
extern const char* ZMQ_BIND_ADDRESS;


extern const int MIN_ZOOM_LEVEL;
extern const int MAX_ZOOM_LEVEL;
extern const int DEFAULT_ZOOM_LEVEL;
extern const float TILE_SIZE_PX;


extern const float DEFAULT_HEATMAP_ALPHA;
extern const float DEFAULT_IDW_RADIUS_M;
extern const float DEFAULT_IDW_POWER;

#endif 