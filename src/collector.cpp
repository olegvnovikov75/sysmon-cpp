#include "collector.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <climits>
#include <algorithm>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>
#include <glob.h>
#include <ctime>
#include <curl/curl.h>

// ---------- утилиты ----------

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string basename_of(const std::string& p) {
    size_t i = p.find_last_of('/');
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

static bool read_first_line(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::getline(f, out);
    return true;
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// значение метрики из тела /metrics; имя ищем и в виде name, и с ':' вместо '_'
static bool get_metric(const std::string& body, const std::string& name, double& out) {
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) continue;
        size_t brace = tok.find('{');
        if (brace != std::string::npos) tok = tok.substr(0, brace);
        std::string norm = tok;
        for (char& c : norm) if (c == ':') c = '_';
        std::string normq = name;
        for (char& c : normq) if (c == ':') c = '_';
        if (norm == normq) {
            double v;
            if (ls >> v) { out = v; return true; }
        }
    }
    return false;
}

Collector::Collector() {}

void Collector::collect() {
    collect_cpu();
    collect_ram();
    collect_gpu();
    collect_sensors();
    collect_disks();
    collect_net();
    collect_llm();

    // метаданные снимка
    char hb[256] = "";
    gethostname(hb, sizeof(hb) - 1);
    host = hb[0] ? hb : "localhost";
    { std::ifstream f("/proc/uptime"); f >> uptime_s; }
    time_t t = time(nullptr);
    char tb[32];
    strftime(tb, sizeof(tb), "%d.%m.%Y %H:%M:%S", localtime(&t));
    ts = tb;
}

// ---------- CPU ----------

void Collector::collect_cpu() {
    auto read_stat = [](std::string& line, uint64_t& idle, uint64_t& total) -> bool {
        std::ifstream stat("/proc/stat");
        if (!stat.is_open()) return false;
        uint64_t user, nice, system, idle_, iowait, irq, softirq, steal;
        stat >> line >> user >> nice >> system >> idle_ >> iowait >> irq >> softirq >> steal;
        idle = idle_ + iowait + steal;
        total = user + nice + system + idle_ + iowait + irq + softirq + steal;
        return true;
    };

    std::string line;
    uint64_t idle = 0, total = 0;

    if (first_cpu_) {
        if (!read_stat(line, prev_cpu_idle_, prev_cpu_total_)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!read_stat(line, idle, total)) return;
        first_cpu_ = false;
    } else {
        if (!read_stat(line, idle, total)) return;
    }

    uint64_t idle_delta = idle - prev_cpu_idle_;
    uint64_t total_delta = total - prev_cpu_total_;
    cpu.usage = (total_delta > 0) ? 100.0 * (1.0 - (double)idle_delta / total_delta) : 0.0;
    prev_cpu_idle_ = idle;
    prev_cpu_total_ = total;

    cpu.temperature = cpu_temperature();

    std::ifstream loadavg("/proc/loadavg");
    if (loadavg.is_open()) loadavg >> cpu.load1 >> cpu.load5 >> cpu.load15;

    cpu.cores = std::thread::hardware_concurrency();
    if (cpu.cores == 0) cpu.cores = 1;

    // модель CPU (из /proc/cpuinfo, читаем один раз)
    if (cpu.model.empty()) {
        std::ifstream ci("/proc/cpuinfo");
        std::string l;
        while (std::getline(ci, l)) {
            if (l.find("model name") != std::string::npos) {
                size_t p = l.find(':');
                if (p != std::string::npos) cpu.model = trim(l.substr(p + 1));
                break;
            }
        }
        if (cpu.model.size() > 60) cpu.model = cpu.model.substr(0, 60) + "…";
    }
}

