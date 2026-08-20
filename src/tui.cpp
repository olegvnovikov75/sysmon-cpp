#include "tui.h"

#include <ncurses.h>
#include <unistd.h>

#ifndef CURSOR_INVISIBLE
#define CURSOR_INVISIBLE 2
#endif
#ifndef COLOR_GRAY
#define COLOR_GRAY COLOR_BLACK
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

int W = 100;

// весь текст белый (кроме % и баров) — по просьбе Шефа
const attr_t C_TXT   = COLOR_PAIR(1);
const attr_t C_DIM   = COLOR_PAIR(1);
const attr_t C_SEC   = COLOR_PAIR(1);
const attr_t C_HDR   = COLOR_PAIR(1) | A_BOLD;
const attr_t C_WARN  = COLOR_PAIR(1) | A_BOLD;
const attr_t C_OK    = COLOR_PAIR(2);
const attr_t C_HOT   = COLOR_PAIR(4);
const attr_t C_TRACK = COLOR_PAIR(6); // серый «хвост» прогресс-бара

// плавный градиент синий(0%) -> красный(100%) через 256-цветную палитру
const int GRAD_BASE  = 20;
const int GRAD_STEPS = 32;

void hsv2rgb(double h, double s, double v, int& r, int& g, int& b) {
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double r1, g1, b1;
    if (h < 60)       { r1=c; g1=x; b1=0; }
    else if (h < 120) { r1=x; g1=c; b1=0; }
    else if (h < 180) { r1=0; g1=c; b1=x; }
    else if (h < 240) { r1=0; g1=x; b1=c; }
    else if (h < 300) { r1=x; g1=0; b1=c; }
    else              { r1=c; g1=0; b1=x; }
    r = (int)((r1 + m) * 255 + 0.5);
    g = (int)((g1 + m) * 255 + 0.5);
    b = (int)((b1 + m) * 255 + 0.5);
}

int xterm256_nearest(int r, int g, int b) {
    int best = 0, bestd = 0x7fffffff;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
            for (int k = 0; k < 6; k++) {
                int cr = i ? i * 40 + 55 : 0;
                int cg = j ? j * 40 + 55 : 0;
                int cb = k ? k * 40 + 55 : 0;
                int d = (cr-r)*(cr-r) + (cg-g)*(cg-g) + (cb-b)*(cb-b);
                if (d < bestd) { bestd = d; best = 16 + 36*i + 6*j + k; }
            }
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        int d = (v-r)*(v-r) + (v-g)*(v-g) + (v-b)*(v-b);
        if (d < bestd) { bestd = d; best = 232 + i; }
    }
    return best;
}

void init_gradient() {
    for (int i = 0; i < GRAD_STEPS; i++) {
        double t = (double)i / (GRAD_STEPS - 1);
        double hue = 240.0 * (1.0 - t); // 240 (синий) -> 0 (красный)
        int r, g, b;
        hsv2rgb(hue, 1.0, 1.0, r, g, b);
        init_pair(GRAD_BASE + i, xterm256_nearest(r, g, b), -1);
    }
}

