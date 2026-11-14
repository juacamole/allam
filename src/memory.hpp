#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace allam {

struct Buffers {
    double weights = 0;
    double kv = 0;
    double rs = 0;
    double output = 0;
    double compute = 0;

    double reserved() const;
    nlohmann::json to_json() const;
};

struct Footprint {
    Buffers main;
    Buffers mtp;
    int slots = 0;
    int slot_ctx = 0;
    bool kv_unified = false;
    bool complete = false;
    uint64_t generation = 0;

    double reserved() const;
    double kv() const;
    double compute() const;
    long pool_tokens() const;
    nlohmann::json to_json() const;
};

class LoadLog {
public:
    void feed(const std::string& line);
    Footprint footprint() const;

private:
    mutable std::mutex mu_;
    Footprint fp_;
    bool in_mtp_ = false;
    bool after_compute_ = false;
};

struct SystemMemory {
    long available_mib = 0;
    long swap_used_mib = 0;
    nlohmann::json to_json() const;
};

SystemMemory system_memory();

}