double Collector::cpu_temperature() {
    static const char* wanted[] = {"x86_pkg_temp", "cpu_thermal", "soc_thermal", "coretemp", nullptr};
    char path[512];
    for (int z = 0; ; z++) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        std::string type;
        if (!read_first_line(path, type)) break;
        type = trim(type);
        for (int i = 0; wanted[i]; i++) {
            if (type == wanted[i]) {
                snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
                int t = 0;
                std::ifstream f(path);
                if (f >> t && t >= 1000) return t / 1000.0;
                return -1.0;
            }
        }
    }
    // hwmon: k10temp / coretemp / x86_pkg_temp (без fork'а sensors каждую секунду)
    {
        static const char* hw_wanted[] = {"k10temp", "coretemp", "x86_pkg_temp", nullptr};
        DIR* hw = opendir("/sys/class/hwmon");
        if (hw) {
            struct dirent* e;
            while ((e = readdir(hw)) != NULL) {
                if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
                snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
                std::string nm;
                { std::ifstream nf(path); std::getline(nf, nm); }
                for (int i = 0; hw_wanted[i]; i++) {
                    if (nm == hw_wanted[i]) {
                        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", e->d_name);
                        int t = 0;
                        std::ifstream tf(path);
                        if (tf >> t && t >= 1000) { closedir(hw); return t / 1000.0; }
                        break;
                    }
                }
            }
            closedir(hw);
        }
    }
    // fallback: sensors (Tctl / Package id 0)
    FILE* f = popen("sensors 2>/dev/null | grep -E 'Tctl|Package id 0:' | head -1", "r");
    if (f) {
        char buf[256];
        std::string line;
        while (fgets(buf, sizeof(buf), f)) line += buf;
        pclose(f);
        size_t p = line.find_first_of("0123456789");
        if (p != std::string::npos) {
            int v = atoi(line.c_str() + p);
            return v > 0 ? (double)v : -1.0;
        }
    }
    return -1.0;
}

// ---------- RAM ----------

void Collector::collect_ram() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return;

    uint64_t mem_total = 0, mem_available = 0, swap_total = 0, swap_free = 0;

    std::string line;
    while (std::getline(meminfo, line)) {
        std::istringstream ls(line);
        std::string key;
        uint64_t value;
        if (!(ls >> key >> value)) continue;
        if (key == "MemTotal:") mem_total = value;
        else if (key == "MemAvailable:") mem_available = value;
        else if (key == "SwapTotal:") swap_total = value;
        else if (key == "SwapFree:") swap_free = value;
    }

    ram.total_gb = mem_total / (1024.0 * 1024.0);
    ram.used_gb = (mem_total - mem_available) / (1024.0 * 1024.0);
    ram.usage_percent = mem_total > 0 ? 100.0 * ram.used_gb / ram.total_gb : 0.0;
    ram.swap_total_gb = swap_total / (1024.0 * 1024.0);
    ram.swap_used_gb = (swap_total - swap_free) / (1024.0 * 1024.0);
}

// ---------- GPU ----------

static std::string amd_gpu_name(const std::string& card) {
    // PCI-адрес: readlink /sys/class/drm/cardN/device
    char buf[512];
    snprintf(buf, sizeof(buf), "/sys/class/drm/%s/device", card.c_str());
    std::string pci;
    FILE* f = popen(("readlink -f " + std::string(buf) + " 2>/dev/null").c_str(), "r");
    if (f) {
        char b2[512];
        if (fgets(b2, sizeof(b2), f)) pci = trim(b2);
        pclose(f);
    }
    pci = basename_of(pci);  // 0000:00:02.0

    if (pci.empty()) return "AMD GPU";
    std::string cmd = "lspci -s " + pci + " 2>/dev/null";
    f = popen(cmd.c_str(), "r");
    if (!f) return "AMD GPU";
    char line[512];
    std::string out;
    while (fgets(line, sizeof(line), f)) out += line;
    pclose(f);
    out = trim(out);
    if (out.empty()) return "AMD GPU";
    // формат: "c6:00.0 Display controller: Vendor [VendorID] Device (rev c1)"
    // имя устройства — после "[VendorID] "
    std::string dev;
    size_t b = out.find('[');
    if (b != std::string::npos) {
        size_t e = out.find(']', b);
        if (e != std::string::npos && e + 1 < out.size()) dev = trim(out.substr(e + 1));
    }
    if (dev.empty()) {
        size_t p = out.rfind(": ");
        if (p != std::string::npos) dev = trim(out.substr(p + 2));
    }
    // убираем (rev XX)
    size_t r = dev.rfind(" (rev ");
    if (r != std::string::npos) dev = dev.substr(0, r);
    // "Device 1586" — нет имени в pci.ids
    if (dev.rfind("Device ", 0) == 0) dev = "AMD GPU";
    if (dev.size() > 28) dev = dev.substr(0, 28);
    return dev.empty() ? "AMD GPU" : dev;
}

