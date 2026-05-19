#include "../include/database.h"
#include "../include/config.h"
#include "../include/json_parser.h"
#include <iostream>
#include <string>

PGconn* conn;

void init_db()
{
    std::string conn_str = "host=" + std::string(DB_HOST) + 
                           " port=" + std::string(DB_PORT) +
                           " dbname=" + std::string(DB_NAME) + 
                           " user=" + std::string(DB_USER) +
                           " password=" + std::string(DB_PASSWORD);
    conn = PQconnectdb(conn_str.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connection failed: " << PQerrorMessage(conn) << std::endl;
        exit(1);
    }
    std::cout << "Connected to PostgreSQL\n";
}

bool database_has_data()
{
    PGresult* res = PQexec(conn, "SELECT COUNT(*) FROM measurements");
    if (PQresultStatus(res) != PGRES_TUPLES_OK) { PQclear(res); return false; }
    int count = atoi(PQgetvalue(res,0,0)); PQclear(res);
    return count > 0;
}

size_t insert_to_db(const nlohmann::json& json)
{
    if (!json.contains("networks")) return 0;
    size_t inserted = 0;

    for (auto& net : json["networks"]) {
        std::string type      = net.value("type","");
        std::string time      = json.value("time","");
        std::string latitude  = std::to_string(json.value("latitude",0.0));
        std::string longitude = std::to_string(json.value("longitude",0.0));
        std::string altitude  = std::to_string(json.value("altitude",0.0));
        std::string accuracy  = std::to_string(json.value("accuracy",0.0));

        auto si = [&](std::initializer_list<const char*> k){ 
            return std::to_string(int_value_any(net,k)); 
        };
        auto ss = [&](std::initializer_list<const char*> k){ 
            return string_value_any(net,k); 
        };

        std::string p[36];
        p[0]=time; p[1]=latitude; p[2]=longitude; p[3]=altitude; p[4]=accuracy;
        p[5]=type;
        p[6]=ss({"band"});    p[7]=si({"ci"});           p[8]=si({"earfcn"});
        p[9]=ss({"mcc"});     p[10]=ss({"mnc"});          p[11]=si({"pci"});
        p[12]=si({"tac"});    p[13]=si({"asu"});          p[14]=si({"cqi"});
        p[15]=si({"rsrp"});   p[16]=si({"rsrq"});         p[17]=si({"rssi"});
        p[18]=si({"rssnr"});  p[19]=si({"timingAdvance"});
        p[20]=si({"cid"});    p[21]=si({"bsic"});         p[22]=si({"arfcn"});
        p[23]=si({"lac"});    p[24]=ss({"mcc"});          p[25]=ss({"mnc"});
        p[26]=si({"psc"});    p[27]=si({"dbm"});          p[28]=si({"rssi"});
        p[29]=si({"timingAdvance"});
        p[30]=ss({"band"});   p[31]=si({"nci"});          p[32]=si({"pci"});
        p[33]=si({"nrarfcn"});p[34]=si({"tac"});          p[35]=ss({"mcc"});

        const char* cparams[36];
        for (int i=0;i<36;i++) cparams[i]=p[i].c_str();

        PGresult* res = PQexecParams(conn,
            "INSERT INTO measurements ("
            "time,latitude,longitude,altitude,accuracy,network_type,"
            "lte_band,lte_ci,lte_earfcn,lte_mcc,lte_mnc,lte_pci,lte_tac,"
            "lte_asu_level,lte_cqi,lte_rsrp,lte_rsrq,lte_rssi,lte_rssnr,lte_timing_advance,"
            "gsm_cid,gsm_bsic,gsm_arfcn,gsm_lac,gsm_mcc,gsm_mnc,gsm_psc,"
            "gsm_dbm,gsm_rssi,gsm_timing_advance,"
            "nr_band,nr_nci,nr_pci,nr_nrarfcn,nr_tac,nr_mcc"
            ") VALUES ("
            "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,"
            "$16,$17,$18,$19,$20,$21,$22,$23,$24,$25,$26,$27,$28,$29,$30,"
            "$31,$32,$33,$34,$35,$36)",
            36, NULL, cparams, NULL, NULL, 0);

        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "Insert error: " << PQerrorMessage(conn) << std::endl;
        else
            inserted++;
        PQclear(res);
    }
    return inserted;
}