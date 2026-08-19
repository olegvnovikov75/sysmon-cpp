#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <clocale>
#include <cstdio>
#include <thread>
#include <chrono>

#include <curl/curl.h>

#include "collector.h"
#include "tui.h"
#include "textout.h"

static volatile sig_atomic_t g_running = 1;

static void on_signal(int) { g_running = 0; }

static void usage() {
    std::cout <<
        "sysmon — компактный мониторинг ПК (CPU/RAM/GPU/сеть/LLM)\n"
        "\n"
        "Использование:\n"
        "  sysmon           TUI, обновление каждые 2 сек\n"
        "  sysmon -w [N]    TUI, обновление каждые N сек\n"
        "  sysmon -1        один текстовый снимок (для пайпа/логов)\n"
        "\n"
        "Клавиши (TUI): q/ESC/Ctrl+C — выход, r/пробел — пауза\n";
}

int main(int argc, char* argv[]) {
    bool once = false;
    int interval = 2;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-w" || a == "--watch") {
            if (i + 1 < argc) {
                interval = std::atoi(argv[++i]);
                if (interval < 1) interval = 2;
            }
        } else if (a == "-1" || a == "--once" || a == "--text") {
            once = true;
        } else if (a == "-h" || a == "--help") {
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

    setlocale(LC_ALL, "");       // UTF-8 для box-drawing
    setlocale(LC_NUMERIC, "C");  // точка в числах, как в sysmon.sh
    tui_init();
    bool paused = false;
    {
        Collector c;
        while (g_running) {
            if (!paused) c.collect();
            if (!tui_frame(c, interval, paused)) break;
        }
        tui_shutdown();
        if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] endwin done\n");
    }
    if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] collector destroyed\n");
    curl_global_cleanup();
    if (getenv("SYSMON_DEBUG")) fprintf(stderr, "[sysmon] curl cleaned\n");
    return 0;
}
