#include "tui.h"

#include <ncursesw/ncurses.h>
#ifdef _WIN32
#include <winsock2.h>   // gethostname
#endif
#include <unistd.h>

#ifndef CURSOR_INVISIBLE
#define CURSOR_INVISIBLE 2
#endif
#ifndef COLOR_GRAY
#define COLOR_GRAY COLOR_BLACK
#endif
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <algorithm>
#include <functional>

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
const attr_t C_ACT   = COLOR_PAIR(3) | A_BOLD;   // v2.0: жёлтый — активная фаза LLM (gen/prefill)

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

// Строка ровно в maxw колонок (не шире COLS); нижняя строка экрана — на колонку короче,
// чтобы не писать в правую нижнюю клетку (ncurses: затир/скролл)
void draw_line(int y, const std::vector<Seg>& segs, int maxw) {
    if (y < 0 || y >= LINES) return;
    if (maxw > COLS) maxw = COLS;
    if (y == LINES - 1 && maxw > COLS - 1) maxw = COLS - 1;
    if (maxw <= 0) return;
    if (getenv("SYSMON_DEBUG"))
        fprintf(stderr, "[tui] line %d maxw=%d COLS=%d LINES=%d\n", y, maxw, COLS, LINES);
    move(y, 0);
    int x = 0;
    for (const auto& s : segs) {
        if (x >= maxw) break;
        // обрезать сегмент по колонкам (addnstr с n криво считает multi-byte)
        size_t i = 0, used = 0;
        int cols = 0;
        while (i < s.t.size() && cols < maxw - x) {
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

std::string now_str() {
    time_t t = time(nullptr);
    char b[32];
    strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", localtime(&t));
    return b;
}

// v2.0: кредит — внизу, в футере (в шапке больше не рисуется)
// v2.2g (Шеф 02.11 14:53): подпись — ОТДЕЛЬНОЙ строкой под подсказкой клавиш,
// в одну строку с подсказкой она не читалась.
static const std::string CREDIT = " Designed: Chief (man), Gaechka, Chip (Openclaw)";
static const std::string CREDIT_SHORT = " Des.: Chief (man), Gaechka, Chip (Openclaw)";

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

std::string trunc_disp(const std::string& s, int cols) {
    auto v = fit_width({{s, C_TXT}}, cols);
    return v.empty() ? std::string() : v[0].t;
}

// v2.2c: компактные единицы для io-строк (без выравнивающего отступа),
// чтобы в узкой remote-панели влезало «io sda ↓ 32.4 KB/s».
static std::string io_rate(double mbps) {
    char b[24];
    if (mbps < 0.1) snprintf(b, sizeof(b), "%.1f KB/s", mbps * 1024.0);
    else            snprintf(b, sizeof(b), "%.1f MB/s", mbps);
    return b;
}

// v2.2d (Шеф 02.11): адаптивный бар активности диска. Раньше бар = busy% ядра, и он
// почти всегда 0 (на X99 с ядром 6.8 счётчик «ms doing I/O» у NVMe вообще не ходит).
// Теперь бар — доля от реального потолка скорости устройства, в ЛОГАРИФМИЧЕСКОЙ шкале
// (диапазон скоростей три порядка: 0.1 МБ/с … 1 ГБ/с). Число слева от бара — по-прежнему
// busy% ядра («—» если ядро его не считает).
static double io_ceiling(const DiskStats& d) {
    if (d.type == "NVMe") return 1200.0;   // PCIe3/4 x4 NVMe
    if (d.type == "SSD")  return 550.0;    // SATA SSD
    if (d.type == "HDD")  return 180.0;    // 7200 об/мин
    return 600.0;
}

static double io_bar_pct(double mbps, const DiskStats& d) {
    if (mbps <= 0.01) return 0.0;
    double f = std::log10(1.0 + mbps) / std::log10(1.0 + io_ceiling(d));
    if (f > 1.0) f = 1.0;
    return f * 100.0;
}

// v2.2e (Шеф 02.11): «былой максимум» на баре io — вертикальная полоска там, где был
// пик активности. Максимум спадает плавно (за ~90 с уменьшается вдвое), поэтому в паузе
// видно «как буянил» диск: [░░░░│░░░░░]. Состояние — на панель (tag = IP машины) + диск.
struct IoPeak { double pct = 0.0; std::chrono::steady_clock::time_point ts{}; };
static std::map<std::string, IoPeak> g_io_peak;

static double io_peak_pct(const std::string& tag, const std::string& disk, double cur) {
    IoPeak& p = g_io_peak[tag + "|" + disk];
    auto now = std::chrono::steady_clock::now();
    if (p.ts.time_since_epoch().count() == 0) { p.ts = now; p.pct = cur; return p.pct; }
    double dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.ts).count() / 1000.0;
    p.ts = now;
    if (dt > 0) p.pct *= std::exp(-dt / 90.0);       // полураспад ~90 с
    if (cur > p.pct) p.pct = cur;
    if (p.pct < 1.5) p.pct = 0.0;                    // совсем тихо — полоску не рисуем
    return p.pct;
}

// бар с меткой максимума: залитая часть = текущая активность, «│» = былой пик
static void add_bar_peak(std::vector<Seg>& row, double pct, double peak_pct, int width) {
    int filled = (pct < 0) ? 0 : (int)(pct / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    int mark = (peak_pct > 0) ? (int)(peak_pct / 100.0 * width + 0.5) - 1 : -1;
    if (mark >= width) mark = width - 1;
    if (mark < filled || mark < 1) mark = -1;        // пик = текущий уровень или слишком мал — метки нет
    row.push_back({"[", C_TXT});
    for (int i = 0; i < width; i++) {
        double r = width > 1 ? (double)i / (width - 1) : 1.0;
        if (i < filled)      row.push_back({"\u2588", grad_attr(r, false)});
        else if (i == mark)  row.push_back({"\u2502", C_SEC});
        else                 row.push_back({"\u2591", C_TRACK});
    }
    row.push_back({"]", C_TXT});
}

// v2.0: заголовок секции — ОБРЕЗКА длинного текста (перенос строки убран:
// именно он ломал рамки на двухпанельном экране; директива Шефа 21.10.2026)
void box_top(std::vector<std::vector<Seg>>& f, const std::string& title) {
    int inner = W - 4;
    if (inner < 4) inner = 4;
    std::string t = trunc_disp(title, inner);
    std::vector<Seg> line;
    line.push_back({"┌─ ", C_SEC});
    line.push_back({t, C_HDR});
    line.push_back({repeat("─", std::max(1, inner - disp_width(t))), C_SEC});
    line.push_back({"┐", C_SEC});
    f.push_back(line);
}

void box_sep(std::vector<std::vector<Seg>>& f) {
    f.push_back({{"├", C_SEC}, {repeat("─", std::max(1, W - 2)), C_SEC}, {"┤", C_SEC}});
}

void box_bottom(std::vector<std::vector<Seg>>& f) {
    f.push_back({{"└", C_SEC}, {repeat("─", std::max(1, W - 2)), C_SEC}, {"┘", C_SEC}});
}

// v2.2c: MOE-строка длиннее панели — переносим по разделителям «·», а не режем:
// иначе в узкой remote-панели теряются «cache …» и «VRAM free …».
static void box_row(std::vector<std::vector<Seg>>& f, const std::vector<Seg>& content);

void moe_rows(std::vector<std::vector<Seg>>& f, const std::string& moe) {
    int maxw = W - 3;
    if (maxw < 24) maxw = 24;
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < moe.size()) {
        size_t sep = moe.find("\u00b7", pos);          // «·» = 2 байта в UTF-8
        std::string t = (sep == std::string::npos) ? moe.substr(pos) : moe.substr(pos, sep - pos);
        size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        if (a != std::string::npos) parts.push_back(t.substr(a, b - a + 1));
        if (sep == std::string::npos) break;
        pos = sep + 2;
    }
    std::vector<std::string> lines;
    std::string cur = "MOE ";
    for (auto& p : parts) {
        std::string cand = (cur.size() > 4) ? cur + "  ·  " + p : cur + p;
        if (disp_width(cand) > maxw && cur.size() > 4) { lines.push_back(cur); cur = "     " + p; }
        else cur = cand;
    }
    if (cur.size() > 4) lines.push_back(cur);
    if (lines.empty()) lines.push_back("MOE " + moe);
    for (auto& s : lines) box_row(f, {{s, C_DIM}});
}

// v2.2d (Шеф 02.11): компактный MOE-блок. Было 4 строки (experts / CPU-prefill / cache /
// VRAM free + mem) — стало две: «GPU MOE exp. 24576/24576» и «CPU-prefill = off   cache = 46.8 Gb».
// VRAM free убран из TUI: остаток VRAM и так виден в GPU-панели выше (в JSON поле осталось).
// Если структурированных полей нет (удалённый сервер старой версии) — прежний перенос по «·».
static void moe_compact(std::vector<std::vector<Seg>>& f, const LLMStats& l) {
    if (l.moe_experts < 0 && l.moe_cpu_prefill < 0 && l.moe_cache_gib < 0) { moe_rows(f, l.moe); return; }
    std::string p1;
    if (l.moe_experts > 0) {
        p1 = (l.moe_place == "host") ? "CPU MOE exp. " : "GPU MOE exp. ";
        p1 += std::to_string(l.moe_experts);
        if (l.moe_total > 0) p1 += "/" + std::to_string(l.moe_total);
    }
    std::string p2;
    if (l.moe_cpu_prefill >= 0) p2 = std::string("CPU-prefill = ") + (l.moe_cpu_prefill ? "on" : "off");
    if (l.moe_cache_gib > 0) {
        // v2.3 (Шеф 03.11): показываем РЕАЛЬНО выделенные слоты кэша, а не полное число
        // экспертов модели: «cache 24036 сл / 45.8 Gb». Число в «exp.» — резидентные эксперты.
        char b[64];
        if (l.moe_cache_slots > 0)
            snprintf(b, sizeof(b), "cache %d сл / %.1f Gb", l.moe_cache_slots, l.moe_cache_gib);
        else
            snprintf(b, sizeof(b), "cache = %.1f Gb", l.moe_cache_gib);
        p2 += (p2.empty() ? "" : "   ") + std::string(b);
    }
    // v2.2h (Шеф 02.11 14:59): порядок — сначала «CPU-prefill … cache …» строкой,
    // затем «GPU MOE exp.»; раньше при переносе узкой панели части p2 перемешивались
    // со строкой experts (получалось CPU-prefill / MOE exp / cache).
    if (!p2.empty()) {
        size_t sp = p2.find("   ");
        if (disp_width(p2) <= W - 3 || sp == std::string::npos) {
            box_row(f, {{p2, C_DIM}});
        } else {
            box_row(f, {{p2.substr(0, sp), C_DIM}});
            box_row(f, {{p2.substr(sp + 3), C_DIM}});
        }
    }
    if (!p1.empty()) box_row(f, {{p1, C_DIM}});
}

// v2.2d (Шеф 02.11): RAM/SWAP процесса модели — ДВА бара в одну строку вместо «mem RAM ... swap ...».
// Шкала — от системного тотала: RAM 11.5G из 121.5G ≈ 9%. Для Strata строка не рисуется вовсе
// (просьба Шефа: расход памяти движка и так виден из системных панелей RAM/GPU выше).
static void mem_bars(std::vector<std::vector<Seg>>& f, const LLMStats& l, const RAMStats& r) {
    if (l.rss_gb < 0 || l.engine == "strata") return;
    double swp = l.swap_gb >= 0 ? l.swap_gb : 0.0;
    int bw = (W - 6 - 26) / 2;
    if (bw < 3) {                                       // экстремально узко — только числа
        char mb[48]; snprintf(mb, sizeof(mb), "RAM %4.1f Gb  SWAP %4.1f Gb", l.rss_gb, swp);
        box_row(f, {{mb, swp > 0.5 ? C_ACT : C_DIM}});
        return;
    }
    if (bw > 10) bw = 10;
    char a1[24], a2[24];
    snprintf(a1, sizeof(a1), "RAM %4.1fG ", l.rss_gb);
    snprintf(a2, sizeof(a2), "SWAP %4.1fG ", swp);
    double p1 = (r.total_gb > 0) ? l.rss_gb / r.total_gb * 100.0 : -1.0;
    double p2 = (r.swap_total_gb > 0) ? swp / r.swap_total_gb * 100.0 : -1.0;
    std::vector<Seg> row;
    row.push_back({a1, C_TXT});
    add_bar(row, p1, bw);
    row.push_back({" ", C_TXT});
    row.push_back({a2, C_TXT});
    add_bar(row, p2, bw);                                // Своп подсвечивает градиент: чем больше — тем тревожнее
    box_row(f, row);
}

static void box_row(std::vector<std::vector<Seg>>& f, const std::vector<Seg>& content) {
    std::vector<Seg> line;
    line.push_back({"│ ", C_SEC});
    for (auto& s : fit_width(content, W - 3)) line.push_back(s);
    line.push_back({"│", C_SEC});
    f.push_back(line);
}

// Строка-бар: описание (слева) + значение (левое выравнивание в 5 кол.) + бар у правого края.
// Все бары выровнены в одну колонку. value<0 → «—» и пустой бар. unit = "%" или "°C".
// safe=true — «безопасная» гамма (до зелёного): 100% нормально.
static int bar_len(int w) {
    int n = 24 * w / 100;
    if (n < 8) n = 8;
    if (n > 24) n = 24;
    return n;
}

// v2.2d (Шеф 02.11): постоянная io-строка диска (рядом с bar_row — те же колонки,
// поэтому бары стоят в одной линии со всеми остальными барами панели).
// Пауза: «io sda idle» + пустой бар. Работа: ↓/↑ и бар от лог-шкалы скорости.
void io_row(std::vector<std::vector<Seg>>& f, const DiskStats& d, const std::string& tag) {
    double r = d.read_mbs > 0 ? d.read_mbs : 0.0;
    double w = d.write_mbs > 0 ? d.write_mbs : 0.0;
    double tot = r + w;
    bool act = (tot > 0.05) || (d.busy_percent > 2.0);
    int bl = bar_len(W);
    int desc_w = W - 11 - bl - 1;
    if (desc_w < 8) desc_w = 8;
    std::string head = "io " + std::string(d.name);
    std::vector<Seg> desc;
    if (!act) {
        desc.push_back({head + "  ", C_DIM});
        desc.push_back({"idle", C_DIM});
    } else {
        std::string rs = io_rate(r), ws = io_rate(w);
        bool both = (disp_width(head + "  \u2193 " + rs + "  \u2191 " + ws) <= desc_w);
        bool show_r = both ? true : (r >= w);       // узкая панель — ТОТ поток, что грузит диск
        desc.push_back({head + " ", C_DIM});
        desc.push_back({show_r ? "\u2193 " : "\u2191 ", C_SEC});
        desc.push_back({show_r ? rs : ws, C_TXT});
        if (both) {
            desc.push_back({"  \u2191 ", C_SEC});
            desc.push_back({ws, C_TXT});
        }
    }
    std::vector<Seg> line;
    line.push_back({"\u2502 ", C_SEC});
    for (auto& s : fit_width(desc, desc_w)) line.push_back(s);
    line.push_back({" ", C_TXT});
    std::string vs; attr_t va = C_DIM;
    if (act && d.busy_percent >= 0) {               // число = busy% ядра (— если ядро не считает)
        char b[16]; snprintf(b, sizeof(b), "%.0f%%", d.busy_percent);
        vs = b; va = pct_attr(d.busy_percent);
    } else {
        vs = "\u2014";
    }
    int vw = disp_width(vs);
    if (vw < 5) vs = vs + std::string(5 - vw, ' ');
    line.push_back({vs, va});
    line.push_back({" ", C_TXT});
    double cur_pct = act ? io_bar_pct(tot, d) : -1.0;
    double peak = io_peak_pct(tag, d.name, cur_pct > 0 ? cur_pct : 0.0);
    add_bar_peak(line, cur_pct, peak, bl);
    line.push_back({"\u2502", C_SEC});
    f.push_back(fit_width(line, W));
}

void bar_row(std::vector<std::vector<Seg>>& f, std::vector<Seg> desc, double value,
             const std::string& unit = "%", bool safe = false) {
    std::vector<Seg> line;
    line.push_back({"│ ", C_SEC});
    int bl = bar_len(W);
    int desc_w = W - 11 - bl;                  // "│ "(2) + значение(5) + " "(1) + "["(1) + бар + "]"(1) + "│"(1)
    if (desc_w < 4) desc_w = 4;                // экстремально узкий кадр
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
    // жёсткая подгонка: строка не может быть шире панели (защита рамок)
    f.push_back(fit_width(line, W));
}

// v2.0: вертикальные индикаторы загрузки ядер — по глифу на ядро (▁▂▃▄▅▆▇█,
// нижнее выравнивание, 8 уровней). Если ядер больше, чем доступных колонок —
// агрегация в слоты по максимуму. Цвет — «безопасный» градиент.
std::vector<Seg> core_glyphs(const Collector& c, int panel_w) {
    const std::vector<double>& cu = c.cpu.core_usage;
    if (cu.empty()) return {};
    int avail = panel_w - 13 - bar_len(panel_w);   // «окошко» описания в bar_row
    if (avail < 4) avail = 4;
    int n = (int)cu.size();
    static const char* G[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    bool spaced = avail >= n * 2;                  // просторно — разобьём глифы пробелами
    int slots = spaced ? n : std::min(avail, n);
    if (slots < 1) slots = 1;
    int group = (n + slots - 1) / slots;
    std::vector<Seg> segs;
    for (int s0 = 0; s0 < slots; s0++) {
        double v = 0;
        for (int k = 0; k < group; k++) {
            int idx = s0 * group + k;
            if (idx < n && cu[idx] > v) v = cu[idx];
        }
        int lvl = (int)(v / 100.0 * 8.0 + 0.999);  // ceil: любая nonzero загрузка ≥ ▁
        if (lvl < 0) lvl = 0;
        if (lvl > 8) lvl = 8;
        if (s0 && spaced) segs.push_back({" ", C_TXT});
        segs.push_back({G[lvl], grad_attr(v / 100.0, true)});
    }
    return segs;
}

// v2.0: адаптивные единицы скорости сети: малые значения — в KB/s (иначе всегда «0.0»)
std::string rate_str(double mbps) {
    char b[32];
    if (mbps < 0.1) snprintf(b, sizeof(b), "%6.1f KB/s", mbps * 1024.0);
    else            snprintf(b, sizeof(b), "%5.1f MB/s", mbps);
    return b;
}

// Параметры кадра (локальный и remote-кадр строятся одним кодом)
struct FrameOpts {
    int width = 100;
    std::string title;        // заголовок панели — IP машины
    std::string status_line;  // «дд.мм.гггг чч:мм:сс  up ...» (время самой машины)
    bool paused = false;
};

std::vector<std::vector<Seg>> build_frame(const Collector& c, const FrameOpts& o) {
    int savedW = W;
    W = o.width;
    std::vector<std::vector<Seg>> f;

    // шапка: только IP машины (без SYSMON/имён); строка статуса — локальное время + uptime
    box_top(f, o.title);
    {
        std::vector<Seg> content;
        content.push_back({o.status_line, C_TXT});
        if (o.paused) content.push_back({"   PAUSED", C_WARN});
        box_row(f, content);
    }

    // CPU + RAM (v2.0: RAM внутри блока CPU одной строкой; load avg убран;
    // вместо общего % — вертикальные индикаторы по ядрам)
    box_sep(f);
    box_top(f, c.cpu.model.empty() ? "CPU" : "CPU — " + c.cpu.model);
    {
        std::vector<Seg> desc = core_glyphs(c, W);
        if (desc.empty()) {
            // нет per-core (Windows / нет данных) — общий %
            char lb[32]; snprintf(lb, sizeof(lb), "%.0f%%", c.cpu.usage);
            desc.push_back({lb, C_TXT});
        }
        bar_row(f, desc, c.cpu.temperature, "°C");    // температура: 100°C — поломка (до красного)
        char rb[64];
        snprintf(rb, sizeof(rb), "RAM %5.1f / %5.1f Gb", c.ram.used_gb, c.ram.total_gb);
        bar_row(f, {{rb, C_TXT}}, c.ram.usage_percent, "%");   // 100% памяти — неисправность
    }

    // GPU
    for (size_t i = 0; i < c.gpus.size(); i++) {
        const auto& g = c.gpus[i];
        box_sep(f);
        box_top(f, "GPU " + std::to_string(g.index) + " — " + g.name +
                        (g.pcie.empty() ? "" : ("  \u00b7  " + g.pcie)));   // v2.2: как карта подключена
        double vram_pct = (g.vram_total_gb > 0) ? 100.0 * g.vram_used_gb / g.vram_total_gb : -1.0;
        std::vector<Seg> desc;
        if (g.clock_mhz >= 0) desc.push_back({maybe(g.clock_mhz, " MHz"), C_DIM});
        if (g.power_watts >= 0) desc.push_back({(desc.empty() ? "" : "  ") + maybe(g.power_watts, " W"), C_DIM});
        bar_row(f, desc, g.utilization, "%", true);   // загрузка GPU: 100% — нормально
        char vb[64];
        snprintf(vb, sizeof(vb), "vram %5.1f/%5.1f Gb", g.vram_used_gb, g.vram_total_gb);
        bar_row(f, {{vb, C_TXT}}, vram_pct, "%");
        bar_row(f, {}, g.temperature, "°C");
    }
    if (c.gpus.empty()) {
        box_sep(f);
        box_top(f, "GPU");
        box_row(f, {{"не найдена (нет nvidia-smi и AMD sysfs)", C_DIM}});
    }

    // LLM — сразу за GPU; главная модель первой (сортировка в collect_llm: ctx DESC)
    for (const auto& l : c.llms) {
        box_sep(f);
        // v2.2 (Шеф 02.11): Strata подписывается своим именем, llama остаётся LLM
        box_top(f, (l.engine == "strata" ? "Strata : " : "LLM : ") +
                   std::to_string(l.port) + " \u2014 " + l.model_name);
        {
            std::vector<Seg> desc;
            if (l.ctx_limit > 0)
                desc.push_back({"ctx " + fmt_tok(l.ctx_used) + "/" + fmt_tok(l.ctx_limit) + " tok", C_TXT});
            else
                desc.push_back({"ctx", C_TXT});
            bar_row(f, desc, l.ctx_percent, "%");
        }
        bar_row(f, {{"cache reuse", C_TXT}}, l.cache_reuse_percent, "%", true);
        bar_row(f, {{"spec accept", C_TXT}}, l.spec_accept_percent, "%", true);
        {
            // v2.0: активная фаза — жёлтым (gen / prefill)
            std::vector<Seg> row;
            char gb[48];
            if (l.gen_tps > 0) snprintf(gb, sizeof(gb), "decode gen.%6s t/s", fmt1(l.gen_tps).c_str());
            else snprintf(gb, sizeof(gb), "decode gen.   —   t/s");
            row.push_back({gb, l.active == "gen" ? C_ACT : C_TXT});
            box_row(f, row);
            row.clear();
            char pb[64];
            if (l.prefill_tps >= 0 && l.prefill2_tps > 0)
                snprintf(pb, sizeof(pb), "prefill upl.%6s / gen.%6s t/s", fmt1(l.prefill_tps).c_str(), fmt1(l.prefill2_tps).c_str());
            else if (l.prefill_tps >= 0)
                snprintf(pb, sizeof(pb), "prefill upl.%6s / gen.   —   t/s", fmt1(l.prefill_tps).c_str());
            else
                snprintf(pb, sizeof(pb), "prefill upl.   —   / gen.   —   t/s");
            row.push_back({pb, l.active == "prefill" ? C_ACT : C_TXT});
            box_row(f, row);
        }
        // v1.11 (Шеф 27.10): краткие подписи task/n_t/trnc (старые не влезали —
        // «trunc» обрезалось); значения только текущие, без средних;
        // при отсутствии данных: task — прочерк (держим последний), n_t/trnc — 0.
        {
            // v1.12 (Шеф 27.10): на ходу задача — строка живая и ЖЁЛТАЯ: n_t считается
            // тем же чтением лога, что tg/progress (прогресс префилла + n_gen), trnc=0
            // (неизвестно до конца задачи). В паузе: последний task, фактическое
            // заполнение контекста, trunc завершённой задачи; нет данных — 0/прочерк.
            char tb[64];
            bool act = l.task_live;
            snprintf(tb, sizeof(tb), "task %s   n_t %s   trnc %d",
                     l.last_task > 0 ? std::to_string(l.last_task).c_str() : "\u2014",
                     fmt_tok(act ? l.task_n_live : l.ctx_used).c_str(),
                     act ? 0 : (l.last_truncated >= 0 ? l.last_truncated : 0));
            box_row(f, {{tb, act ? C_ACT : C_DIM}});
        }
        // v2.2: MOE-размещение (Strata) — куда легли эксперты
        // v2.2d: компактно; VRAM free и строка mem для Strata убраны (дублируют системные панели)
        if (!l.moe.empty() || l.moe_experts > 0)
            moe_compact(f, l);
        // v2.2: RAM/своп процесса модели; v2.2d — два бара в одну строку (не для Strata)
        mem_bars(f, l, c.ram);
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
            // компактная/богатая форма по доступной ширине (в узкой панели числа
            // не должны обрезаться посередине — «585.3 /   9»);
            // rich форматы с «Gb» — директива Шефа
            int avail = W - 12 - bar_len(W);   // ширина описания в bar_row
            char sz[64];
            bool sizes = !d.by_partitions && d.usage_percent >= 0;
            if (avail >= 40 && sizes && !d.model.empty())
                snprintf(sz, sizeof(sz), "%7.1f / %7.1f Gb  %s", d.used_gb, d.total_gb, d.model.c_str());
            else if (sizes && avail >= 26)
                snprintf(sz, sizeof(sz), "%5.0f/%5.0f Gb", d.used_gb, d.total_gb);
            else if (sizes)
                snprintf(sz, sizeof(sz), "%4.0f/%4.0fG", d.used_gb, d.total_gb);
            else if (d.by_partitions && avail >= 26)
                snprintf(sz, sizeof(sz), "%6.1f Gb  [%s]%s", d.size_gb, d.fs.c_str(),
                         d.model.empty() ? "" : ("  " + d.model).c_str());
            else
                snprintf(sz, sizeof(sz), "%5.1f Gb", d.size_gb);
            desc.push_back({sz, C_TXT});
            if (d.by_partitions) {
                std::vector<Seg> line;
                line.push_back({"\u2502 ", C_SEC});
                for (auto& s : fit_width(desc, W - 11)) line.push_back(s);
                int used = 0;
                for (auto& s : line) used += disp_width(s.t);
                if (used <= W - 8) line.push_back({std::string(W - used - 8, ' ') + "unknown", C_DIM});
                line.push_back({"\u2502", C_SEC});
                f.push_back(fit_width(line, W));
            } else {
                bar_row(f, desc, d.usage_percent, "%");
            }
            // v2.2d (Шеф 02.11): строка io под каждым диском ПОСТОЯННА (раньше появлялась
            // только при активности и «прыгала» по высоте панели) — меняется только содержимое:
            // в паузе «idle», в работе — ↓/↑ и бар активности (лог-шкала от потолка устройства).
            io_row(f, d, o.title);
        }
    }

    // Network — в самом низу (сразу после Disks, директива Шефа 21.10.2026)
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
            row.push_back({rate_str(ifc.rx_mbps), C_TXT});
            row.push_back({"  ↑ ", C_SEC});
            row.push_back({rate_str(ifc.tx_mbps), C_TXT});
            box_row(f, row);
        }
    }

    box_bottom(f);

    W = savedW;
    return f;
}

