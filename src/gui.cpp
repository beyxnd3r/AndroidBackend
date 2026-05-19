#include "../include/gui.h"
#include "../include/database.h"
#include "../include/json_parser.h"
#include "../include/heatmap.h"
#include "../include/gps_tracking.h"
#include "../include/texture_utils.h"
#include "../include/config.h"
#include "../include/types.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <iostream>
#include <mutex>
#include <map>
#include <vector>
#include <cmath>
#include <algorithm>


static std::map<int, std::vector<float>> rsrp_values_map, rssi_values_map,
                                          sinr_values_map, pci_values_map, time_values_map;
static float graph_time = 0.0f;
static bool json_loaded = false;

void run_gui(location* loc)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* window = SDL_CreateWindow("Smartphone Network & GPS Monitor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 750,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext(); 
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 130");
    glEnable(GL_BLEND); 
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    bool done = false;
    while (!done) {
        if (!json_loaded) { 
            load_from_json(loc); 
            json_loaded = true; 
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
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
            lat = loc->latitude; 
            lon = loc->longitude; 
            alt = loc->altitude; 
            acc = loc->accuracy;
            time_str = loc->time; 
            lte_copy = loc->lte; 
            gsm_copy = loc->gsm; 
            nr_copy = loc->nr; 
        }

        
        ImGui::Begin("Smartphone Monitor");
        ImGui::SeparatorText("GPS DATA");
        ImGui::Text("Latitude:  %.6f", lat); 
        ImGui::Text("Longitude: %.6f", lon);
        ImGui::Text("Altitude:  %.2f m", alt); 
        ImGui::Text("Accuracy:  %.2f m", acc);
        ImGui::Text("Time: %s", time_str.c_str());

        static float last_lat = 0, last_lon = 0;
        if (fabs(lat - last_lat) > 0.00001f || fabs(lon - last_lon) > 0.00001f) {
            std::lock_guard<std::mutex> lock(gps_mutex);
            lat_values.push_back(lat); 
            lon_values.push_back(lon);
            last_lat = lat; 
            last_lon = lon;
        }

        ImGui::Spacing(); 
        ImGui::SeparatorText("LTE NETWORKS");
        if (!lte_copy.empty()) {
            for (auto& lte : lte_copy) {
                int pci = lte.value("pci", 0), 
                    rsrp = lte.value("rsrp", 0),
                    rssi = lte.value("rssi", 0), 
                    rssnr = lte.value("rssnr", 0);
                ImGui::Text("CI: %d", lte.value("ci", 0));
                ImGui::Text("EARFCN: %d", lte.value("earfcn", 0));
                ImGui::Text("PCI: %d", pci);   
                ImGui::Text("TAC: %d", lte.value("tac", 0));
                ImGui::Text("RSRP: %d", rsrp);  
                ImGui::Text("RSRQ: %d", lte.value("rsrq", 0));
                ImGui::Text("RSSI: %d", rssi);  
                ImGui::Text("RSSNR: %d", rssnr);

                rsrp_values_map[pci].push_back((float)rsrp);
                rssi_values_map[pci].push_back((float)rssi);
                sinr_values_map[pci].push_back((float)rssnr);
                pci_values_map[pci].push_back((float)pci);
                time_values_map[pci].push_back(graph_time);

                for (auto* m : {&rsrp_values_map[pci], &rssi_values_map[pci],
                                 &sinr_values_map[pci], &pci_values_map[pci], 
                                 &time_values_map[pci]})
                    if (m->size() > 200) m->erase(m->begin());
                ImGui::Separator();
            }
            graph_time++;
        } else {
            ImGui::Text("No LTE data received");
        }
        ImGui::Spacing();
        
        ImGui::SeparatorText("GSM NETWORKS");
        if (!gsm_copy.empty()) {
            for (auto& gsm : gsm_copy) {
                ImGui::Text("CID: %d", gsm.value("cid", 0));
                ImGui::Text("ARFCN: %d", gsm.value("arfcn", 0));
                ImGui::Text("LAC: %d", gsm.value("lac", 0));
                ImGui::Text("MCC: %s", gsm.value("mcc", "").c_str());
                ImGui::Text("MNC: %s", gsm.value("mnc", "").c_str());
                ImGui::Text("RSSI: %d", gsm.value("rssi", 0));
                ImGui::Text("DBM: %d", gsm.value("dbm", 0));
                ImGui::Separator();
            }
        } else {
            ImGui::Text("No GSM data received");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("NR 5G NETWORKS");
        if (!nr_copy.empty()) {
            for (auto& nr : nr_copy) {
                ImGui::Text("NCI: %d", nr.value("nci", 0));
                ImGui::Text("PCI: %d", nr.value("pci", 0));
                ImGui::Text("TAC: %d", nr.value("tac", 0));
                ImGui::Text("NRARFCN: %d", nr.value("nrarfcn", 0));
                ImGui::Text("MCC: %s", nr.value("mcc", "").c_str());
                ImGui::Text("MNC: %s", nr.value("mnc", "").c_str());
                ImGui::Text("SS-RSRP: %d", nr.value("ssRsrp", 0));
                ImGui::Text("SS-RSRQ: %d", nr.value("ssRsrq", 0));
                ImGui::Text("SS-SINR: %d", nr.value("rssnr", 0));
                ImGui::Separator();
            }
        } else {
            ImGui::Text("No NR 5G data received");
        }
        ImGui::End();

        
        ImGui::Begin("Map");
        static int zoom = DEFAULT_ZOOM_LEVEL; 
        static double cx = 0, cy = 0; 
        static bool cam_init = false;
        ImGui::SeparatorText("HEATMAP SETTINGS");
        ImGui::Checkbox("Show Heatmap", &heatmap_enabled); 
        ImGui::SameLine();
        ImGui::Checkbox("Show GPS Track", &gps_track_enabled);
        
        int ci = (int)selected_criterion;
        if (ImGui::Combo("Criterion", &ci, heatmap_criterion_names, 4)) {
            selected_criterion = (HeatmapCriterion)ci; 
            invalidate_heatmap_cache();
        }

        {
            std::vector<int> earfcns = {-1};
            {
                std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                for (auto& p : heatmap_points)
                    if (std::find(earfcns.begin(), earfcns.end(), p.earfcn) == earfcns.end())
                        earfcns.push_back(p.earfcn);
            }
            int sel_idx = 0;
            for (int i = 0; i < (int)earfcns.size(); i++) 
                if (earfcns[i] == selected_earfcn) { sel_idx = i; break; }
            std::vector<std::string> labels; 
            std::vector<const char*> cl;
            for (int e : earfcns) 
                labels.push_back(e == -1 ? "All" : "EARFCN " + std::to_string(e));
            for (auto& s : labels) 
                cl.push_back(s.c_str());
            if (ImGui::Combo("EARFCN", &sel_idx, cl.data(), (int)cl.size())) {
                selected_earfcn = earfcns[sel_idx]; 
                invalidate_heatmap_cache();
            }
        }

        if (ImGui::SliderFloat("IDW Radius (m)", &idw_radius_m, 50.f, 1000.f)) 
            invalidate_heatmap_cache();
        if (ImGui::SliderFloat("Heatmap Alpha", &heatmap_alpha, 0.1f, 1.0f))   
            invalidate_heatmap_cache();
        
        {
            std::lock_guard<std::mutex> lk(heatmap_points_mutex);
            ImGui::Text("Heatmap points: %zu", heatmap_points.size());
        }
        ImGui::Separator();

        float cur_lat, cur_lon; 
        size_t gps_size;
        {
            std::lock_guard<std::mutex> lock(gps_mutex);
            gps_size = lat_values.size();
            cur_lat = gps_size > 0 ? lat_values.back() : 0;
            cur_lon = gps_size > 0 ? lon_values.back() : 0;
        }

        if (gps_size == 0) { 
            ImGui::Text("No GPS data"); 
        } else {
            const float TILE_PX = 256.f, MAP_PX = 768.f;
            if (!cam_init) { 
                cx = lon_to_tilex(cur_lon, zoom); 
                cy = lat_to_tiley(cur_lat, zoom); 
                cam_init = true; 
            }
            ImGui::Text("Position: %.6f  %.6f", cur_lat, cur_lon);
            ImGui::Text("Zoom: %d", zoom);

            ImVec2 map_start = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("map_canvas", ImVec2(MAP_PX, MAP_PX), ImGuiButtonFlags_MouseButtonLeft);
            bool hovered = ImGui::IsItemHovered();
            bool dragged = ImGui::IsItemActive() && ImGui::IsMouseDragging(0);

            if (dragged) {
                ImVec2 d = ImGui::GetIO().MouseDelta; 
                cx -= (double)d.x / TILE_PX; 
                cy -= (double)d.y / TILE_PX;
                double mt = (double)(1 << zoom);
                if (cx < 0) cx += mt; 
                if (cx >= mt) cx -= mt;
                cy = std::max(0.0, std::min(mt - 1e-6, cy));
            }
            if (hovered && ImGui::GetIO().MouseWheel != 0) {
                int oz = zoom;
                zoom += ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
                zoom = std::max(MIN_ZOOM_LEVEL, std::min(MAX_ZOOM_LEVEL, zoom));
                if (zoom != oz) {
                    double scale = (double)(1 << zoom) / (double)(1 << oz);
                    cx *= scale; 
                    cy *= scale;
                    double mt = (double)(1 << zoom);
                    while (cx < 0) cx += mt;
                    while (cx >= mt) cx -= mt;
                    cy = std::max(0.0, std::min(mt - 1e-6, cy));
                }
            }

            double tl_x = cx - MAP_PX / (2.0 * TILE_PX);
            double tl_y = cy - MAP_PX / (2.0 * TILE_PX);
            double br_x = cx + MAP_PX / (2.0 * TILE_PX);
            double br_y = cy + MAP_PX / (2.0 * TILE_PX);
            int min_tx = (int)floor(tl_x);
            int max_tx = (int)floor(br_x);
            int min_ty = std::max(0, (int)floor(tl_y));
            int max_ty = std::min((1 << zoom) - 1, (int)floor(br_y));
            int tile_count = 1 << zoom;

            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(map_start, ImVec2(map_start.x + MAP_PX, map_start.y + MAP_PX), true);

            
            for (int ty = min_ty; ty <= max_ty; ty++) {
                for (int tx = min_tx; tx <= max_tx; tx++) {
                    int wtx = ((tx % tile_count) + tile_count) % tile_count;
                    GLuint tex = get_or_request_tile(zoom, wtx, ty);
                    float sx = map_start.x + (float)((tx - tl_x) * TILE_PX);
                    float sy = map_start.y + (float)((ty - tl_y) * TILE_PX);
                    if (tex) {
                        draw->AddImage((void*)(intptr_t)tex, ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX));
                    } else {
                        draw->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX), IM_COL32(50, 50, 50, 255));
                    }
                }
            }

            
            if (heatmap_enabled) {
                size_t hp_count;
                {
                    std::lock_guard<std::mutex> lk(heatmap_points_mutex);
                    hp_count = heatmap_points.size();
                }
                if (hp_count > 0) {
                    for (int ty = min_ty; ty <= max_ty; ty++) {
                        for (int tx = min_tx; tx <= max_tx; tx++) {
                            int wtx = ((tx % tile_count) + tile_count) % tile_count;
                            GLuint htex = get_heatmap_tile_texture(zoom, wtx, ty, selected_criterion,
                                selected_earfcn, idw_radius_m, idw_power, heatmap_alpha);
                            float sx = map_start.x + (float)((tx - tl_x) * TILE_PX);
                            float sy = map_start.y + (float)((ty - tl_y) * TILE_PX);
                            if (htex) {
                                draw->AddImage((void*)(intptr_t)htex, ImVec2(sx, sy), ImVec2(sx + TILE_PX, sy + TILE_PX),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (unsigned char)(heatmap_alpha * 255)));
                            }
                        }
                    }
                }
            }

            
            {
                std::lock_guard<std::mutex> lock(gps_mutex);
                if (gps_track_enabled) {
                    for (size_t i = 1; i < lat_values.size(); i++) {
                        ImVec2 p1 = latlon_to_screen(lat_values[i-1], lon_values[i-1], zoom, tl_x, tl_y, map_start);
                        ImVec2 p2 = latlon_to_screen(lat_values[i], lon_values[i], zoom, tl_x, tl_y, map_start);
                        draw->AddLine(p1, p2, IM_COL32(30, 140, 255, 120), 1.5f);
                    }
                }
                if (!lat_values.empty()) {
                    ImVec2 sc = latlon_to_screen(cur_lat, cur_lon, zoom, tl_x, tl_y, map_start);
                    draw->AddCircleFilled(sc, 9.f, IM_COL32(255, 255, 255, 255));
                    draw->AddCircleFilled(sc, 6.f, IM_COL32(220, 40, 40, 255));
                }
            }

            draw->PopClipRect();
            ImGui::SameLine();
            draw_heatmap_legend(draw, ImGui::GetCursorScreenPos());
            ImGui::Dummy(ImVec2(96.f, 210.f));
        }
        ImGui::End();

        
        ImGui::Begin("Signal Graph");
        auto plot_map = [](const char* title, std::map<int, std::vector<float>>& data_map) {
            if (ImPlot::BeginPlot(title)) {
                for (auto& p : data_map) {
                    std::string lbl = "PCI " + std::to_string(p.first);
                    ImPlot::PlotLine(lbl.c_str(), time_values_map[p.first].data(), p.second.data(), (int)p.second.size());
                }
                ImPlot::EndPlot();
            }
        };
        plot_map("LTE RSRP", rsrp_values_map);
        plot_map("LTE RSSI", rssi_values_map);
        plot_map("LTE SINR", sinr_values_map);
        plot_map("LTE PCI Activity", pci_values_map);
        
        if (ImPlot::BeginPlot("GPS Track")) {
            std::lock_guard<std::mutex> lock(gps_mutex);
            if (!lat_values.empty()) {
                ImPlot::PlotLine("Path", lon_values.data(), lat_values.data(), (int)lat_values.size());
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
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