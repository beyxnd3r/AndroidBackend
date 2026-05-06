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
    if (!data) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(data);
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

    bool done = false;

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

            
            {
                std::lock_guard<std::mutex> lock(gps_mutex);

                
                for (size_t i = 1; i < lat_values.size(); i++)
                {
                    ImVec2 p1 = latlon_to_screen(lat_values[i-1], lon_values[i-1],
                                                  zoom, origin_tx, origin_ty, map_start);
                    ImVec2 p2 = latlon_to_screen(lat_values[i],   lon_values[i],
                                                  zoom, origin_tx, origin_ty, map_start);
                    draw->AddLine(p1, p2, IM_COL32(30, 140, 255, 220), 2.5f);
                }

                // Текущая позиция — красная точка, НЕ телепортируется
                // (всегда = lat_values.back(), карта центрируется вокруг неё)
                if (!lat_values.empty())
                {
                    ImVec2 cur_screen = latlon_to_screen(cur_lat, cur_lon,
                                                          zoom, origin_tx, origin_ty, map_start);
                    // Внешний белый круг
                    draw->AddCircleFilled(cur_screen, 9.0f, IM_COL32(255, 255, 255, 255));
                    // Внутренний красный
                    draw->AddCircleFilled(cur_screen, 6.0f, IM_COL32(220, 40,  40,  255));
                }
            }

            // Резервируем место под карту
            ImGui::Dummy(ImVec2(GRID * TILE_PX, GRID * TILE_PX));
        }

        ImGui::End();

        // ===================== Signal Graph =====================
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