// remote-кадр: время самой удалённой машины — тикает от snapshot+drift;
// при паузе замораживается
std::vector<std::vector<Seg>> remote_frame(const Collector& r, const TuiState& st, int width) {
    std::string title = st.remote_addr.empty() ? std::string("remote") : st.remote_addr;
    if (!st.remote_ok) {
        std::vector<std::vector<Seg>> f;
        int savedW = W; W = width;
        box_top(f, title);
        box_row(f, {{"недоступен", C_WARN}});
        box_bottom(f);
        W = savedW;
        return f;
    }
    double drift = 0;
    if (!st.paused && st.remote_fetch_wall > 0) {
        drift = (double)time(nullptr) - st.remote_fetch_wall;
        if (drift < 0) drift = 0;
    }
    std::string status;
    {
        int d, mo, yy, h, m, s;   // формат ts: «дд.мм.гггг чч:мм:сс» — ПЕРВЫМ идёт день!
        if (sscanf(r.ts.c_str(), "%d.%d.%d %d:%d:%d", &d, &mo, &yy, &h, &m, &s) == 6) {
            struct tm tm0;
            memset(&tm0, 0, sizeof(tm0));
            tm0.tm_year = yy - 1900; tm0.tm_mon = mo - 1; tm0.tm_mday = d;
            tm0.tm_hour = h; tm0.tm_min = m; tm0.tm_sec = s; tm0.tm_isdst = -1;
            time_t e = mktime(&tm0);
            if (e != (time_t)-1) e += (time_t)drift;
            char b[32];
            strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", localtime(&e));
            status = std::string(b) + "  up " + uptime_from_s(r.uptime_s + drift);
        } else {
            status = r.ts + "  up " + uptime_from_s(r.uptime_s);
        }
    }
    FrameOpts o;
    o.width = width;
    o.title = title;
    o.status_line = status;
    return build_frame(r, o);
}

