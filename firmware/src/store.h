// The board's own copy of the collection, on the LittleFS partition, so the screen never waits for
// the network. Flash is touched as little as possible: a write stalls the RGB panel, which reads
// its picture from PSRAM, and shows as streaks. So files are only rewritten when they changed.
#pragma once
#include <map>
#include <set>
#include <vector>

#include "vault.h"

bool store_begin();
bool store_load_records(std::vector<Record> &out);
bool store_save_records(const std::vector<Record> &records);   // true when the file changed
bool store_load_details(std::map<int, Detail> &out);
bool store_save_details(const std::map<int, Detail> &details);  // true when the file changed
String store_cover_path(int id);    // a baseline JPEG, 240 px, made by the sync task
std::set<int> store_cover_ids();    // ids with a cover on flash; one directory walk
// Write a file whole: readers see the old one or the new one, never half of it.
bool store_write(const String &path, const uint8_t *data, size_t len);
