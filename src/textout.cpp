#include "textout.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unistd.h>

namespace {

struct Colors {
    const char* bold = "";
    const char* dim = "";
    const char* red = "";
    const char* yel = "";
    const char* grn = "";
    const char* cyn = "";
    const char* rst = "";
};

Colors colors_init() {
    Colors c;
    if (isatty(1)) {
        c.bold = "\e[1m"; c.dim = "\e[2m"; c.red = "\e[31m";
        c.yel = "\e[33m"; c.grn = "\e[32m"; c.cyn = "\e[36m"; c.rst = "\e[0m";
    }
    return c;
}

const char* color_for_pct(Colors& c, double pct) {
    if (pct < 60) return c.grn;
    if (pct < 85) return c.yel;
    return c.red;
}

std::string fmt1(double v) { char b[32]; snprintf(b, sizeof(b), "%.1f", v); return b; }

std::string fmt_tok(int v) {
    if (v >= 1000) { char b[32]; snprintf(b, sizeof(b), "%.0fk", v / 1000.0); return b; }
    return std::to_string(v);
}

std::string uptime_str() {
    std::ifstream f("/proc/uptime");
    double s = 0; f >> s;
    if (!f || s <= 0) return "—";
    char b[48];
    snprintf(b, sizeof(b), "%ldd %02ldh", (long)(s / 86400), (long)((long)s % 86400 / 3600));
    return b;
}

std::string hostname_str() {
    char b[256] = "";
    gethostname(b, sizeof(b) - 1);
    return b[0] ? b : "localhost";
}

void hr() {
    for (int i = 0; i < 129; i++) putchar('-');
    putchar('\n');
}

} // namespace

void render_text(const Collector& c) {
    Colors col = colors_init();

    time_t t = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%d.%m.%Y %H:%M:%S", localtime(&t));

    hr();
    printf("%s%s%s  |  %s  |  up %s%s\n",
           col.bold, hostname_str().c_str(), col.rst, ts, uptime_str().c_str(), col.rst);
    hr();

    // CPU
    std::string cpu_model = c.cpu.model.empty() ? "" : "   " + c.cpu.model;
    if (c.cpu.temperature >= 0)
        printf("CPU:  %s%3.0f%%%s  @ %s%.0f°C%s   load: %.2f %.2f %.2f   cores: %d%s\n",
               color_for_pct(col, c.cpu.usage), c.cpu.usage, col.rst,
               (c.cpu.temperature > 80 ? col.red : (c.cpu.temperature > 65 ? col.yel : col.grn)),
               c.cpu.temperature, col.rst,
               c.cpu.load1, c.cpu.load5, c.cpu.load15, c.cpu.cores, cpu_model.c_str());
    else
        printf("CPU:  %s%3.0f%%%s  @ —   load: %.2f %.2f %.2f   cores: %d%s\n",
               color_for_pct(col, c.cpu.usage), c.cpu.usage, col.rst,
               c.cpu.load1, c.cpu.load5, c.cpu.load15, c.cpu.cores, cpu_model.c_str());

    // RAM
    printf("RAM:  %s%4.1f / %4.1f GiB  (%3.0f%%)%s   Swap: %4.2f / %4.2f GiB\n",
           color_for_pct(col, c.ram.usage_percent),
           c.ram.used_gb, c.ram.total_gb, c.ram.usage_percent, col.rst,
           c.ram.swap_used_gb, c.ram.swap_total_gb);

    // GPU
    if (c.gpus.empty()) {
        printf("  GPU: не найдена (нет nvidia-smi и AMD sysfs)\n");
    } else {
        for (const auto& g : c.gpus) {
            double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : 0;
            printf("  %sGPU%d%s  %-28s  util %s%3.0f%%%s  vram %5.1f/%5.1f GiB (%2.0f%%)",
                   col.bold, g.index, col.rst, g.name.c_str(),
                   color_for_pct(col, g.utilization), g.utilization, col.rst,
                   g.vram_used_gb, g.vram_total_gb, vram_pct);
            if (g.temperature >= 0)
                printf("  %s%.0f°C%s",
                       (g.temperature > 80 ? col.red : (g.temperature > 65 ? col.yel : col.grn)),
                       g.temperature, col.rst);
            if (g.clock_mhz >= 0) printf("  %.0f MHz", g.clock_mhz);
            if (g.power_watts >= 0) printf("  %.0f W", g.power_watts);
            printf("\n");
        }
    }

    // NET
    if (c.net.ifaces.empty()) {
        printf("NET:  нет активных интерфейсов\n");
    } else {
        std::string list;
        double rx = 0, tx = 0;
        for (const auto& ifc : c.net.ifaces) {
            if (!list.empty()) list += ",";
            list += ifc.name;
            rx += ifc.rx_mbps;
            tx += ifc.tx_mbps;
        }
        printf("NET:  rx %5.1f MB/s  tx %5.1f MB/s   [%s]\n", rx, tx, list.c_str());
    }

    // DISKS — целые диски (+ занятость ФС; без ФС — по партициям)
    if (!c.disks.empty()) {
        for (const auto& d : c.disks) {
            std::string md = d.model.empty() ? "" : "  " + d.model;  // не .c_str() с временного!
            if (d.by_partitions)
                // ФС не смонтирована — точная занятость неизвестна
                printf("  %s%-8s%s %-4s %7.1f GiB  [%s]%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(),
                       d.size_gb, d.fs.c_str(), md.c_str());
            else if (d.usage_percent >= 0)
                printf("  %s%-8s%s %-4s %7.1f / %7.1f GiB  %s%3.0f%%%s%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(),
                       d.used_gb, d.total_gb,
                       color_for_pct(col, d.usage_percent), d.usage_percent, col.rst, md.c_str());
            else
                printf("  %s%-8s%s %-4s %6.1f GiB%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(), d.size_gb, md.c_str());
        }
    }

    // LLM — только если запущен сервер
    if (!c.llms.empty()) {
        hr();
        for (const auto& l : c.llms) {
            if (l.ctx_percent >= 0)
                printf("  %sLLM%s  port %5d  %-32s  ctx %3.0f%% (%s/%s tok)\n",
                       col.bold, col.rst, l.port, l.model_name.c_str(),
                       l.ctx_percent, fmt_tok(l.ctx_used).c_str(), fmt_tok(l.ctx_limit).c_str());
            else
                printf("  %sLLM%s  port %5d  %-32s  ctx —\n",
                       col.bold, col.rst, l.port, l.model_name.c_str());
            printf("       gen %6s tok/s   prefill %6s tok/s   cache reuse %3s%%   spec accept %3s%%   busy slots %s   reqs %s%s\n",
                   l.gen_tps >= 0 ? fmt1(l.gen_tps).c_str() : "-",
                   l.prefill_tps >= 0 ? fmt1(l.prefill_tps).c_str() : "-",
                   l.cache_reuse_percent >= 0 ? fmt1(l.cache_reuse_percent).c_str() : "-",
                   l.spec_accept_percent >= 0 ? fmt1(l.spec_accept_percent).c_str() : "-",
                   l.busy_slots >= 0 ? fmt1(l.busy_slots).c_str() : "-",
                   l.requests_processing >= 0 ? fmt1(l.requests_processing).c_str() : "-",
                   l.metrics_ok ? "" : "   (метрики недоступны)");
        }
    }
    hr();
}
