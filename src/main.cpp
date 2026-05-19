#include "../include/gui.h"
#include "../include/database.h"
#include "../include/json_parser.h"
#include "../include/config.h"
#include <iostream>
#include <thread>

// Forward declaration of run_gui since it's in another file
// (already in gui.h)

int main()
{
    init_db();
    static location locationInfo;
    std::thread server_thread(run_server, &locationInfo);
    bool db_has_data = database_has_data();
    if (!db_has_data) {
        std::cout << "Database empty -> loading JSON history and inserting into DB\n";
    } else {
        std::cout << "Database already contains data -> loading JSON history without inserting duplicates\n";
    }
    load_from_json(&locationInfo, !db_has_data);
    
    std::thread gui_thread(run_gui, &locationInfo);
    server_thread.detach(); 
    gui_thread.join();
    PQfinish(conn);
    return 0;
}