void Collector::collect_gpu() {
    gpus.clear();

    // NVIDIA
    FILE* n = popen("nvidia-smi --query-gpu=index,name,utilization.gpu,memory.used,memory.total,temperature.gpu,clocks.sm,power.draw --format=csv,noheader,nounits 2>/dev/null", "r");
    if (n) {
        char buf[512];
        while (fgets(buf, sizeof(buf), n)) {
            GPUStats gpu;
            gpu.vendor = "nvidia";
            int idx = -1, util = -1, temp = -1, clk = -1;
            char name[256] = "";
            unsigned long mu = 0, mt = 0;
            double pow = -1;
            int got = sscanf(buf, "%d, %255[^,], %d, %lu, %lu, %d, %d, %lf",
                             &idx, name, &util, &mu, &mt, &temp, &clk, &pow);
            if (got >= 6) {
                gpu.index = idx;
                gpu.name = trim(name);
                gpu.utilization = util >= 0 ? (double)util : -1.0;
                // nvidia-smi (nounits) отдаёт VRAM в MiB
                gpu.vram_used_gb = mu / 1024.0;
                gpu.vram_total_gb = mt / 1024.0;
                gpu.temperature = temp >= 0 ? (double)temp : -1.0;
                gpu.clock_mhz = clk >= 0 ? (double)clk : -1.0;
                if (pow > 0) gpu.power_watts = pow; // [N/A] -> -1 (не показываем)
                gpus.push_back(gpu);
            }
        }
        pclose(n);
    }

    // AMD (sysfs): маркер — mem_info_vram_total
    if (gpus.empty()) {
        DIR* drm = opendir("/sys/class/drm");
        if (drm) {
            struct dirent* entry;
            while ((entry = readdir(drm)) != NULL) {
                if (strncmp(entry->d_name, "card", 4) != 0) continue;
                char path[512];
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_vram_total", entry->d_name);
                FILE* f = fopen(path, "r");
                if (!f) continue;
                fclose(f);

                GPUStats gpu;
                gpu.vendor = "amd";
                gpu.index = atoi(entry->d_name + 4);
                gpu.name = amd_gpu_name(entry->d_name);

                unsigned long v = 0;
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/gpu_busy_percent", entry->d_name);
                f = fopen(path, "r");
                if (f) { if (fscanf(f, "%lu", &v) == 1) gpu.utilization = (double)v; fclose(f); }

                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_vram_used", entry->d_name);
                f = fopen(path, "r");
                if (f) { if (fscanf(f, "%lu", &v) == 1) gpu.vram_used_gb = v / (1024.0 * 1024.0 * 1024.0); fclose(f); }

                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/mem_info_vram_total", entry->d_name);
                f = fopen(path, "r");
                if (f) { if (fscanf(f, "%lu", &v) == 1) gpu.vram_total_gb = v / (1024.0 * 1024.0 * 1024.0); fclose(f); }

                // температура + мощность: hwmon (glob)
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/hwmon/hwmon*/temp1_input", entry->d_name);
                glob_t gb;
                if (glob(path, GLOB_NOSORT, NULL, &gb) == 0 && gb.gl_pathc > 0) {
                    f = fopen(gb.gl_pathv[0], "r");
                    if (f) { long t = 0; if (fscanf(f, "%ld", &t) == 1 && t > 0) gpu.temperature = t / 1000.0; fclose(f); }
                    globfree(&gb);
                }
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/hwmon/hwmon*/power1_input", entry->d_name);
                glob_t gp;
                if (glob(path, GLOB_NOSORT, NULL, &gp) == 0 && gp.gl_pathc > 0) {
                    f = fopen(gp.gl_pathv[0], "r");
                    if (f) { long p = 0; if (fscanf(f, "%ld", &p) == 1 && p > 0) gpu.power_watts = p / 1000000.0; fclose(f); }
                    globfree(&gp);
                }

                gpus.push_back(gpu);
            }
            closedir(drm);
        }
    }
}

// ---------- Датчики (hwmon, кроме CPU и GPU — они уже в своих секциях) ----------

