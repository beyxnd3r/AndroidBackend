#include "../include/config.h"

const char* kArchiveLogPath = "archive.json";
const char* kDriveTestLogPath = "drive_test_log.json";
const char* kLegacyLocationLogPath = "location_log.json";

const char* DB_HOST = "localhost";
const char* DB_PORT = "5432";
const char* DB_NAME = "network_monitor";
const char* DB_USER = "postgres";
const char* DB_PASSWORD = "1234";

const int ZMQ_PORT = 5555;
const char* ZMQ_BIND_ADDRESS = "tcp://0.0.0.0:5555";

const int MIN_ZOOM_LEVEL = 3;
const int MAX_ZOOM_LEVEL = 18;
const int DEFAULT_ZOOM_LEVEL = 14;
const float TILE_SIZE_PX = 256.0f;

const float DEFAULT_HEATMAP_ALPHA = 0.65f;
const float DEFAULT_IDW_RADIUS_M = 50.0f;
const float DEFAULT_IDW_POWER = 2.0f;