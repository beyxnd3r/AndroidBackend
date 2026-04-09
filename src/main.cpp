#include <thread>
#include <mutex>
#include <fstream>
#include <iostream>
#include <vector>

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
    float latitude = 0.0f;
    float longitude = 0.0f;
    float altitude = 0.0f;
    float accuracy = 0.0f;
    std::string time = "no data";

    std::vector<nlohmann::json> lte;
    std::vector<nlohmann::json> gsm;
    std::vector<nlohmann::json> nr;
};

std::mutex loc_mutex;

std::vector<float> rsrp_values;
std::vector<float> time_values;
float graph_time = 0.0f;

std::vector<float> lat_values;
std::vector<float> lon_values;

bool json_loaded = false;



PGconn* conn; // ДОБАВЛЕНО

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
        std::string lat = std::to_string(json.value("latitude", 0.0));
        std::string lon = std::to_string(json.value("longitude", 0.0));
        std::string alt = std::to_string(json.value("altitude", 0.0));
        std::string acc = std::to_string(json.value("accuracy", 0.0));

        std::string pci = std::to_string(net.value("pci", 0));
        std::string rsrp = std::to_string(net.value("rsrp", 0));
        std::string rsrq = std::to_string(net.value("rsrq", 0));
        std::string rssi = std::to_string(net.value("rssi", 0));
        std::string sinr = std::to_string(net.value("rssnr", 0));

        const char* params[10] = {
            time.c_str(),
            lat.c_str(),
            lon.c_str(),
            alt.c_str(),
            acc.c_str(),
            pci.c_str(),
            rsrp.c_str(),
            rsrq.c_str(),
            rssi.c_str(),
            sinr.c_str()
        };

        PGresult* res = PQexecParams(
            conn,
            "INSERT INTO measurements(time, latitude, longitude, altitude, accuracy, pci, rsrp, rsrq, rssi, rssnr) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
            10,
            NULL,
            params,
            NULL,
            NULL,
            0
        );

        if (PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            std::cerr << "Insert error: " << PQerrorMessage(conn) << std::endl;
        }

        PQclear(res);
    }
}



void load_from_json()
{
    std::ifstream file("location_log.json");
    if (!file.is_open()) return;

    std::string line;

    while (std::getline(file, line))
    {
        if (line.empty()) continue;

        try
        {
            auto json = nlohmann::json::parse(line);

            if (json.contains("latitude") && json.contains("longitude"))
            {
                float lat = json["latitude"];
                float lon = json["longitude"];

                lat_values.push_back(lat);
                lon_values.push_back(lon);
            }
        }
        catch (...)
        {
        }
    }

    std::cout << "Loaded " << lat_values.size() << " GPS points\n";
}