void Collector::collect_sensors() {
    sensors.clear();
    DIR* hw = opendir("/sys/class/hwmon");
    if (!hw) return;
    struct dirent* entry;
    while ((entry = readdir(hw)) != NULL) {
        if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", entry->d_name);
        std::ifstream nf(path);
        std::string chip;
        std::getline(nf, chip);
        // CPU (k10temp/coretemp) и GPU (amdgpu) показаны в своих секциях;
        // acpitz/ACAD — ACPI-зона и адаптер, часто врут или пусты
        if (chip == "k10temp" || chip == "coretemp" || chip == "cpu_thermal" ||
            chip == "acpitz" || chip == "ACAD" || chip == "amdgpu") continue;
        for (int n = 1; n <= 8; n++) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_input", entry->d_name, n);
            std::ifstream tf(path);
            long millic = 0;
            if (!(tf >> millic) || millic < 1000 || millic > 150000) continue; // отсев мёртвых датчиков
            std::string label = "temp" + std::to_string(n);
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_label", entry->d_name, n);
            std::ifstream lf(path);
            std::string l;
            if (std::getline(lf, l) && !l.empty()) label = l;
            SensorStats s;
            s.name = chip + " " + label;
            s.temp = millic / 1000.0;
            sensors.push_back(s);
        }
    }
    closedir(hw);
}

// ---------- Диски (целые, без партиций) ----------

// mountpoint из /proc/mounts: unescape \NNN (\040 = пробел и т.п.)
static std::string unescape_mp(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            char a = s[i+1], b = s[i+2], c = s[i+3];
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                r += (char)((a-'0')*64 + (b-'0')*8 + (c-'0'));
                i += 3; continue;
            }
        }
        r += s[i];
    }
    return r;
}

// pv (sda1 / nvme0n1p2) → целый диск (sda / nvme0n1)
static std::string pv_to_disk(const std::string& pv) {
    size_t i = pv.size();
    while (i > 0 && isdigit((unsigned char)pv[i-1])) i--;
    if (i > 0 && pv[i-1] == 'p' && pv.rfind("nvme", 0) == 0) i--; // nvme0n1p2 → nvme0n1
    return pv.substr(0, i);
}

// Тип ФС на партиции (blkid; результат кэшируем — ФС за время работы не меняется).
// blkid не всегда в PATH — пробуем известные пути.
static std::string part_fs_type(const std::string& part) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(part);
    if (it != cache.end()) return it->second;
    std::string type;
    const char* bins[] = {"/sbin/blkid", "/usr/sbin/blkid", "blkid"};
    for (int i = 0; i < 3 && type.empty(); i++) {
        std::string cmd = std::string(bins[i]) + " -s TYPE -o value /dev/" + part + " 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) continue;
        char b[128] = "";
        if (fgets(b, sizeof(b), f)) type = trim(b);
        int rc = pclose(f);
        if (getenv("SYSMON_DEBUG"))
            fprintf(stderr, "[blkid] %s -> '%s' rc=%d\n", part.c_str(), type.c_str(), rc);
    }
    cache[part] = type;
    return type;
}

