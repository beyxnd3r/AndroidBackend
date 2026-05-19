#include "../include/json_parser.h"
#include "../include/database.h"
#include "../include/heatmap.h"
#include "../include/gps_tracking.h"
#include "../include/config.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

// Временно объявим эти переменные здесь, пока они не перенесены в правильные места
static std::map<int, std::vector<float>> rsrp_values_map, rssi_values_map,
                                          sinr_values_map, pci_values_map, time_values_map;
static float graph_time = 0.0f;

const nlohmann::json* find_json_key(const nlohmann::json& obj,
                                    std::initializer_list<const char*> keys)
{
    if (!obj.is_object()) return nullptr;
    for (const char* k : keys) { 
        auto it = obj.find(k); 
        if (it!=obj.end()) return &(*it); 
    }
    return nullptr;
}

int int_value_any(const nlohmann::json& obj,
                  std::initializer_list<const char*> keys, int def)
{
    const nlohmann::json* v = find_json_key(obj,keys); 
    if (!v) return def;
    try {
        if (v->is_number_integer()) return v->get<int>();
        if (v->is_number())         return (int)v->get<double>();
        if (v->is_string())         return std::stoi(v->get<std::string>());
    } catch(...) {}
    return def;
}

double double_value_any(const nlohmann::json& obj,
                        std::initializer_list<const char*> keys, double def)
{
    const nlohmann::json* v = find_json_key(obj,keys); 
    if (!v) return def;
    try {
        if (v->is_number()) return v->get<double>();
        if (v->is_string()) return std::stod(v->get<std::string>());
    } catch(...) {}
    return def;
}

std::string string_value_any(const nlohmann::json& obj,
                             std::initializer_list<const char*> keys,
                             const std::string& def)
{
    const nlohmann::json* v = find_json_key(obj,keys); 
    if (!v) return def;
    try {
        if (v->is_string())          return v->get<std::string>();
        if (v->is_number_integer())  return std::to_string(v->get<long long>());
        if (v->is_number_float())    return std::to_string(v->get<double>());
    } catch(...) {}
    return def;
}

nlohmann::json normalize_measurement_json(const nlohmann::json& raw)
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

void apply_measurement_to_location(const nlohmann::json& json, location* loc)
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

void load_from_json(location* loc, bool insert_into_db)
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