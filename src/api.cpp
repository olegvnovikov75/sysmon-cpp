#include "api.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>

// ---------- JSON: сериализация ----------

static std::string jesc(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); r += b; }
                else r += (char)c;
        }
    }
    return r;
}

static std::string jnum(double v) {
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

std::string collector_to_json(const Collector& c) {
    std::string j;
    j.reserve(4096);
    j += "{";
    j += "\"host\":\"" + jesc(c.host) + "\",";
    j += "\"ts\":\"" + jesc(c.ts) + "\",";
    j += "\"uptime_s\":" + jnum(c.uptime_s) + ",";

    j += "\"cpu\":{";
    j += "\"usage\":" + jnum(c.cpu.usage) + ",";
    j += "\"temperature\":" + jnum(c.cpu.temperature) + ",";
    j += "\"load1\":" + jnum(c.cpu.load1) + ",";
    j += "\"load5\":" + jnum(c.cpu.load5) + ",";
    j += "\"load15\":" + jnum(c.cpu.load15) + ",";
    j += "\"cores\":" + jnum(c.cpu.cores) + ",";
    j += "\"model\":\"" + jesc(c.cpu.model) + "\"}";

    j += ",\"ram\":{";
    j += "\"used_gb\":" + jnum(c.ram.used_gb) + ",";
    j += "\"total_gb\":" + jnum(c.ram.total_gb) + ",";
    j += "\"usage_percent\":" + jnum(c.ram.usage_percent) + ",";
    j += "\"swap_used_gb\":" + jnum(c.ram.swap_used_gb) + ",";
    j += "\"swap_total_gb\":" + jnum(c.ram.swap_total_gb) + "}";

    j += ",\"gpus\":[";
    for (size_t i = 0; i < c.gpus.size(); i++) {
        const auto& g = c.gpus[i];
        if (i) j += ",";
        j += "{\"index\":" + jnum(g.index) + ",";
        j += "\"name\":\"" + jesc(g.name) + "\",";
        j += "\"vendor\":\"" + jesc(g.vendor) + "\",";
        j += "\"utilization\":" + jnum(g.utilization) + ",";
        j += "\"vram_used_gb\":" + jnum(g.vram_used_gb) + ",";
        j += "\"vram_total_gb\":" + jnum(g.vram_total_gb) + ",";
        j += "\"temperature\":" + jnum(g.temperature) + ",";
        j += "\"clock_mhz\":" + jnum(g.clock_mhz) + ",";
        j += "\"power_watts\":" + jnum(g.power_watts) + "}";
    }
    j += "],\"sensors\":[";
    for (size_t i = 0; i < c.sensors.size(); i++) {
        if (i) j += ",";
        j += "{\"name\":\"" + jesc(c.sensors[i].name) + "\",";
        j += "\"temp\":" + jnum(c.sensors[i].temp) + "}";
    }
    j += "],\"disks\":[";
    for (size_t i = 0; i < c.disks.size(); i++) {
        const auto& d = c.disks[i];
        if (i) j += ",";
        j += "{\"name\":\"" + jesc(d.name) + "\",";
        j += "\"type\":\"" + jesc(d.type) + "\",";
        j += "\"size_gb\":" + jnum(d.size_gb) + ",";
        j += "\"used_gb\":" + jnum(d.used_gb) + ",";
        j += "\"total_gb\":" + jnum(d.total_gb) + ",";
        j += "\"usage_percent\":" + jnum(d.usage_percent) + ",";
        j += "\"by_partitions\":" + std::string(d.by_partitions ? "true" : "false") + ",";
        j += "\"fs\":\"" + jesc(d.fs) + "\",";
        j += "\"model\":\"" + jesc(d.model) + "\"}";
    }
    j += "],\"net\":{";
    j += "\"rx_mbps\":" + jnum(c.net.rx_mbps) + ",";
    j += "\"tx_mbps\":" + jnum(c.net.tx_mbps) + ",";
    j += "\"ifaces\":[";
    for (size_t i = 0; i < c.net.ifaces.size(); i++) {
        if (i) j += ",";
        j += "{\"name\":\"" + jesc(c.net.ifaces[i].name) + "\",";
        j += "\"rx_mbps\":" + jnum(c.net.ifaces[i].rx_mbps) + ",";
        j += "\"tx_mbps\":" + jnum(c.net.ifaces[i].tx_mbps) + "}";
    }
    j += "]}";

    j += ",\"llms\":[";
    for (size_t i = 0; i < c.llms.size(); i++) {
        const auto& l = c.llms[i];
        if (i) j += ",";
        j += "{\"port\":" + jnum(l.port) + ",";
        j += "\"model_name\":\"" + jesc(l.model_name) + "\",";
        j += "\"ctx_limit\":" + jnum(l.ctx_limit) + ",";
        j += "\"ctx_used\":" + jnum(l.ctx_used) + ",";
        j += "\"ctx_percent\":" + jnum(l.ctx_percent) + ",";
        j += "\"gen_tps\":" + jnum(l.gen_tps) + ",";
        j += "\"prefill_tps\":" + jnum(l.prefill_tps) + ",";
        j += "\"cache_reuse_percent\":" + jnum(l.cache_reuse_percent) + ",";
        j += "\"spec_accept_percent\":" + jnum(l.spec_accept_percent) + ",";
        j += "\"busy_slots\":" + jnum(l.busy_slots) + ",";
        j += "\"requests_processing\":" + jnum(l.requests_processing) + ",";
        j += "\"metrics_ok\":" + std::string(l.metrics_ok ? "true" : "false") + "}";
    }
    j += "]}";
    return j;
}

// ---------- JSON: минимальный парсер (рекурсивный спуск) ----------

namespace {

struct JVal {
    enum T { NUL, NUM, STR, ARR, OBJ } t = NUL;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* find(const std::string& k) const {
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double getnum(const std::string& k, double dflt) const {
        const JVal* v = find(k);
        return (v && v->t == NUM) ? v->num : dflt;
    }
    std::string getstr(const std::string& k) const {
        const JVal* v = find(k);
        return (v && v->t == STR) ? v->str : "";
    }
};

class JParser {
public:
    explicit JParser(const std::string& s) : s_(s) {}
    bool parse(JVal& out) {
        skipws();
        out = value();
        skipws();
        return ok_ && pos_ == s_.size();
    }
private:
    const std::string& s_;
    size_t pos_ = 0;
    bool ok_ = true;

    void skipws() {
        while (pos_ < s_.size() && isspace((unsigned char)s_[pos_])) pos_++;
    }

    bool string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') { ok_ = false; return false; }
        pos_++;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\' && pos_ < s_.size()) {
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                        if (pos_ + 4 <= s_.size()) {
                            int cp = (int)std::stoul(s_.substr(pos_, 4), nullptr, 16);
                            pos_ += 4;
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    default: out += e;
                }
            } else {
                out += c;
            }
        }
        ok_ = false;
        return false;
    }

    JVal value() {
        JVal v;
        if (!ok_ || pos_ >= s_.size()) { ok_ = false; return v; }
        char c = s_[pos_];
        if (c == '{') {
            v.t = JVal::OBJ; pos_++;
            skipws();
            if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; return v; }
            while (ok_) {
                skipws();
                std::string key;
                if (!string(key)) break;
                skipws();
                if (pos_ >= s_.size() || s_[pos_] != ':') { ok_ = false; break; }
                pos_++;
                v.obj.push_back({key, value()});
                skipws();
                if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; break; }
                ok_ = false; break;
            }
            return v;
        }
        if (c == '[') {
            v.t = JVal::ARR; pos_++;
            skipws();
            if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; return v; }
            while (ok_) {
                v.arr.push_back(value());
                skipws();
                if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; break; }
                ok_ = false; break;
            }
            return v;
        }
        if (c == '"') { v.t = JVal::STR; string(v.str); return v; }
        // число / true / false / null
        size_t a = pos_;
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' &&
               s_[pos_] != ']' && !isspace((unsigned char)s_[pos_])) pos_++;
        std::string tok = s_.substr(a, pos_ - a);
        if (tok == "null") { v.t = JVal::NUL; return v; }
        if (tok == "true")  { v.t = JVal::NUM; v.num = 1; return v; }
        if (tok == "false") { v.t = JVal::NUM; v.num = 0; return v; }
        try { v.t = JVal::NUM; v.num = std::stod(tok); }
        catch (...) { ok_ = false; }
        return v;
    }
};

} // namespace