// два кадра в ряд (left + right); строки ровно по ширине панелей (v2.0: переносов
// заголовков больше нет — «костыли» title_cont убраны)
std::vector<std::vector<Seg>> merge_h(const std::vector<std::vector<Seg>>& a,
                                      const std::vector<std::vector<Seg>>& b, int width) {
    std::vector<std::vector<Seg>> out;
    size_t n = std::max(a.size(), b.size());
    int half = width / 2;
    const char dash3[3] = {(char)0xE2, (char)0x94, (char)0x80};   // ─
    for (size_t y = 0; y < n; y++) {
        std::vector<Seg> row;
        if (y < a.size()) {
            for (const auto& s : a[y]) row.push_back(s);
            int used = 0;
            for (const auto& s : row) used += disp_width(s.t);
            if (used < half) row.push_back({repeat(dash3, half - used), C_TXT});   // ровный край
        } else {
            row.push_back({repeat(dash3, half), C_TXT});
        }
        if (y < b.size())
            for (const auto& s : b[y]) row.push_back(s);
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

// футер: ПРЕЖДЕ подсказка клавиш, ПОД НЕЙ отдельной строкой подпись авторов.
// lines = 2 (обычно) или 1 (очень низкий терминал — подпись жертвуется).
void draw_footer(const TuiState& st, int full, int lines) {
    if (LINES < 1) return;
    std::string keys = " q — выход   r — пауза/продолжить" + footer_extra(st);
    int lim = std::min(full, COLS);
    draw_line(LINES - lines, fit_width({{keys, C_DIM}}, lim), lim);
    if (lines < 2) return;
    for (const std::string& cr : { CREDIT, CREDIT_SHORT }) {
        if (disp_width(cr) <= lim) { draw_line(LINES - 1, {{cr, C_DIM}}, lim); return; }
    }
}

// локальная строка статуса: «дд.мм.гггг чч:мм:сс  up ...»; при паузе — заморожена
std::string local_status(const Collector& local, const TuiState& st, bool& frozen_valid,
                         std::string& frozen) {
    if (st.paused) {
        if (!frozen_valid) {
            frozen = now_str() + "  up " + uptime_from_s(local.uptime_s);
            frozen_valid = true;
        }
        return frozen;
    }
    frozen_valid = false;
    return now_str() + "  up " + uptime_from_s(local.uptime_s);
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
    init_extended_pair(3, 226, -1);   // яркий жёлтый (256-цв.)
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

bool tui_frame(const Collector& local, const Collector* remote, TuiState& st,
               const std::function<void()>& fast_refresh) {
    static bool frozen_valid = false;
    static std::string frozen;

    auto draw_all = [&]() {
        int full = COLS;
        if (full < 1) full = 1;
        W = full;
        bool show_remote = !st.remote_addr.empty() && st.remote_mode != 0 && remote != nullptr;
        bool right = show_remote && st.remote_mode == 2 && full >= 76;   // узкий терминал — стеком

        std::vector<std::vector<Seg>> frame;
        std::string lstat = local_status(local, st, frozen_valid, frozen);
        if (right) {
            int half = full / 2;
            FrameOpts lo;
            lo.width = half;
            lo.paused = st.paused;
            lo.title = local.ip;
            if (lo.title.empty()) lo.title = "local";
            lo.status_line = lstat;
            auto lf = build_frame(local, lo);
            auto rf = remote_frame(*remote, st, full - half);
            frame = merge_h(lf, rf, full);
        } else {
            FrameOpts lo;
            lo.width = full;
            lo.paused = st.paused;
            lo.title = local.ip;
            if (lo.title.empty()) lo.title = "local";
            lo.status_line = lstat;
            frame = build_frame(local, lo);
            if (show_remote) {
                auto rf = remote_frame(*remote, st, full);
                for (auto& line : rf) frame.push_back(line);
            }
        }
        erase();
        // футер — ДВЕ строки (клавиши + подпись); в очень низком терминале остаётся одна
        int flines = (LINES >= 5) ? 2 : 1;
        int ymax = LINES - flines;
        for (int y = 0; y < (int)frame.size() && y < ymax; y++)
            draw_line(y, frame[y], full);
        draw_footer(st, full, flines);
        refresh();
        curs_set(0);   // курсор может «проявляться» на некоторых терминалах — прячем каждый кадр
    };

    draw_all();

    // ожидание до следующего кадра: slice'ами по 500 мс — для суб-рефреша LLM/CPU
    // (реалтайв gen/prefill) и мгновенной реакции на смену размера окна
    const int total = st.interval * 1000;
    const int SLICE = 500;
    int waited = 0;
    while (waited < total) {
        int slice = std::min(SLICE, total - waited);
        timeout(slice);
        int ch = wgetch(stdscr);
        timeout(-1);
        if (ch == KEY_RESIZE) return true;    // перерисовать кадр под новый размер
        if (ch == ERR) {
            waited += slice;
            if (fast_refresh && !st.paused && waited < total - 100) {
                fast_refresh();
                draw_all();
            }
            continue;
        }
        // ncurses может возвращать кириллицу парой UTF-8-байтов — собираем в символ
        // (й = D0 B9, к = D0 BA, ь = D1 9F)
        if (ch == 0xD0 || ch == 0xD1) {
            timeout(100);
            int ch2 = getch();
            timeout(-1);
            if (ch == 0xD0 && ch2 == 0xB9) ch = 0x0439;      // й
            else if (ch == 0xD0 && ch2 == 0xBA) ch = 0x043A; // к
            else if (ch == 0xD1 && ch2 == 0x9F) ch = 0x044F; // ь
            else ungetch(ch2);
        }
        switch (ch) {
            case 'q': case 'Q': case 0x0439 /* й = q (рус.) */: case 27: case 3: return false;
            case 'r': case 'R': case 0x043A /* к = r (рус.) */: case ' ':
                st.paused = !st.paused;
                draw_all();
                break;
            case 'm': case 'M': case 0x044F /* ь = m (рус.) */:
                if (!st.remote_addr.empty()) st.remote_mode = (st.remote_mode + 2) % 3;
                draw_all();
                break;
            default: break;
        }
    }
    return true;
}
