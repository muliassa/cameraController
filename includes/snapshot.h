#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

class Snapshot {
	string root;
public:
    explicit Snapshot(json config);
	string take();
}

#endif