bool json_to_collector(const std::string& json, Collector& out, std::string& err) {
    JVal root;
    JParser p(json);
    if (!p.parse(root)) { err = "JSON: ошибка парсинга"; return false; }
    if (root.t != JVal::OBJ) { err = "JSON: ожидается объект"; return false; }

    out.host = root.getstr("host");
    out.ts = root.getstr("ts");
    out.uptime_s = root.getnum("uptime_s", 0);

    if (const JVal* cpu = root.find("cpu")) {
        out.cpu.usage = cpu->getnum("usage", 0);
        out.cpu.temperature = cpu->getnum("temperature", -1);
        out.cpu.load1 = cpu->getnum("load1", 0);
        out.cpu.load5 = cpu->getnum("load5", 0);
        out.cpu.load15 = cpu->getnum("load15", 0);
        out.cpu.cores = (int)cpu->getnum("cores", 0);
        out.cpu.model = cpu->getstr("model");
    }
    if (const JVal* ram = root.find("ram")) {
        out.ram.used_gb = ram->getnum("used_gb", 0);
        out.ram.total_gb = ram->getnum("total_gb", 0);
        out.ram.usage_percent = ram->getnum("usage_percent", 0);
        out.ram.swap_used_gb = ram->getnum("swap_used_gb", 0);
        out.ram.swap_total_gb = ram->getnum("swap_total_gb", 0);
    }

    out.gpus.clear();
    if (const JVal* g = root.find("gpus"); g && g->t == JVal::ARR)
        for (const auto& e : g->arr) {
            GPUStats s;
            s.index = (int)e.getnum("index", 0);
            s.name = e.getstr("name");
            s.vendor = e.getstr("vendor");
            s.utilization = e.getnum("utilization", -1);
            s.vram_used_gb = e.getnum("vram_used_gb", 0);
            s.vram_total_gb = e.getnum("vram_total_gb", 0);
            s.temperature = e.getnum("temperature", -1);
            s.clock_mhz = e.getnum("clock_mhz", -1);
            s.power_watts = e.getnum("power_watts", -1);
            out.gpus.push_back(s);
        }

    out.sensors.clear();
    if (const JVal* s = root.find("sensors"); s && s->t == JVal::ARR)
        for (const auto& e : s->arr) {
            SensorStats st;
            st.name = e.getstr("name");
            st.temp = e.getnum("temp", -1);
            out.sensors.push_back(st);
        }

    out.disks.clear();
    if (const JVal* d = root.find("disks"); d && d->t == JVal::ARR)
        for (const auto& e : d->arr) {
            DiskStats ds;
            ds.name = e.getstr("name");
            ds.type = e.getstr("type");
            ds.size_gb = e.getnum("size_gb", 0);
            ds.used_gb = e.getnum("used_gb", -1);
            ds.total_gb = e.getnum("total_gb", 0);
            ds.usage_percent = e.getnum("usage_percent", -1);
            ds.by_partitions = e.getnum("by_partitions", 0) > 0.5;
            ds.fs = e.getstr("fs");
            ds.model = e.getstr("model");
            out.disks.push_back(ds);
        }

    if (const JVal* net = root.find("net")) {
        out.net.rx_mbps = net->getnum("rx_mbps", 0);
        out.net.tx_mbps = net->getnum("tx_mbps", 0);
        out.net.ifaces.clear();
        if (const JVal* ifs = net->find("ifaces"); ifs && ifs->t == JVal::ARR)
            for (const auto& e : ifs->arr) {
                NetStats::Iface ic;
                ic.name = e.getstr("name");
                ic.rx_mbps = e.getnum("rx_mbps", 0);
                ic.tx_mbps = e.getnum("tx_mbps", 0);
                out.net.ifaces.push_back(ic);
            }
    }

    out.llms.clear();
    if (const JVal* l = root.find("llms"); l && l->t == JVal::ARR)
        for (const auto& e : l->arr) {
            LLMStats ls;
            ls.port = (int)e.getnum("port", 0);
            ls.model_name = e.getstr("model_name");
            ls.ctx_limit = (int)e.getnum("ctx_limit", 0);
            ls.ctx_used = (int)e.getnum("ctx_used", 0);
            ls.ctx_percent = e.getnum("ctx_percent", -1);
            ls.gen_tps = e.getnum("gen_tps", -1);
            ls.prefill_tps = e.getnum("prefill_tps", -1);
            ls.cache_reuse_percent = e.getnum("cache_reuse_percent", -1);
            ls.spec_accept_percent = e.getnum("spec_accept_percent", -1);
            ls.busy_slots = e.getnum("busy_slots", -1);
            ls.requests_processing = e.getnum("requests_processing", -1);
            ls.metrics_ok = e.getnum("metrics_ok", 0) > 0.5;
            out.llms.push_back(ls);
        }

    return true;
}