// Занятость ФС на диске: прямые партиции + LVM (dm → slaves → pv).
// Дополнительно: суммарный размер партиций с распознаваемой ФС (part_fs_gb)
// и их типы (fs_names) — для дисков без смонтированной ФС.
static void disk_usage(const std::string& disk, double& used_gb, double& total_gb,
                       double& part_fs_gb, std::string& fs_names) {
    used_gb = 0; total_gb = 0; part_fs_gb = 0; fs_names.clear();
    // /proc/mounts (свежий, псевдо-ФС отсечены)
    std::vector<std::pair<std::string, std::string>> mounts;  // (dev, mountpoint)
    { std::ifstream m("/proc/mounts"); std::string line;
      while (std::getline(m, line)) {
          std::istringstream ls(line); std::string dev, mp, fst;
          if (!(ls >> dev >> mp >> fst)) continue;
          if (fst == "tmpfs" || fst == "devtmpfs" || fst == "proc" || fst == "procfs" ||
              fst == "sysfs" || fst == "cgroup" || fst == "cgroup2" || fst == "overlay" ||
              fst == "squashfs" || fst == "iso9660" || fst == "efivarfs" || fst == "mqueue" ||
              fst == "debugfs" || fst == "tracefs" || fst == "securityfs" || fst == "pstore" ||
              fst == "bpf" || fst == "configfs" || fst == "ramfs" || fst == "autofs" ||
              fst == "hugetlbfs" || fst == "binfmt_misc" ||
              fst.rfind("fuse", 0) == 0 || fst.rfind("nsfs", 0) == 0) continue;
          mounts.push_back({dev, unescape_mp(mp)});
      } }
    // партиции диска
    std::vector<std::string> parts;
    { DIR* d = opendir(("/sys/block/" + disk).c_str());
      if (d) { struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            std::string p = e->d_name;
            if (p[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "/sys/block/%s/%s/partition", disk.c_str(), p.c_str());
            struct stat st; if (stat(path, &st) == 0) parts.push_back(p);
        } closedir(d); } }
    // ФС на партициях (blkid): даже не смонтированная ФС занимает место на диске
    for (const auto& p : parts) {
        std::string fs = part_fs_type(p);
        if (fs.empty()) continue;
        unsigned long long sectors = 0;
        char path[256];
        snprintf(path, sizeof(path), "/sys/block/%s/%s/size", disk.c_str(), p.c_str());
        { std::ifstream f(path); f >> sectors; }
        part_fs_gb += sectors * 512.0 / (1024.0 * 1024.0 * 1024.0);
        if (fs_names.find(fs) == std::string::npos)
            fs_names += (fs_names.empty() ? "" : ",") + fs;
    }
    // LVM-томы, чьи PV на этом диске
    std::vector<std::string> my_vols;
    { DIR* db = opendir("/sys/block");
      if (db) { struct dirent* e;
        while ((e = readdir(db)) != NULL) {
            std::string dm = e->d_name;
            if (dm.rfind("dm-", 0) != 0) continue;
            char path[256]; std::string vol;
            snprintf(path, sizeof(path), "/sys/block/%s/dm/name", dm.c_str());
            { std::ifstream f(path); std::getline(f, vol); vol = trim(vol); }
            if (vol.empty()) continue;
            char spath[256]; snprintf(spath, sizeof(spath), "/sys/block/%s/slaves", dm.c_str());
            DIR* ds = opendir(spath); if (!ds) continue;
            struct dirent* se; bool mine = false;
            while ((se = readdir(ds)) != NULL) {
                std::string pv = se->d_name;
                if (pv[0] == '.') continue;
                if (pv_to_disk(pv) == disk) { mine = true; break; }
            }
            closedir(ds);
            if (mine) my_vols.push_back(vol);
        } closedir(db); } }
    // суммируем ФС
    struct statvfs sv;
    for (const auto& mt : mounts) {
        const std::string& dev = mt.first;
        if (dev.rfind("/dev/", 0) != 0) continue;
        bool hit = false;
        std::string b = dev.substr(5);
        for (const auto& p : parts) if (b == p) { hit = true; break; }
        if (!hit && dev.rfind("/dev/mapper/", 0) == 0) {
            std::string vol = dev.substr(12);
            for (const auto& v : my_vols) if (v == vol) { hit = true; break; }
        }
        if (hit && statvfs(mt.second.c_str(), &sv) == 0) {
            total_gb += (double)sv.f_blocks * sv.f_frsize / (1024.0*1024.0*1024.0);
            used_gb  += (double)(sv.f_blocks - sv.f_bavail) * sv.f_frsize / (1024.0*1024.0*1024.0);
        }
    }
}

