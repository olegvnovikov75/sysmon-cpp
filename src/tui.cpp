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

// Два градиента (256-цвет, 32 шага):
//  GRAD_BASE — «опасный»: синий(0%) -> красный(100%). Для показателей,
//    которые при заполнении приведут к поломке: температура, RAM, VRAM, ctx, диски.
//  GRAD_SAFE — «безопасный»: синий(0%) -> зелёный(100%). Для загрузки,
//    где 100% — это нормально: CPU, GPU util, cache reuse, spec accept.
const int GRAD_BASE  = 20;
const int GRAD_SAFE  = 52;
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
        int r, g, b;
        hsv2rgb(240.0 * (1.0 - t), 1.0, 1.0, r, g, b);   // синий -> красный
        init_pair(GRAD_BASE + i, xterm256_nearest(r, g, b), -1);
        hsv2rgb(240.0 - 120.0 * t, 1.0, 1.0, r, g, b);   // синий -> зелёный
        init_pair(GRAD_SAFE + i, xterm256_nearest(r, g, b), -1);
    }
}

attr_t grad_attr(double r, bool safe = false) {
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    int idx = (int)(r * (GRAD_STEPS - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= GRAD_STEPS) idx = GRAD_STEPS - 1;
    return COLOR_PAIR((safe ? GRAD_SAFE : GRAD_BASE) + idx);
}

// цвет % индикатора = цвет бара на его уровне
attr_t pct_attr(double pct, bool safe = false) {
    if (pct < 0) return C_DIM;
    return grad_attr(pct / 100.0, safe);
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

// бар: цвет ячейки — градиент по её позиции (лево = синий, право —
// красный (danger) или зелёный (safe)), поле заключено в рамку [ ]
void add_bar(std::vector<Seg>& row, double pct, int width, bool safe = false) {
    int filled = (pct < 0) ? 0 : (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    row.push_back({"[", C_TXT});
    for (int i = 0; i < width; i++) {
        double r = width > 1 ? (double)i / (width - 1) : 1.0;
        row.push_back({i < filled ? "█" : "░", i < filled ? grad_attr(r, safe) : C_TRACK});
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

std::string uptime_from_s(double s) {
    if (s <= 0) return "—";
    char b[48];
    snprintf(b, sizeof(b), "%ldd %02ldh %02ldm",
             (long)(s / 86400), (long)((long)s % 86400 / 3600), (long)((long)s % 3600 / 60));
    return b;
}

std::string uptime_str() {
    std::ifstream f("/proc/uptime");
    double s = 0;
    f >> s;
    return uptime_from_s(s);
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

// Строка-бар: описание (слева) + значение (левое выравнивание в 5 кол.) + бар у правого края.
// Все бары выровнены в одну колонку. value<0 → «—» и пустой бар. unit = "%" или "°C".
// safe=true — «безопасная» гамма (до зелёного): 100% нормально.
// Длина бара масштабируется с шириной кадра: 24 при 100 колонках, сужается при узком кадре
// (режим «справа» — каждый кадр получает половину окна).
static int bar_len(int w) {
    int n = 24 * w / 100;
    if (n < 8) n = 8;
    if (n > 24) n = 24;
    return n;
}

void bar_row(std::vector<std::vector<Seg>>& f, std::vector<Seg> desc, double value,
             const std::string& unit = "%", bool safe = false) {
    std::vector<Seg> line;
    line.push_back({"│ ", C_SEC});
    int bl = bar_len(W);
    int desc_w = W - 11 - bl;                  // "│ "(2) + значение(5) + " "(1) + "["(1) + бар + "]"(1) + "│"(1)
    // desc — на desc_w-1 + обязательный пробел, чтобы не «прилипать» к значению в узком кадре
    for (auto& s : fit_width(desc, desc_w - 1)) line.push_back(s);
    line.push_back({" ", C_TXT});
    std::string vs; attr_t va = C_DIM;
    if (value >= 0) { char b[16]; snprintf(b, sizeof(b), "%.0f%s", value, unit.c_str()); vs = b; va = pct_attr(value, safe); }
    else vs = "—";
    int vw = disp_width(vs);
    if (vw < 5) vs = vs + std::string(5 - vw, ' ');   // значение — в столбик по ЛЕВОМУ краю
    line.push_back({vs, va});
    line.push_back({" ", C_TXT});
    add_bar(line, value, bl, safe);
    line.push_back({"│", C_SEC});
    f.push_back(line);
}

// Параметры кадра (локальный и remote-кадр строятся одним кодом)
struct FrameOpts {
    int width = 100;
    int interval = 2;
    bool paused = false;
    std::string title;      // пуст → "sysmon — <локальный hostname>"
    std::string status;     // пуст → локальная строка статуса
    bool footer = true;
    std::string footer_extra;
};

std::vector<std::vector<Seg>> build_frame(const Collector& c, const FrameOpts& o) {
    int savedW = W;
    W = o.width;
    std::vector<std::vector<Seg>> f;

    // title bar
    box_top(f, o.title.empty() ? ("sysmon — " + hostname_str()) : o.title);

    // строка статуса
    {
        std::vector<Seg> content;
        if (o.status.empty()) {
            content.push_back({now_str() + "   ", C_TXT});
            content.push_back({"up " + uptime_str() + "   ", C_DIM});
            content.push_back({"refresh " + std::to_string(o.interval) + "s", C_DIM});
            if (o.paused) content.push_back({"   PAUSED", C_WARN});
        } else {
            content.push_back({o.status, C_DIM});
        }
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
        bar_row(f, desc, c.cpu.usage, "%", true);   // загрузка: 100% — нормально (до зелёного)
        bar_row(f, {}, c.cpu.temperature, "°C");    // температура: 100°C — поломка (до красного)
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
        bar_row(f, desc, c.ram.usage_percent, "%");  // 100% памяти — неисправность
    }

    // GPU
    for (size_t i = 0; i < c.gpus.size(); i++) {
        const auto& g = c.gpus[i];
        box_sep(f);
        box_top(f, "GPU " + std::to_string(g.index) + " — " + g.name);
        double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : -1.0;
        // загрузка GPU: 100% — нормально (до зелёного)
        std::vector<Seg> desc;
        if (g.clock_mhz >= 0) desc.push_back({maybe(g.clock_mhz, " MHz"), C_DIM});
        if (g.power_watts >= 0) desc.push_back({(desc.empty() ? "" : "  ") + maybe(g.power_watts, " W"), C_DIM});
        bar_row(f, desc, g.utilization, "%", true);
        // VRAM: 100% — память заполнена (до красного)
        char vb[64];
        snprintf(vb, sizeof(vb), "vram %5.1f/%5.1f GiB", g.vram_used_gb, g.vram_total_gb);
        bar_row(f, {{vb, C_TXT}}, vram_pct, "%");
        // температура
        bar_row(f, {}, g.temperature, "°C");
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
            if (d.by_partitions)
                snprintf(sz, sizeof(sz), "%7.1f GiB", d.size_gb);   // ФС не смонтирована — только размер
            else if (d.usage_percent >= 0)
                snprintf(sz, sizeof(sz), "%7.1f / %7.1f GiB", d.used_gb, d.total_gb);
            else
                snprintf(sz, sizeof(sz), "%7.1f GiB", d.size_gb);
            desc.push_back({sz, C_TXT});
            if (d.by_partitions && !d.fs.empty()) desc.push_back({"  [" + d.fs + "]", C_DIM});
            if (!d.model.empty()) desc.push_back({"  " + d.model, C_DIM});
            if (d.by_partitions) {
                // смонтированной ФС нет — точная занятость неизвестна: «unknown» вместо бара
                std::vector<Seg> line;
                line.push_back({"│ ", C_SEC});
                for (auto& s : fit_width(desc, W - 11)) line.push_back(s);
                int used = 0;
                for (auto& s : line) used += disp_width(s.t);
                if (used <= W - 8) line.push_back({std::string(W - used - 8, ' ') + "unknown", C_DIM});
                line.push_back({"│", C_SEC});
                f.push_back(line);
            } else {
                bar_row(f, desc, d.usage_percent, "%");  // 100% диска — неисправность
            }
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

    // LLM — в самом конце; показываем только если запущен сервер
    if (!c.llms.empty()) {
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
                bar_row(f, desc, l.ctx_percent, "%");  // ctx заполнен — слот занят/поломка
            }
            // cache reuse / spec accept — бары (100% — хорошо, до зелёного)
            bar_row(f, {{"cache reuse", C_TXT}}, l.cache_reuse_percent, "%", true);
            bar_row(f, {{"spec accept", C_TXT}}, l.spec_accept_percent, "%", true);

            // gen/prefill и busy/reqs — двумя строками (сглажено: среднее из 5 + удержание 60с после нуля)
            {
                std::vector<Seg> row;
                row.push_back({"gen " + (l.gen_tps >= 0 ? fmt1(l.gen_tps) : std::string("—")) + " t/s   ", C_TXT});
                row.push_back({"prefill " + (l.prefill_tps >= 0 ? fmt1(l.prefill_tps) : std::string("—")) + " t/s", C_TXT});
                box_row(f, row);
                row.clear();
                row.push_back({"busy " + (l.busy_slots >= 0 ? fmt1(l.busy_slots) : std::string("—")) + "   ", C_DIM});
                row.push_back({"reqs " + (l.requests_processing >= 0 ? fmt1(l.requests_processing) : std::string("—")), C_DIM});
                if (!l.metrics_ok) row.push_back({"   (метрики недоступны)", C_WARN});
                box_row(f, row);
            }
        }
    }

    box_bottom(f);

    // footer
    if (o.footer) {
        std::vector<Seg> line;
        line.push_back({" q — выход   r — пауза/продолжить" + o.footer_extra, C_DIM});
        f.push_back(line);
    }

    W = savedW;
    return f;
}

// remote-кадр: тот же build_frame, но со своим заголовком/статусом, без футера
std::vector<std::vector<Seg>> remote_frame(const Collector& r, const TuiState& st, int width) {
    std::string title = "sysmon — remote: " +
        (r.host.empty() ? st.remote_addr : r.host + " (" + st.remote_addr + ")");
    if (!st.remote_ok) {
        std::vector<std::vector<Seg>> f;
        box_top(f, title);
        box_row(f, {{"недоступен", C_WARN}});
        box_bottom(f);
        return f;
    }
    FrameOpts o;
    o.width = width;
    o.title = title;
    o.status = r.ts + "   up " + uptime_from_s(r.uptime_s) + "   (snapshot)";
    o.footer = false;
    return build_frame(r, o);
}

// два кадра в ряд (left + right), строка за строкой; короче — дополнить пробелами
std::vector<std::vector<Seg>> merge_h(const std::vector<std::vector<Seg>>& a,
                                      const std::vector<std::vector<Seg>>& b, int width) {
    std::vector<std::vector<Seg>> out;
    size_t n = std::max(a.size(), b.size());
    int half = width / 2;
    for (size_t y = 0; y < n; y++) {
        std::vector<Seg> row;
        if (y < a.size()) {
            for (const auto& s : a[y]) row.push_back(s);
            int used = 0;
            for (const auto& s : row) used += disp_width(s.t);
            if (used < half) row.push_back({std::string(half - used, ' '), C_TXT});
        } else {
            row.push_back({std::string(half, ' '), C_TXT});   // левый кадр короче — правый не «съезжает» влево
        }
        if (y < b.size()) {
            for (const auto& s : b[y]) row.push_back(s);
            int used = 0;
            for (const auto& s : row) used += disp_width(s.t);
            if (used < width) row.push_back({std::string(width - used, ' '), C_TXT});
        }
        out.push_back(std::move(row));
    }
    return out;
}

static const char* mode_name(int mode) {
    static const char* names[] = {"отключено", "снизу", "справа"};
    return names[mode];
}

std::string footer_extra(const TuiState& st) {
    if (st.remote_addr.empty()) return "";
    return std::string("   m — remote: ") + mode_name(st.remote_mode);
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
    // курсор — в угол и скрыть, чтобы не «горел» после выхода
    move(LINES - 1, COLS - 1);
    curs_set(0);
    endwin();
}

bool tui_frame(const Collector& local, const Collector* remote, TuiState& st) {
    int full = std::min(COLS, 100);
    if (full < 40) full = COLS;
    W = full;

    bool show_remote = !st.remote_addr.empty() && st.remote_mode != 0 && remote != nullptr;
    if (getenv("SYSMON_DEBUG"))
        fprintf(stderr, "[sysmon] frame: addr='%s' mode=%d remote=%p ok=%d\n",
                st.remote_addr.c_str(), st.remote_mode, (const void*)remote, (int)st.remote_ok);

    std::vector<std::vector<Seg>> frame;
    if (show_remote && st.remote_mode == 2) {
        // справа: два кадра в ряд, каждый — половина ширины (расширяются/сужаются вместе с окном)
        int half = full / 2;
        FrameOpts lo;
        lo.width = half;
        lo.interval = st.interval;
        lo.paused = st.paused;
        lo.footer = false;          // футер рисуется отдельно — на последней строке окна
        auto lf = build_frame(local, lo);
        auto rf = remote_frame(*remote, st, half);
        frame = merge_h(lf, rf, full);
    } else {
        FrameOpts lo;
        lo.width = full;
        lo.interval = st.interval;
        lo.paused = st.paused;
        lo.footer = false;
        frame = build_frame(local, lo);
        if (show_remote) {
            // снизу: remote-кадр под локальным
            auto rf = remote_frame(*remote, st, full);
            for (auto& line : rf) frame.push_back(line);
        }
    }

    erase();
    for (size_t y = 0; y < frame.size() && (int)y < LINES; y++)
        draw_line((int)y, frame[y]);
    // футер — всегда на последней строке окна (при расширении окна остаётся внизу)
    if (LINES >= 2)
        draw_line(LINES - 1, {{std::string(" q — выход   r — пауза/продолжить") + footer_extra(st), C_DIM}});
    refresh();
    curs_set(0);   // курсор может «проявляться» на некоторых терминалах — прячем каждый кадр

    timeout(st.interval * 1000);
    int ch = getch();
    timeout(-1);
    // ncurses может возвращать кириллицу парой UTF-8-байтов — собираем в символ
    // (й = D0 B9, к = D0 BA, ь = D1 9F)
    if (ch == 0xD0 || ch == 0xD1) {
        timeout(100);          // второй байт UTF-8 приходит мгновенно — не блокируемся навсегда
        int ch2 = getch();
        timeout(-1);
        if (ch == 0xD0 && ch2 == 0xB9) ch = 0x0439;      // й
        else if (ch == 0xD0 && ch2 == 0xBA) ch = 0x043A; // к
        else if (ch == 0xD1 && ch2 == 0x9F) ch = 0x044F; // ь
        else ungetch(ch2);
    }
    switch (ch) {
        case 'q': case 'Q': case 0x0439 /* й = q (рус.) */: case 27: case 3: return false;
        case 'r': case 'R': case 0x043A /* к = r (рус.) */: case ' ': st.paused = !st.paused; break;
        case 'm': case 'M': case 0x044F /* ь = m (рус.) */:
            // справа → снизу → отключено → справа
            if (!st.remote_addr.empty()) st.remote_mode = (st.remote_mode + 2) % 3;
            break;
        default: break;
    }
    return true;
}
