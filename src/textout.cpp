#include "textout.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#ifdef _WIN32
#include <winsock2.h>   // gethostname
#endif
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
    // v2.0: машина — IP (стандарт как в TUI), hostname — только если IP не определился
    std::string ip = primary_ip("");
    printf("%s%s%s  |  %s  |  up %s%s\n",
           col.bold, (ip.empty() ? hostname_str() : ip).c_str(), col.rst, ts, uptime_str().c_str(), col.rst);
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

    // RAM (v2.0: swap убран, единицы — Gb)
    printf("RAM:  %s%4.1f / %4.1f Gb  (%3.0f%%)%s\n",
           color_for_pct(col, c.ram.usage_percent),
           c.ram.used_gb, c.ram.total_gb, c.ram.usage_percent, col.rst);

    // GPU
    if (c.gpus.empty()) {
        printf("  GPU: не найдена (нет nvidia-smi и AMD sysfs)\n");
    } else {
        for (const auto& g : c.gpus) {
            double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : 0;
            printf("  %sGPU%d%s  %-28s  util %s%3.0f%%%s  vram %5.1f/%5.1f Gb (%2.0f%%)",
                   col.bold, g.index, col.rst, g.name.c_str(),
                   color_for_pct(col, g.utilization), g.utilization, col.rst,
                   g.vram_used_gb, g.vram_total_gb, vram_pct);
            if (g.temperature >= 0)
                printf("  %s%.0f°C%s",
                       (g.temperature > 80 ? col.red : (g.temperature > 65 ? col.yel : col.grn)),
                       g.temperature, col.rst);
            if (g.clock_mhz >= 0) printf("  %.0f MHz", g.clock_mhz);
            if (g.power_watts >= 0) printf("  %.0f W", g.power_watts);
            if (!g.pcie.empty()) printf("  %s", g.pcie.c_str());   // v2.2: как карта подключена
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
        // v2.0: малые скорости — в KB/s (иначе всегда «0.0 MB/s»)
        if (rx < 0.1 && tx < 0.1)
            printf("NET:  rx %6.0f KB/s  tx %6.0f KB/s   [%s]\n", rx * 1024.0, tx * 1024.0, list.c_str());
        else
            printf("NET:  rx %5.1f MB/s  tx %5.1f MB/s   [%s]\n", rx, tx, list.c_str());
    }

    // DISKS — целые диски (+ занятость ФС; без ФС — по партициям)
    if (!c.disks.empty()) {
        for (const auto& d : c.disks) {
            std::string md = d.model.empty() ? "" : "  " + d.model;  // не .c_str() с временного!
            if (d.by_partitions)
                // ФС не смонтирована — точная занятость неизвестна
                printf("  %s%-8s%s %-4s %7.1f Gb  [%s]%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(),
                       d.size_gb, d.fs.c_str(), md.c_str());
            else if (d.usage_percent >= 0)
                printf("  %s%-8s%s %-4s %7.1f / %7.1f Gb  %s%3.0f%%%s%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(),
                       d.used_gb, d.total_gb,
                       color_for_pct(col, d.usage_percent), d.usage_percent, col.rst, md.c_str());
            else
                printf("  %s%-8s%s %-4s %6.1f Gb%s\n",
                       col.bold, d.name.c_str(), col.rst, d.type.c_str(), d.size_gb, md.c_str());
            // v2.2: активность диска — детектор «модель лезет на NVMe» (дельты /proc/diskstats)
            {
                double r = d.read_mbs > 0 ? d.read_mbs : 0.0;
                double w = d.write_mbs > 0 ? d.write_mbs : 0.0;
                if (r + w > 0.05 || d.busy_percent > 2.0)
                    printf("          r %6.1f MB/s  w %6.1f MB/s   busy %3.0f%%\n", r, w,
                           d.busy_percent > 0 ? d.busy_percent : 0.0);
            }
        }
    }

    // LLM — только если запущен сервер
    if (!c.llms.empty()) {
        hr();
        for (const auto& l : c.llms) {
            // v2.2 (Шеф 02.11): Strata подписывается своим именем, llama остаётся LLM
            const char* eng = (l.engine == "strata") ? "Strata" : "LLM";
            if (l.ctx_percent >= 0)
                printf("  %s%s%s  port %5d  %-32s  ctx %3.0f%% (%s/%s tok)\n",
                       col.bold, eng, col.rst, l.port, l.model_name.c_str(),
                       l.ctx_percent, fmt_tok(l.ctx_used).c_str(), fmt_tok(l.ctx_limit).c_str());
            else
                printf("  %s%s%s  port %5d  %-32s  ctx \u2014\n",
                       col.bold, eng, col.rst, l.port, l.model_name.c_str());
            // v2.0: разбор printa поправлен — аргументы стояли НЕ на своих местах
            // (cache reuse печатал prefill2, spec accept — cache, spec — висячим %s).
            // Теперь: decode gen | prefill upl./gen. — как в TUI; плюс ACTIVE-тег.
            printf("       ");
            if (!l.active.empty()) printf("%sACTIVE: %s%s   ", col.yel, l.active.c_str(), col.rst);
            if (l.gen_tps > 0) printf("decode gen.%6s t/s", fmt1(l.gen_tps).c_str());
            else printf("decode gen.   —   t/s");
            if (l.prefill_tps >= 0 && l.prefill2_tps > 0)
                printf("   prefill upl.%7s / gen.%7s t/s", fmt1(l.prefill_tps).c_str(), fmt1(l.prefill2_tps).c_str());
            else if (l.prefill_tps >= 0)
                printf("   prefill upl.%7s / gen.      - t/s", fmt1(l.prefill_tps).c_str());
            else
                printf("   prefill upl.      - / gen.      - t/s");
            printf("   cache reuse %5s%%   spec accept %5s%%\n",
                   l.cache_reuse_percent >= 0 ? fmt1(l.cache_reuse_percent).c_str() : "-",
                   l.spec_accept_percent >= 0 ? fmt1(l.spec_accept_percent).c_str() : "-");
            // v1.11 (Шеф 27.10): task/n_t/trnc — только текущие значения;
            // при отсутствии: task — прочерк (последний), остальные — 0
            // v1.12 (Шеф 27.10): на ходу — живой n_t (прогресс префилла + n_gen), trnc 0
            printf("       task %-7s n_t %-6s trnc %d\n",
                   l.last_task > 0 ? std::to_string(l.last_task).c_str() : "\u2014",
                   fmt_tok(l.task_live ? l.task_n_live : l.ctx_used).c_str(),
                   l.task_live ? 0 : (l.last_truncated >= 0 ? l.last_truncated : 0));
            // v2.2: MOE-размещение (Strata) и RAM/своп процесса модели
            if (!l.moe.empty())
                printf("       %sMOE  %s%s\n", col.dim, l.moe.c_str(), col.rst);
            if (l.rss_gb >= 0)
                printf("       %smem  RAM %5.1f Gb  swap %4.1f Gb%s\n", col.dim,
                       l.rss_gb, l.swap_gb >= 0 ? l.swap_gb : 0.0, col.rst);
        }
    }
    hr();
}
