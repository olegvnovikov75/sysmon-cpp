#ifndef COLLECTOR_H
#define COLLECTOR_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct CPUStats {
    double usage = 0.0;
    double temperature = -1.0;   // -1 = не найдено
    double load1 = 0.0, load5 = 0.0, load15 = 0.0;
    int cores = 0;
    std::string model;           // из /proc/cpuinfo "model name"
};

struct RAMStats {
    double used_gb = 0.0;
    double total_gb = 0.0;
    double usage_percent = 0.0;
    double swap_used_gb = 0.0;
    double swap_total_gb = 0.0;
};

struct GPUStats {
    int index = 0;
    std::string name;
    std::string vendor;          // "nvidia" | "amd"
    double utilization = -1.0;   // -1 = n/a
    double vram_used_gb = 0.0;
    double vram_total_gb = 0.0;
    double temperature = -1.0;
    double clock_mhz = -1.0;
    double power_watts = -1.0;
};

struct SensorStats {
    std::string name;   // "nvme Composite", "eno1 PHY Temperature"
    double temp = -1.0;
};

struct DiskStats {
    std::string name;      // "sda", "nvme0n1"
    std::string type;      // "HDD" | "SSD" | "NVMe"
    double size_gb = 0.0;
    std::string model;     // из sysfs (может быть пуст)
};

struct NetStats {
    struct Iface {
        std::string name;
        double rx_mbps = 0.0;
        double tx_mbps = 0.0;
    };
    std::vector<Iface> ifaces;   // только активные (UP)
    double rx_mbps = 0.0;        // сумма
    double tx_mbps = 0.0;
};

struct LLMStats {
    int port = 0;
    std::string model_name;
    int ctx_limit = 0;           // из cmdline: -c / --ctx-size (0 = не задано)
    int ctx_used = 0;            // llamacpp:n_tokens_max
    double ctx_percent = -1.0;   // -1 = нельзя посчитать
    double gen_tps = -1.0;       // predicted_tokens_seconds
    double prefill_tps = -1.0;   // prompt_tokens_seconds
    double cache_reuse_percent = -1.0;
    double spec_accept_percent = -1.0;
    int busy_slots = -1;
    int requests_processing = -1;
    bool metrics_ok = false;
};

class Collector {
public:
    Collector();

    void collect();             // один проход по всем источникам

    CPUStats cpu;
    RAMStats ram;
    std::vector<GPUStats> gpus;
    std::vector<SensorStats> sensors;  // hwmon-датчики кроме CPU/GPU (NVMe, NIC, ...)
    std::vector<DiskStats> disks;      // целые диски (без партиций)
    NetStats net;
    std::vector<LLMStats> llms;

private:
    bool first_cpu_ = true;
    uint64_t prev_cpu_idle_ = 0;
    uint64_t prev_cpu_total_ = 0;
    double prev_net_ts_ = 0.0;
    std::map<std::string, std::pair<uint64_t, uint64_t>> prev_net_;  // iface -> (rx,tx)

    void collect_cpu();
    void collect_ram();
    void collect_gpu();
    void collect_sensors();
    void collect_disks();
    void collect_net();
    void collect_llm();

    static double cpu_temperature();
};

#endif // COLLECTOR_H
