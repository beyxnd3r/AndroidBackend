#ifndef DATABASE_H
#define DATABASE_H

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

extern PGconn* conn;

void init_db();
bool database_has_data();
size_t insert_to_db(const nlohmann::json& json);

#endif 