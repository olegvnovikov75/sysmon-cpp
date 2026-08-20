#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <clocale>
#include <cstdio>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <winsock2.h>
#endif

#include <curl/curl.h>

#include "collector.h"
#include "tui.h"
#include "textout.h"
#include "api.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int) { g_running = 0; }

static void usage() {
    std::cout <<
        "sysmon — компактный мониторинг ПК (CPU/RAM/GPU/сеть/диски/LLM)\n"
        "\n"
        "Использование:\n"
        "  sysmon               TUI, обновление каждые 2 сек\n"
        "  sysmon -w [N]        TUI, обновление каждые N сек\n"
        "  sysmon -1            один текстовый снимок (для пайпа/логов)\n"
        "  sysmon -ip [ADDR]    TUI + данные другого ПК (консолидация)\n"
        "                       ADDR: 192.168.1.40 или 192.168.1.40:18080\n"
        "  sysmon -h            скрытый режим: JSON-сервер (без TUI)\n"
        "  sysmon -p [N]        порт API (по умолчанию " + std::to_string(API_PORT_DEFAULT) + ")\n"
        "  sysmon --help        эта справка\n"
        "\n"
        "Клавиши (TUI): q/ESC/Ctrl+C — выход, r/пробел — пауза,\n"
        "               m — remote: снизу/справа/отключено\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // консоль: запуск двойным кликом (без консоли) — создать; заголовок окна
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
        AllocConsole();
        SetConsoleTitleA("SYSMON");
    }
    // ncurses: без TERM — xterm (в ncurses есть встроенный fallback xterm-new)
    if (!getenv("TERM")) {
#ifdef _WIN32
        _putenv_s("TERM", "xterm");
#else
        setenv("TERM", "xterm", 1);
#endif
    }
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);   // WSACleanup — автоматически при выходе
#endif
    bool once = false, hidden = false;
    int interval = 2;
    int port = API_PORT_DEFAULT;
    std::string ip;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-w" || a == "--watch") {
            if (i + 1 < argc) {
                interval = std::atoi(argv[++i]);
                if (interval < 1) interval = 2;
            }
        } else if (a == "-1" || a == "--once" || a == "--text") {
            once = true;
        } else if (a == "-h" || a == "--hidden" || a == "--server") {
            hidden = true;
        } else if (a == "-ip" || a == "--ip") {
            if (i + 1 < argc) ip = argv[++i];
        } else if (a == "-p" || a == "--port") {
            if (i + 1 < argc) {
                port = std::atoi(argv[++i]);
                if (port < 1 || port > 65535) port = API_PORT_DEFAULT;
            }
        } else if (a == "--help") {
            usage();
            return 0;
        } else {
            std::cerr << "неизвестный аргумент: " << a << "\n";
            usage();
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // явная инициализация curl (lazy-init внутри curl_easy_init гоняется с сигналами)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (once) {
        // два прохода: первый даёт базовые дельты (CPU 0.5с), второй — сеть за ~1с
        Collector c;
        c.collect();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        c.collect();
        render_text(c);
        curl_global_cleanup();
        return 0;
    }

    // скрытый режим: JSON-сервер (без TUI)
    if (hidden) {
        setlocale(LC_NUMERIC, "C");
        ApiServer srv;
        std::string err;
        if (!srv.start(port, err)) {
            fprintf(stderr, "sysmon: сервер: %s\n", err.c_str());
            curl_global_cleanup();
            return 1;
        }
        printf("sysmon: JSON-сервер http://0.0.0.0:%d/  (refresh %ds, Ctrl+C — стоп)\n", port, interval);
        fflush(stdout);
        Collector c;
        while (g_running) {
            c.collect();
            srv.set_snapshot(collector_to_json(c));
            // сон мелкими шагами — чтобы Ctrl+C/SIGTERM отработал быстро
            for (int i = 0; i < interval * 10 && g_running; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        srv.stop();
        curl_global_cleanup();
        return 0;
    }

    setlocale(LC_ALL, "");       // UTF-8 для box-drawing
    setlocale(LC_NUMERIC, "C");  // точка в числах, как в sysmon.sh
    tui_init();
    {
        TuiState st;
        st.interval = interval;
        st.remote_addr = ip;
        Collector c;
        Collector remote;
        while (g_running) {
            if (!st.paused) {
                c.collect();
                if (!ip.empty()) {
                    std::string err;
                    st.remote_ok = fetch_remote(ip, remote, err);
                }
            }
            if (!tui_frame(c, st.remote_addr.empty() ? nullptr : &remote, st)) break;
        }
        tui_shutdown();
        if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] endwin done\n");
    }
    if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] collector destroyed\n");
    curl_global_cleanup();
    if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] curl cleaned\n");
    return 0;
}
