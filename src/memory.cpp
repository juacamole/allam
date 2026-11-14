#include "memory.hpp"

#include <cmath>
#include <fstream>
#include <optional>
#include <utility>

namespace allam {

using nlohmann::json;

namespace {

double round2(double v) {
    return std::round(v * 100) / 100;
}

std::optional<std::pair<std::string, double>> buffer_line(const std::string& line) {
    const std::string marker = " buffer size = ";
    auto at = line.find(marker);
    if (at == std::string::npos || at == 0) return std::nullopt;
    auto end = line.find_last_not_of(' ', at - 1);
    auto begin = line.find_last_of(' ', end);
    auto kind = line.substr(begin == std::string::npos ? 0 : begin + 1, end - begin);
    try {
        return std::pair(kind, std::stod(line.substr(at + marker.size())));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

long field(const std::string& line, const std::string& key) {
    auto at = line.find(key);
    if (at == std::string::npos) return 0;
    try {
        return std::stol(line.substr(at + key.size()));
    } catch (const std::exception&) {
        return 0;
    }
}

bool ends_with(const std::string& s, const std::string& tail) {
    auto end = s.find_last_not_of(" \r\n");
    if (end == std::string::npos || end + 1 < tail.size()) return false;
    return s.compare(end + 1 - tail.size(), tail.size(), tail) == 0;
}

}

double Buffers::reserved() const {
    return kv + rs + output + compute;
}

json Buffers::to_json() const {
    return {
        {"weights_mib", round2(weights)},
        {"kv_mib", round2(kv)},
        {"rs_mib", round2(rs)},
        {"output_mib", round2(output)},
        {"compute_mib", round2(compute)},
    };
}

double Footprint::reserved() const {
    return main.reserved() + mtp.reserved();
}

double Footprint::kv() const {
    return main.kv + mtp.kv;
}

double Footprint::compute() const {
    return main.compute + mtp.compute;
}

long Footprint::pool_tokens() const {
    return kv_unified ? slot_ctx : static_cast<long>(slot_ctx) * slots;
}

json Footprint::to_json() const {
    double weights = main.weights + mtp.weights;
    return {
        {"main", main.to_json()},
        {"mtp", mtp.to_json()},
        {"reserved_mib", round2(reserved())},
        {"weights_mib", round2(weights)},
        {"total_mib", round2(weights + reserved())},
        {"slots", slots},
        {"slot_ctx", slot_ctx},
        {"kv_unified", kv_unified},
        {"complete", complete},
    };
}

void LoadLog::feed(const std::string& line) {
    std::lock_guard lock(mu_);
    if (line.find("load_model: loading model '") != std::string::npos) {
        auto generation = fp_.generation + 1;
        fp_ = Footprint{};
        fp_.generation = generation;
        in_mtp_ = false;
        after_compute_ = false;
        return;
    }
    if (fp_.complete) return;
    if (line.find("load_model: initializing, n_slots = ") != std::string::npos) {
        fp_.slots = static_cast<int>(field(line, "n_slots = "));
        fp_.slot_ctx = static_cast<int>(field(line, "n_ctx_slot = "));
        fp_.kv_unified = line.find("kv_unified = 'true'") != std::string::npos;
        return;
    }
    if (ends_with(line, "model loaded")) {
        fp_.complete = true;
        return;
    }

    auto buf = buffer_line(line);
    if (!buf) return;
    auto& [kind, size] = *buf;
    if (kind == "compute") {
        (in_mtp_ ? fp_.mtp : fp_.main).compute += size;
        after_compute_ = true;
        return;
    }
    if (after_compute_ && !in_mtp_) in_mtp_ = true;
    auto& b = in_mtp_ ? fp_.mtp : fp_.main;
    if (kind == "model") b.weights += size;
    else if (kind == "KV" || kind == "state") b.kv += size;
    else if (kind == "RS") b.rs += size;
    else if (kind == "output") b.output += size;
}

Footprint LoadLog::footprint() const {
    std::lock_guard lock(mu_);
    return fp_;
}

json SystemMemory::to_json() const {
    return {{"mem_available_mib", available_mib}, {"swap_used_mib", swap_used_mib}};
}

SystemMemory system_memory() {
    std::ifstream in("/proc/meminfo");
    long available = 0, swap_total = 0, swap_free = 0;
    for (std::string key; in >> key;) {
        long kb = 0;
        in >> kb;
        if (key == "MemAvailable:") available = kb;
        else if (key == "SwapTotal:") swap_total = kb;
        else if (key == "SwapFree:") swap_free = kb;
        in.ignore(64, '\n');
    }
    return {available / 1024, (swap_total - swap_free) / 1024};
}

}
