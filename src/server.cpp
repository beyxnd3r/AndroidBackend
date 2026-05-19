#include "../include/gui.h"
#include "../include/database.h"
#include "../include/json_parser.h"
#include "../include/heatmap.h"
#include "../include/gps_tracking.h"
#include "../include/config.h"
#include <zmq.hpp>
#include <iostream>
#include <fstream>

void run_server(location* loc)
{
    zmq::context_t ctx(1);
    zmq::socket_t  socket(ctx, ZMQ_REP);
    socket.bind(ZMQ_BIND_ADDRESS);
    std::cout << "SERVER STARTED ON PORT " << ZMQ_PORT << "\n";

    while (true) {
        zmq::message_t request;
        if (!socket.recv(request, zmq::recv_flags::none)) continue;
        std::string data(static_cast<char*>(request.data()), request.size());
        std::cout << "RECEIVED: " << data << "\n";

        try {
            auto json = normalize_measurement_json(nlohmann::json::parse(data));
            insert_to_db(json);
            apply_measurement_to_location(json, loc);

            if (json.contains("networks") && json["networks"].is_array())
                for (auto& net : json["networks"])
                    if (net.value("type","")=="LTE" && json.contains("latitude") && json.contains("longitude")) {
                        HeatmapPoint hp{json["latitude"],json["longitude"],
                            (float)net.value("rsrp",0),(float)net.value("rsrq",0),
                            (float)net.value("rssi",0),(float)json.value("altitude",0.0),
                            net.value("earfcn",0)};
                        { std::lock_guard<std::mutex> lk(heatmap_points_mutex); heatmap_points.push_back(hp); }
                        invalidate_heatmap_cache();
                    }

            std::ofstream("location_log.json", std::ios::app) << json.dump() << "\n";
            socket.send(zmq::buffer("OK"),    zmq::send_flags::none);
        } catch (const std::exception& ex) {
            std::cerr << "SERVER EXCEPTION: " << ex.what() << std::endl;
            socket.send(zmq::buffer("ERROR"), zmq::send_flags::none);
        } catch (...) {
            std::cerr << "SERVER EXCEPTION: unknown error" << std::endl;
            socket.send(zmq::buffer("ERROR"), zmq::send_flags::none);
        }
    }
}