void Collector::collect_disks() {
    disks.clear();
    DIR* d = opendir("/sys/block");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        // skip "."/".." и виртуальные/оптические
        if (name[0] == '.' ||
            name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 ||
            name.rfind("dm-", 0) == 0 || name.rfind("sr", 0) == 0 ||
            name.rfind("zram", 0) == 0 || name.rfind("md", 0) == 0) continue;
        // партиция имеет symlink "partition" → показываем только целые диски
        char path[512];
        snprintf(path, sizeof(path), "/sys/block/%s/partition", name.c_str());
        struct stat st;
        if (stat(path, &st) == 0) continue;

        DiskStats dk;
        dk.name = name;

        // размер (секторы по 512 байт); size=0 — мёртвый слот кардридера (SD/MMC и т.п.)
        unsigned long long sectors = 0;
        snprintf(path, sizeof(path), "/sys/block/%s/size", name.c_str());
        { std::ifstream f(path); f >> sectors; }
        if (sectors == 0) continue;
        dk.size_gb = sectors * 512.0 / (1024.0 * 1024.0 * 1024.0);

        // тип: NVMe по имени, иначе rotational (1=HDD, 0=SSD)
        if (name.rfind("nvme", 0) == 0) {
            dk.type = "NVMe";
        } else {
            int rot = -1;
            snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", name.c_str());
            { std::ifstream f(path); f >> rot; }
            dk.type = (rot == 1) ? "HDD" : "SSD";
        }

        // модель: SCSI/SAS — device/model; NVMe — /sys/class/nvme/nvmeX/model
        std::string model;
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", name.c_str());
        { std::ifstream f(path); std::getline(f, model); model = trim(model); }
        if (model.empty() && name.rfind("nvme", 0) == 0) {
            size_t i = 4; // после "nvme"
            while (i < name.size() && isdigit((unsigned char)name[i])) i++;
            std::string ctrl = name.substr(0, i);  // nvme0n1 -> nvme0
            snprintf(path, sizeof(path), "/sys/class/nvme/%s/model", ctrl.c_str());
            std::ifstream f(path); std::getline(f, model); model = trim(model);
        }
        if (model.size() > 40) model = model.substr(0, 40) + "…";
        dk.model = model;

        // занятость ФС на диске (прямые партиции + LVM)
        double ug = 0, tg = 0, pfg = 0;
        std::string fsn;
        disk_usage(name, ug, tg, pfg, fsn);
        if (tg > 0) {
            dk.used_gb = ug; dk.total_gb = tg; dk.usage_percent = 100.0 * ug / tg;
        } else if (pfg > 0) {
            // смонтированной ФС нет — считаем по партициям:
            // сколько места занято партициями с распознаваемой ФС
            dk.by_partitions = true;
            dk.fs = fsn;
            dk.used_gb = pfg;
            dk.total_gb = dk.size_gb;
            dk.usage_percent = dk.size_gb > 0 ? 100.0 * pfg / dk.size_gb : 0.0;
        }

        disks.push_back(dk);
    }
    closedir(d);
    std::sort(disks.begin(), disks.end(),
              [](const DiskStats& a, const DiskStats& b) { return a.name < b.name; });
}

// ---------- Сеть ----------

void Collector::collect_net() {
    net.ifaces.clear();
    net.rx_mbps = 0.0;
    net.tx_mbps = 0.0;

    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (prev_net_ts_ == 0.0) prev_net_ts_ = now;
    double dt = now - prev_net_ts_;
    if (dt <= 0.01) dt = 0.01;
    prev_net_ts_ = now;

    DIR* d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        std::string name = entry->d_name;
        if (name == "lo") continue;
        if (name.rfind("veth", 0) == 0 || name.rfind("br-", 0) == 0 ||
            name.rfind("docker", 0) == 0 || name.rfind("tailscale", 0) == 0) continue;

        char path[512];
        std::string state;
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name.c_str());
        if (!read_first_line(path, state) || trim(state) != "up") continue;

        uint64_t rx = 0, tx = 0;
        snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", name.c_str());
        std::ifstream frx(path); frx >> rx;
        snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", name.c_str());
        std::ifstream ftx(path); ftx >> tx;

        auto it = prev_net_.find(name);
        NetStats::Iface ifc;
        ifc.name = name;
        if (it != prev_net_.end()) {
            uint64_t drx = rx >= it->second.first ? rx - it->second.first : 0;
            uint64_t dtx = tx >= it->second.second ? tx - it->second.second : 0;
            ifc.rx_mbps = drx / (1024.0 * 1024.0) / dt;
            ifc.tx_mbps = dtx / (1024.0 * 1024.0) / dt;
        }
        // первый проход — дельты ещё нет, показываем 0
        net.ifaces.push_back(ifc);
        net.rx_mbps += ifc.rx_mbps;
        net.tx_mbps += ifc.tx_mbps;
        prev_net_[name] = {rx, tx};
    }
    closedir(d);
}

// ---------- LLM (llama-server) ----------

