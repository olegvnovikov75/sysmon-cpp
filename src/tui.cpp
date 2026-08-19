#include "tui.h"

#include <ncurses.h>
#include <unistd.h>

#ifndef CURSOR_INVISIBLE
#define CURSOR_INVISIBLE 2
#endif
#ifndef COLOR_GRAY
#define COLOR_GRAY COLOR_BLACK
#endif
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

int W = 100;

const attr_t C_HDR  = COLOR_PAIR(1) | A_BOLD;
const attr_t C_OK   = COLOR_PAIR(2);
const attr_t C_WARN = COLOR_PAIR(3);
const attr_t C_HOT  = COLOR_PAIR(4);
const attr_t C_SEC  = COLOR_PAIR(5);
const attr_t C_DIM  = COLOR_PAIR(6);
const attr_t C_TXT  = A_NORMAL;

struct Seg { std::string t; attr_t a; };

int disp_width(const std::string& s) {
    int w = 0;
    for (unsigned char c : s)
        if (c < 0x80 || c >= 0xC0) w++;
    return w;
}

void draw_line(int y, const std::vector<Seg>& segs) {
    if (y < 0 || y >= LINES) return;
    if (getenv("SYSMON_DEBUG"))
        fprintf(stderr, "[tui] line %d W=%d COLS=%d LINES=%d\n", y, W, COLS, LINES);
    move(y, 0);
    int x = 0;
    for (const auto& s : segs) {
        if (x >= W) break;
        // обрезать сегмент по колонкам (addnstr с n криво считает multi-byte)
        size_t i = 0, used = 0;
        int cols = 0;
        while (i < s.t.size() && cols < W - x) {
            unsigned char c = s.t[i];
            if (c < 0x80) { used++; i++; cols++; }
            else if (c >= 0xC0) {
                int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
                if (i + len > s.t.size()) len = (int)(s.t.size() - i);
                used += len; i += len; cols++;
            }
            else i++; // stray continuation byte
        }
        if (used > 0) {
            attron(s.a);
            addstr(s.t.substr(0, used).c_str());
            attroff(s.a);
        }
        x += cols;
    }
    // подчистить хвост строки
    if (x < W) {
        attron(C_DIM);
        addstr(std::string(W - x, ' ').c_str());
        attroff(C_DIM);
    }
}

attr_t color_for(double pct) {
    if (pct < 0) return C_DIM;
    if (pct < 60) return C_OK;
    if (pct < 85) return C_WARN;
    return C_HOT;
}

std::string repeat(const std::string& s, int n) {
    std::string r;
    r.reserve(s.size() * n);
    for (int i = 0; i < n; i++) r += s;
    return r;
}