attr_t grad_attr(double r) {
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    int idx = (int)(r * (GRAD_STEPS - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= GRAD_STEPS) idx = GRAD_STEPS - 1;
    return COLOR_PAIR(GRAD_BASE + idx);
}

// цвет % индикатора = цвет бара на его уровне
attr_t pct_attr(double pct) {
    if (pct < 0) return C_DIM;
    return grad_attr(pct / 100.0);
}

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

std::string repeat(const std::string& s, int n) {
    std::string r;
    r.reserve(s.size() * n);
    for (int i = 0; i < n; i++) r += s;
    return r;
}

// бар: цвет ячейки — градиент по её позиции (лево = синий, право = красный),
// поле заключено в рамку [ ] — чтобы было видно на фоне
void add_bar(std::vector<Seg>& row, double pct, int width) {
    int filled = (pct < 0) ? 0 : (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    row.push_back({"[", C_TXT});
    for (int i = 0; i < width; i++) {
        double r = width > 1 ? (double)i / (width - 1) : 1.0;
        row.push_back({i < filled ? "█" : "░", i < filled ? grad_attr(r) : C_TRACK});
    }
    row.push_back({"]", C_TXT});
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
    strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", localtime(&t));
    return b;
}

void box_top(std::vector<std::vector<Seg>>& f, const std::string& title) {
    std::vector<Seg> line;
    line.push_back({"┌─ ", C_SEC});
    line.push_back({title, C_HDR});
    int fill = std::max(1, W - 4 - disp_width(title)); // 3 + title + fill + 1 = W
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
    int used = 3; // "│ " + "│"
    for (auto& s : content) used += disp_width(s.t);
    if (used < W) line.push_back({std::string(W - used, ' '), C_TXT}); // 2 + cw + pad + 1 = W
    line.push_back({"│", C_SEC});
    f.push_back(line);
}

// Привести сегменты ровно к target дисплейным колонкам (дополнить пробелами / обрезать)
std::vector<Seg> fit_width(std::vector<Seg> segs, int target) {
    if (target < 0) target = 0;
    int total = 0;
    for (auto& s : segs) total += disp_width(s.t);
    if (total < target) { segs.push_back({std::string(target - total, ' '), C_TXT}); return segs; }
    if (total == target) return segs;
    std::vector<Seg> out;
    int used = 0;
    for (auto& s : segs) {
        if (used >= target) break;
        int rem = target - used;
        const std::string& t = s.t;
        int cols = 0; size_t i = 0;
        while (i < t.size() && cols < rem) {
            unsigned char ch = t[i];
            if (ch < 0x80) { i++; cols++; }
            else if (ch >= 0xC0) {
                int len = (ch >= 0xF0) ? 4 : (ch >= 0xE0) ? 3 : 2;
                if (i + len > t.size()) len = (int)(t.size() - i);
                i += len; cols++;
            } else i++;
        }
        out.push_back({t.substr(0, i), s.a});
        used += cols;
    }
    return out;
}

// Строка-бар: описание (слева) + значение (справа, перед баром) + бар у правого края.
// Все бары выровнены в одну колонку. value<0 → «—» и пустой бар. unit = "%" или "°C".
void bar_row(std::vector<std::vector<Seg>>& f, std::vector<Seg> desc, double value, const std::string& unit = "%") {
    std::vector<Seg> line;
    line.push_back({"│ ", C_SEC});
    int desc_w = W - 35;                       // описание; остальное — значение(5) + пробел + бар(26) + рамка
    for (auto& s : fit_width(desc, desc_w)) line.push_back(s);
    std::string vs; attr_t va = C_DIM;
    if (value >= 0) { char b[16]; snprintf(b, sizeof(b), "%.0f%s", value, unit.c_str()); vs = b; va = pct_attr(value); }
    else vs = "—";
    int vw = disp_width(vs);
    if (vw < 5) vs = std::string(5 - vw, ' ') + vs;   // значение — справа в 5 колонках
    line.push_back({vs, va});
    line.push_back({" ", C_TXT});
    add_bar(line, value, 24);
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
    box_top(f, c.cpu.model.empty() ? "CPU" : "CPU — " + c.cpu.model);
    {
        std::vector<Seg> desc;
        char lb[48];
        snprintf(lb, sizeof(lb), "load %.2f %.2f %.2f   %d cores",
                 c.cpu.load1, c.cpu.load5, c.cpu.load15, c.cpu.cores);
        desc.push_back({lb, C_DIM});
        bar_row(f, desc, c.cpu.usage, "%");
        bar_row(f, {}, c.cpu.temperature, "°C");
    }

    // RAM
    box_sep(f);
    box_top(f, "RAM");
    {
        std::vector<Seg> desc;
        char d[64];
        snprintf(d, sizeof(d), "%6.1f / %6.1f GiB  swap %.2f / %.2f GiB",
                 c.ram.used_gb, c.ram.total_gb, c.ram.swap_used_gb, c.ram.swap_total_gb);
        desc.push_back({d, C_TXT});
        bar_row(f, desc, c.ram.usage_percent, "%");
    }

    // GPU
    for (size_t i = 0; i < c.gpus.size(); i++) {
        const auto& g = c.gpus[i];
        box_sep(f);
        box_top(f, "GPU " + std::to_string(g.index) + " — " + g.name);
        std::vector<Seg> desc;
        double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : -1.0;
        char vb[64];
        snprintf(vb, sizeof(vb), "vram %5.1f/%5.1f GiB (%3.0f%%)",
                 g.vram_used_gb, g.vram_total_gb, vram_pct);
        desc.push_back({vb, C_TXT});
        if (g.clock_mhz >= 0) desc.push_back({"  " + maybe(g.clock_mhz, " MHz"), C_DIM});
        if (g.power_watts >= 0) desc.push_back({"  " + maybe(g.power_watts, " W"), C_DIM});
        bar_row(f, desc, g.utilization, "%");
        bar_row(f, {}, g.temperature, "°C");
    }
    if (c.gpus.empty()) {
        box_sep(f);
        box_top(f, "GPU");
        box_row(f, {{"не найдена (нет nvidia-smi и AMD sysfs)", C_DIM}});
    }

    // LLM (перед Network — по просьбе Шефа)
    if (c.llms.empty()) {
        box_sep(f);
        box_top(f, "LLM");
        box_row(f, {{"серверы llama-server не найдены", C_DIM}});
    } else {
        for (const auto& l : c.llms) {
            box_sep(f);
            box_top(f, "LLM : " + std::to_string(l.port) + " — " + l.model_name);

            // ctx — бар (метка с токенами слева, бар справа)
            {
                std::vector<Seg> desc;
                if (l.ctx_limit > 0)
                    desc.push_back({"ctx " + fmt_tok(l.ctx_used) + "/" + fmt_tok(l.ctx_limit) + " tok", C_TXT});
                else
                    desc.push_back({"ctx", C_TXT});
                bar_row(f, desc, l.ctx_percent, "%");
            }
            // cache reuse / spec accept — бары
            bar_row(f, {{"cache reuse", C_TXT}}, l.cache_reuse_percent, "%");
            bar_row(f, {{"spec accept", C_TXT}}, l.spec_accept_percent, "%");

            // gen/prefill/busy/reqs (сглажено: среднее из 5 замеров)
            std::vector<Seg> row;
            row.push_back({"gen " + (l.gen_tps >= 0 ? fmt1(l.gen_tps) : std::string("—")) + " t/s   ", C_TXT});
            row.push_back({"prefill " + (l.prefill_tps >= 0 ? fmt1(l.prefill_tps) : std::string("—")) + " t/s   ", C_TXT});
            row.push_back({"busy " + (l.busy_slots >= 0 ? fmt1(l.busy_slots) : std::string("—")) + "   ", C_DIM});
            row.push_back({"reqs " + (l.requests_processing >= 0 ? fmt1(l.requests_processing) : std::string("—")), C_DIM});
            if (!l.metrics_ok) row.push_back({"   (метрики недоступны)", C_WARN});
            box_row(f, row);
        }
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

    // Disks — целые диски (HDD/SSD/NVMe)
    if (!c.disks.empty()) {
        box_sep(f);
        box_top(f, "Disks");
        for (const auto& d : c.disks) {
            std::vector<Seg> desc;
            char nm[16]; snprintf(nm, sizeof(nm), "%-8s ", d.name.c_str());
            desc.push_back({nm, C_TXT});
            char tp[8]; snprintf(tp, sizeof(tp), "%-4s ", d.type.c_str());
            desc.push_back({tp, C_DIM});
            char sz[32];
            if (d.usage_percent >= 0)
                snprintf(sz, sizeof(sz), "%7.1f / %7.1f GiB", d.used_gb, d.total_gb);
            else
                snprintf(sz, sizeof(sz), "%7.1f GiB", d.size_gb);
            desc.push_back({sz, C_TXT});
            if (!d.model.empty()) desc.push_back({"  " + d.model, C_DIM});
            bar_row(f, desc, d.usage_percent, "%");
        }
    }

    // Sensors — hwmon-датчики кроме CPU/GPU (NVMe, сетевые PHY/MAC, ...)
    if (!c.sensors.empty()) {
        box_sep(f);
        box_top(f, "Sensors");
        for (const auto& s : c.sensors) {
            char nm[48]; snprintf(nm, sizeof(nm), "%-22s ", s.name.c_str());
            bar_row(f, {{nm, C_TXT}}, s.temp, "°C");
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
    init_pair(6, 240, -1);  // серый трек бара (видимый на тёмном фоне)
    init_gradient();
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