static std::string arg_value(const std::vector<std::string>& args, const std::vector<std::string>& names) {
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        for (const auto& n : names) {
            if (a == n) {
                if (i + 1 < args.size()) return args[i + 1];
            } else if (a.rfind(n + "=", 0) == 0) {
                return a.substr(n.size() + 1);
            }
        }
    }
    return "";
}

void Collector::collect_llm() {
    llms.clear();

    DIR* proc = opendir("/proc");
    if (!proc) return;
    struct dirent* entry;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_type != DT_DIR) continue;
        char* end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid < 1) continue;

        char path[256];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) continue;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.find("llama-server") == std::string::npos) continue;

        // разбить на аргументы
        std::vector<std::string> args;
        std::string cur;
        for (char c : raw) {
            if (c == '\0') { if (!cur.empty()) { args.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) args.push_back(cur);

        std::string port_s = arg_value(args, {"--port"});
        int port = atoi(port_s.c_str());
        if (port <= 0) continue;

        std::string model = arg_value(args, {"--alias"});
        if (model.empty()) {
            model = arg_value(args, {"-m", "--model"});
            if (!model.empty()) model = basename_of(model);
        }

        std::string ctx_s = arg_value(args, {"-c", "--ctx-size"});
        int ctx_limit = atoi(ctx_s.c_str());

        LLMStats llm;
        llm.port = port;
        llm.model_name = model.empty() ? "(без имени)" : model;
        llm.ctx_limit = ctx_limit;

        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = "http://127.0.0.1:" + std::to_string(port) + "/metrics";
            std::string response;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);

            CURLcode rc = curl_easy_perform(curl);
            if (getenv("SYSMON_DEBUG"))
                fprintf(stderr, "[sysmon] port %d curl rc=%d len=%zu\n", port, (int)rc, response.size());
            if (rc == CURLE_OK) {
                llm.metrics_ok = true;
                double v = 0;
                if (get_metric(response, "llamacpp:n_tokens_max", v)) llm.ctx_used = (int)v;
                double gen_raw = 0, prefill_raw = 0, busy_raw = 0, reqs_raw = 0;
                if (get_metric(response, "llamacpp:predicted_tokens_seconds", v)) gen_raw = v;
                if (get_metric(response, "llamacpp:prompt_tokens_seconds", v)) prefill_raw = v;
                double ptot = -1, pcached = -1;
                if (get_metric(response, "llamacpp:prompt_tokens_total", v)) ptot = v;
                if (get_metric(response, "llamacpp:prompt_tokens_cached_total", v)) pcached = v;
                if (ptot > 0 && pcached >= 0)
                    llm.cache_reuse_percent = 100.0 * pcached / (ptot + pcached);
                double sacc = -1, sdraft = -1;
                if (get_metric(response, "llamacpp:spec_decode_num_accepted_tokens_total", v)) sacc = v;
                if (get_metric(response, "llamacpp:spec_decode_num_draft_tokens_total", v)) sdraft = v;
                if (sdraft > 0 && sacc >= 0)
                    llm.spec_accept_percent = 100.0 * sacc / sdraft;
                if (get_metric(response, "llamacpp:n_busy_slots_per_decode", v)) busy_raw = v;
                if (get_metric(response, "llamacpp:requests_processing", v)) reqs_raw = v;

                // сглаживание gen/prefill/busy/reqs: среднее из 5 + удержание 60с после нуля
                auto& sm = llm_smooth_[llm.port];
                double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                llm.gen_tps = sm.gen.update(gen_raw, now);
                llm.prefill_tps = sm.prefill.update(prefill_raw, now);
                llm.busy_slots = sm.busy.update(busy_raw, now);
                llm.requests_processing = sm.reqs.update(reqs_raw, now);

                if (llm.ctx_limit > 0 && llm.ctx_used > 0)
                    llm.ctx_percent = 100.0 * (llm.ctx_used < llm.ctx_limit ? llm.ctx_used : llm.ctx_limit) / llm.ctx_limit;
            } else {
                // метрики недоступны — показываем «—» (сглаживатель не трогаем)
                llm.gen_tps = -1; llm.prefill_tps = -1;
                llm.busy_slots = -1; llm.requests_processing = -1;
            }
            curl_easy_cleanup(curl);
        }

        llms.push_back(llm);
    }
    closedir(proc);
}