void run_server(location* loc)
{
    zmq::context_t ctx(1);
    zmq::socket_t socket(ctx, ZMQ_REP);

    socket.bind("tcp://0.0.0.0:5555");

    std::cout << "SERVER STARTED ON PORT 5555\n";

    while (true)
    {
        zmq::message_t request;

        auto result = socket.recv(request, zmq::recv_flags::none);
        if (!result)
            continue;

        std::string data(static_cast<char*>(request.data()), request.size());

        std::cout << "RECEIVED: " << data << std::endl;

        try
        {
            auto json = nlohmann::json::parse(data);

            insert_to_db(json); 

            std::lock_guard<std::mutex> lock(loc_mutex);

            if (json.contains("latitude"))
                loc->latitude = json["latitude"];

            if (json.contains("longitude"))
                loc->longitude = json["longitude"];

            if (json.contains("altitude"))
                loc->altitude = json["altitude"];

            if (json.contains("accuracy"))
                loc->accuracy = json["accuracy"];

            if (json.contains("time"))
                loc->time = json["time"];

            loc->lte.clear();
            loc->gsm.clear();
            loc->nr.clear();

            if (json.contains("networks") && json["networks"].is_array())
            {
                for (auto& net : json["networks"])
                {
                    std::string type = net.value("type", "");

                    if (type == "LTE")
                        loc->lte.push_back(net);

                    else if (type == "GSM")
                        loc->gsm.push_back(net);

                    else if (type == "NR")
                        loc->nr.push_back(net);
                }
            }

            std::ofstream file("location_log.json", std::ios::app);
            file << json.dump() << std::endl;

            socket.send(zmq::buffer("OK"), zmq::send_flags::none);
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
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1000, 700,
        SDL_WINDOW_OPENGL
    );

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool done = false;

    while (!done)
    {
        if (!json_loaded)
        {
            load_from_json();
            json_loaded = true;
        }

        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT)
                done = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        float lat, lon, alt, acc;
        std::string time;

        std::vector<nlohmann::json> lte_copy;
        std::vector<nlohmann::json> gsm_copy;
        std::vector<nlohmann::json> nr_copy;

        {
            std::lock_guard<std::mutex> lock(loc_mutex);

            lat = loc->latitude;
            lon = loc->longitude;
            alt = loc->altitude;
            acc = loc->accuracy;
            time = loc->time;

            lte_copy = loc->lte;
            gsm_copy = loc->gsm;
            nr_copy  = loc->nr;
        }

        ImGui::Begin("Smartphone Monitor");

        ImGui::SeparatorText("GPS DATA");

        ImGui::Text("Latitude: %.6f", lat);
        ImGui::Text("Longitude: %.6f", lon);
        ImGui::Text("Altitude: %.2f m", alt);
        ImGui::Text("Accuracy: %.2f m", acc);
        ImGui::Text("Time: %s", time.c_str());

        static float last_lat = 0.0f;
        static float last_lon = 0.0f;

        if (fabs(lat - last_lat) > 0.00001f || fabs(lon - last_lon) > 0.00001f)
        {
            lat_values.push_back(lat);
            lon_values.push_back(lon);

            last_lat = lat;
            last_lon = lon;
        }

        ImGui::Spacing();

        ImGui::SeparatorText("LTE NETWORKS");

        if (!lte_copy.empty())
        {
            for (auto& lte : lte_copy)
            {
                ImGui::Text("CI: %d", lte.value("ci", 0));
                ImGui::Text("EARFCN: %d", lte.value("earfcn", 0));
                ImGui::Text("PCI: %d", lte.value("pci", 0));
                ImGui::Text("TAC: %d", lte.value("tac", 0));

                int rsrp = lte.value("rsrp", 0);

                ImGui::Text("RSRP: %d", rsrp);
                ImGui::Text("RSRQ: %d", lte.value("rsrq", 0));
                ImGui::Text("RSSI: %d", lte.value("rssi", 0));
                ImGui::Text("RSSNR: %d", lte.value("rssnr", 0));

                rsrp_values.push_back((float)rsrp);
                time_values.push_back(graph_time++);

                if (rsrp_values.size() > 200)
                {
                    rsrp_values.erase(rsrp_values.begin());
                    time_values.erase(time_values.begin());
                }

                ImGui::Separator();
            }
        }
        else
        {
            ImGui::Text("No LTE data received");
        }

        ImGui::End();

        ImGui::SameLine();

        ImGui::Begin("Signal Graph");

        if (ImPlot::BeginPlot("LTE RSRP"))
        {
            if (!rsrp_values.empty())
            {
                ImPlot::PlotLine(
                    "RSRP",
                    time_values.data(),
                    rsrp_values.data(),
                    rsrp_values.size()
                );
            }
            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("GPS Track"))
        {
            if (!lat_values.empty())
            {
                ImPlot::PlotLine(
                    "Path",
                    lon_values.data(),
                    lat_values.data(),
                    lat_values.size()
                );
            }
            ImPlot::EndPlot();
        }

        ImGui::End();

        ImGui::Render();

        glViewport(0, 0, 1000, 700);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
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