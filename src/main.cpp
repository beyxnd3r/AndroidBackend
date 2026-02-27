#include <thread>
#include <mutex>
#include <fstream>
#include <iostream>

#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL.h>
#include <SDL_opengl.h>

struct location
{
    float latitude = 0.0f;
    float longitude = 0.0f;
    float altitude = 0.0f;
    std::string time = "no data";
};

std::mutex loc_mutex;

void run_server(location* loc)
{
    zmq::context_t ctx(1);
    zmq::socket_t socket(ctx, ZMQ_REP);

    socket.bind("tcp://*:5555");
    socket.set(zmq::sockopt::rcvtimeo, 100);

    while (true)
    {
        zmq::message_t request;
        auto result = socket.recv(request, zmq::recv_flags::none);
        if (!result) continue;

        std::string data(static_cast<char*>(request.data()), request.size());
        std::cout << "RAW: " << data << std::endl;

        try
        {
            auto json = nlohmann::json::parse(data);
            std::string type = json.value("type", "");

            if (type == "location")
            {
                std::lock_guard<std::mutex> lock(loc_mutex);

                loc->latitude = json["latitude"].get<float>();
                loc->longitude = json["longitude"].get<float>();
                loc->altitude = json["altitude"].get<float>();
                loc->time = json["time"].get<std::string>();

                std::ofstream file("location_log.json", std::ios::app);
                file << json.dump() << std::endl;

                std::cout << "UPDATED LOCATION\n";
            }

            std::string reply = "OK";
            socket.send(zmq::buffer(reply), zmq::send_flags::none);
        }
        catch (std::exception& e)
        {
            std::cout << "JSON ERROR: " << e.what() << std::endl;
            std::string reply = "ERROR";
            socket.send(zmq::buffer(reply), zmq::send_flags::none);
        }
    }
}

void run_gui(location* loc)
{
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "Location GUI",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        600, 400,
        SDL_WINDOW_OPENGL
    );

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool done = false;

    while (!done)
    {
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

        float lat, lon, alt;
        std::string time;

        {
            std::lock_guard<std::mutex> lock(loc_mutex);
            lat = loc->latitude;
            lon = loc->longitude;
            alt = loc->altitude;
            time = loc->time;
        }

        ImGui::Begin("Location");

        ImGui::Text("Latitude: %.6f", lat);
        ImGui::Text("Longitude: %.6f", lon);
        ImGui::Text("Altitude: %.2f", alt);
        ImGui::Text("Time: %s", time.c_str());

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, 600, 400);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main()
{
    static location locationInfo;

    std::thread server_thread(run_server, &locationInfo);
    std::thread gui_thread(run_gui, &locationInfo);

    gui_thread.join();
    server_thread.join();

    return 0;
}