Seg bar(double pct, int width) {
    int filled = (pct < 0) ? 0 : (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    std::string t;
    t.reserve(width * 3);
    for (int i = 0; i < width; i++) t += (i < filled) ? "█" : "░";
    return {t, color_for(pct)};
}

std::string fmt1(double v) {
    char b[32]; snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

// 118000 -> "118k", 262144 -> "262k", 950 -> "950"
std::string fmt_tok(int v) {
    if (v >= 1000) {
        char b[32]; snprintf(b, sizeof(b), "%.0fk", v / 1000.0);
        return b;
    }
    return std::to_string(v);
}

std::string maybe(double v, const char* unit = "") {
    if (v < 0) return "—";
    char b[48]; snprintf(b, sizeof(b), "%.0f%s", v, unit);
    return b;
}

std::string uptime_str() {
    std::ifstream f("/proc/uptime");
    double s = 0;
    f >> s;
    if (!f || s <= 0) return "—";
    char b[48];
    snprintf(b, sizeof(b), "%ldd %02ldh %02ldm",
             (long)(s / 86400), (long)((long)s % 86400 / 3600), (long)((long)s % 3600 / 60));
    return b;
}

std::string hostname_str() {
    char b[256] = "";
    gethostname(b, sizeof(b) - 1);
    return b[0] ? b : "localhost";
}

std::string now_str() {
    time_t t = time(nullptr);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return b;
}

void box_top(std::vector<std::vector<Seg>>& f, const std::string& title) {
    std::vector<Seg> line;
    line.push_back({"┌─ ", C_SEC});
    line.push_back({title, C_HDR});
    int used = 4 + disp_width(title) + 1;
    int fill = std::max(1, W - 1 - used);
    line.push_back({repeat("─", fill), C_SEC});
    line.push_back({"┐", C_SEC});
    f.push_back(line);
}

void box_sep(std::vector<std::vector<Seg>>& f) {
    f.push_back({{"├", C_SEC}, {repeat("─", W - 2), C_SEC}, {"┤", C_SEC}});
}

void box_bottom(std::vector<std::vector<Seg>>& f) {
    f.push_back({{"└", C_SEC}, {repeat("─", W - 2), C_SEC}, {"┘", C_SEC}});
}

void box_row(std::vector<std::vector<Seg>>& f, const std::vector<Seg>& content) {
    std::vector<Seg> line;
    line.push_back({"│ ", C_SEC});
    for (auto& s : content) line.push_back(s);
    int used = 2 + 2;
    for (auto& s : content) used += disp_width(s.t);
    if (used < W) line.push_back({std::string(W - used, ' '), C_TXT});
    line.push_back({"│", C_SEC});
    f.push_back(line);
}

std::vector<std::vector<Seg>> build_frame(const Collector& c, int interval, bool paused) {
    std::vector<std::vector<Seg>> f;

    // title bar
    box_top(f, "sysmon — " + hostname_str());

    // строка статуса
    {
        std::vector<Seg> content;
        content.push_back({now_str() + "   ", C_TXT});
        content.push_back({"up " + uptime_str() + "   ", C_DIM});
        content.push_back({"refresh " + std::to_string(interval) + "s", C_DIM});
        if (paused) content.push_back({"   PAUSED", C_WARN});
        box_row(f, content);
    }

    // CPU
    box_sep(f);
    box_top(f, "CPU");
    {
        std::vector<Seg> row;
        char b[16]; snprintf(b, sizeof(b), "%3.0f%% ", c.cpu.usage);
        row.push_back({b, color_for(c.cpu.usage)});
        row.push_back({{"  "}, C_TXT});
        row.push_back(bar(c.cpu.usage, 24));
        row.push_back({"  " + maybe(c.cpu.temperature, "°C") + "   ", C_TXT});
        char lb[48];
        snprintf(lb, sizeof(lb), "load %.2f %.2f %.2f   %d cores",
                 c.cpu.load1, c.cpu.load5, c.cpu.load15, c.cpu.cores);
        row.push_back({lb, C_DIM});
        box_row(f, row);
    }

    // RAM
    box_sep(f);
    box_top(f, "RAM");
    {
        std::vector<Seg> row;
        char b[64];
        snprintf(b, sizeof(b), "%6.1f / %6.1f GiB (%3.0f%%)  ",
                 c.ram.used_gb, c.ram.total_gb, c.ram.usage_percent);
        row.push_back({b, color_for(c.ram.usage_percent)});
        row.push_back(bar(c.ram.usage_percent, 24));
        char sb[48];
        snprintf(sb, sizeof(sb), "  swap %.2f / %.2f GiB", c.ram.swap_used_gb, c.ram.swap_total_gb);
        row.push_back({sb, C_DIM});
        box_row(f, row);
    }

    // GPU
    for (size_t i = 0; i < c.gpus.size(); i++) {
        const auto& g = c.gpus[i];
        box_sep(f);
        box_top(f, "GPU " + std::to_string(g.index) + " — " + g.name);
        std::vector<Seg> row;
        char b[16]; snprintf(b, sizeof(b), "%3.0f%% ", g.utilization);
        row.push_back({b, color_for(g.utilization)});
        row.push_back({{"  "}, C_TXT});
        row.push_back(bar(g.utilization, 24));
        row.push_back({"  " + maybe(g.temperature, "°C") + "  ", C_TXT});
        double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : -1.0;
        char vb[64];
        snprintf(vb, sizeof(vb), "vram %5.1f/%5.1f GiB (%3.0f%%)  ",
                 g.vram_used_gb, g.vram_total_gb, vram_pct);
        row.push_back({vb, color_for(vram_pct)});
        row.push_back({maybe(g.clock_mhz, " MHz") + "  ", C_DIM});
        row.push_back({maybe(g.power_watts, " W"), C_DIM});
        box_row(f, row);
    }
    if (c.gpus.empty()) {
        box_sep(f);
        box_top(f, "GPU");
        box_row(f, {{"не найдена (нет nvidia-smi и AMD sysfs)", C_DIM}});
    }

    // Network
    box_sep(f);
    box_top(f, "Network");
    if (c.net.ifaces.empty()) {
        box_row(f, {{"нет активных интерфейсов", C_DIM}});
    } else {
        for (const auto& ifc : c.net.ifaces) {
            std::vector<Seg> row;
            char nm[16]; snprintf(nm, sizeof(nm), "%-10s ", ifc.name.c_str());
            row.push_back({nm, C_TXT});
            row.push_back({"↓ ", C_SEC});
            char rb[32]; snprintf(rb, sizeof(rb), "%7.1f MB/s  ", ifc.rx_mbps);
            row.push_back({rb, C_TXT});
            row.push_back({"↑ ", C_SEC});
            char tb[32]; snprintf(tb, sizeof(tb), "%7.1f MB/s", ifc.tx_mbps);
            row.push_back({tb, C_TXT});
            box_row(f, row);
        }
    }

    // LLM
    if (c.llms.empty()) {
        box_sep(f);
        box_top(f, "LLM");
        box_row(f, {{"серверы llama-server не найдены", C_DIM}});
    } else {
        for (const auto& l : c.llms) {
            box_sep(f);
            box_top(f, "LLM : " + std::to_string(l.port) + " — " + l.model_name);
            std::vector<Seg> row;
            if (l.ctx_percent >= 0) {
                char cb[64];
                snprintf(cb, sizeof(cb), "ctx %3.0f%% (%s/%s tok)   ",
                         l.ctx_percent, fmt_tok(l.ctx_used).c_str(), fmt_tok(l.ctx_limit).c_str());
                row.push_back({cb, color_for(l.ctx_percent)});
            } else {
                row.push_back({"ctx —   ", C_DIM});
            }
            row.push_back({"gen " + (l.gen_tps >= 0 ? fmt1(l.gen_tps) : std::string("—")) + " t/s   ", C_TXT});
            row.push_back({"prefill " + (l.prefill_tps >= 0 ? fmt1(l.prefill_tps) : std::string("—")) + " t/s", C_TXT});
            box_row(f, row);

            std::vector<Seg> row2;
            row2.push_back({"cache reuse " + (l.cache_reuse_percent >= 0 ? fmt1(l.cache_reuse_percent) + "%" : std::string("—")) + "   ", C_DIM});
            row2.push_back({"spec accept " + (l.spec_accept_percent >= 0 ? fmt1(l.spec_accept_percent) + "%" : std::string("—")) + "   ", C_DIM});
            row2.push_back({"busy " + (l.busy_slots >= 0 ? std::to_string(l.busy_slots) : std::string("—")) + "   ", C_DIM});
            row2.push_back({"reqs " + (l.requests_processing >= 0 ? std::to_string(l.requests_processing) : std::string("—")), C_DIM});
            if (!l.metrics_ok) row2.push_back({"   (метрики недоступны)", C_WARN});
            box_row(f, row2);
        }
    }

    box_bottom(f);

    // footer
    {
        std::vector<Seg> line;
        line.push_back({" q — выход   r — пауза/продолжить", C_DIM});
        f.push_back(line);
    }
    return f;
}

} // namespace

void tui_init() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(CURSOR_INVISIBLE);
    start_color();
    use_default_colors();
    init_pair(1, COLOR_WHITE, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_RED, -1);
    init_pair(5, COLOR_CYAN, -1);
    init_pair(6, COLOR_GRAY, -1);
}

void tui_shutdown() {
    endwin();
}

bool tui_frame(const Collector& c, int interval_sec, bool& paused) {
    W = std::min(COLS, 100);
    if (W < 40) W = COLS;

    auto frame = build_frame(c, interval_sec, paused);
    erase();
    for (size_t y = 0; y < frame.size() && (int)y < LINES; y++)
        draw_line((int)y, frame[y]);
    refresh();

    timeout(interval_sec * 1000);
    int ch = getch();
    timeout(-1);
    switch (ch) {
        case 'q': case 'Q': case 27: case 3: return false;
        case 'r': case 'R': case ' ': paused = !paused; break;
        default: break;
    }
    return true;
}