// ---------- HTTP-сервер ----------

void ApiServer::set_snapshot(const std::string& json) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_ = json;
}

bool ApiServer::start(int port, std::string& err) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { err = "socket"; return false; }
    int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) { err = "bind :" + std::to_string(port); return false; }
    if (listen(fd_, 8) < 0) { err = "listen"; return false; }
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void ApiServer::loop() {
    while (run_) {
        int cfd = accept(fd_, nullptr, nullptr);
        if (cfd < 0) { if (run_) continue; break; }
        std::thread([cfd, this] {
            // читаем запрос до конца заголовка (или 8K)
            char buf[8192];
            size_t n = 0;
            while (n < sizeof(buf) - 1) {
                ssize_t r = read(cfd, buf + n, sizeof(buf) - 1 - n);
                if (r <= 0) break;
                n += (size_t)r;
                if (n >= 4 && buf[n-4] == '\r' && buf[n-3] == '\n' &&
                    buf[n-2] == '\r' && buf[n-1] == '\n') break;
            }
            std::string snap;
            { std::lock_guard<std::mutex> lk(mu_); snap = snap_; }
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json; charset=utf-8\r\n"
                               "Content-Length: " + std::to_string(snap.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + snap;
            size_t off = 0;
            while (off < resp.size()) {
                ssize_t w = write(cfd, resp.data() + off, resp.size() - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
            close(cfd);
        }).detach();
    }
}

void ApiServer::stop() {
    run_ = false;
    if (fd_ >= 0) { shutdown(fd_, SHUT_RDWR); close(fd_); fd_ = -1; }
    if (th_.joinable()) th_.join();
}

// ---------- Клиент ----------

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
    std::string* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

bool fetch_remote(const std::string& hostport, Collector& out, std::string& err) {
    std::string h = hostport, p = std::to_string(API_PORT_DEFAULT);
    size_t c = hostport.rfind(':');
    if (c != std::string::npos && c + 1 < hostport.size()) {
        h = hostport.substr(0, c);
        p = hostport.substr(c + 1);
    }
    std::string url = "http://" + h + ":" + p + "/";
    std::string body;
    CURL* curl = curl_easy_init();
    if (!curl) { err = "curl init"; return false; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { err = curl_easy_strerror(rc); return false; }
    if (code != 200) { err = "HTTP " + std::to_string(code); return false; }
    return json_to_collector(body, out, err);
}
