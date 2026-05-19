#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <nlohmann/json.hpp>
#include <initializer_list>
#include <string>
#include "types.h"

const nlohmann::json* find_json_key(const nlohmann::json& obj,
                                    std::initializer_list<const char*> keys);
int int_value_any(const nlohmann::json& obj,
                  std::initializer_list<const char*> keys, int def=0);
double double_value_any(const nlohmann::json& obj,
                        std::initializer_list<const char*> keys, double def=0.0);
std::string string_value_any(const nlohmann::json& obj,
                             std::initializer_list<const char*> keys,
                             const std::string& def="");

nlohmann::json normalize_measurement_json(const nlohmann::json& raw);
void apply_measurement_to_location(const nlohmann::json& json, location* loc);
void load_from_json(location* loc, bool insert_into_db = true);

#endif 