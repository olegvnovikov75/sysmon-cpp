#ifndef TUI_H
#define TUI_H

#include "collector.h"
#include <string>
#include <functional>

// Состояние TUI (управляется из main)
struct TuiState {
    int interval = 2;
    bool paused = false;
    std::string remote_addr;   // пуст — нет remote-ПК (sysmon -ip ADDR)
    int remote_mode = 2;       // 0=отключено 1=снизу 2=справа (клавиша m: справа→снизу→отключено)
    bool remote_ok = false;    // данные remote получены
    double remote_fetch_wall = 0;   // v2.0: epoch-время последней удачной выборки —
                                    // по нему «тикают» часы и uptime remote
};

void tui_init();
void tui_shutdown();

// Отрисовать кадр и ждать до st.interval секунд (или нажатия клавиши).
// false — выйти. Клавиши: q/ESC/Ctrl+C — выход, r/пробел — пауза,
// m — режим показа remote (снизу/справа/отключено).
// remote — nullptr или st.remote_addr пуст — remote не показывать.
// fast_refresh (v2.0) — вызывается каждые ~0.5 с между кадрами (суб-рефреш
// CPU/LLM для реалтайва gen/prefill) с перерисовкой; KEY_RESIZE — мгновенный перерис.
bool tui_frame(const Collector& local, const Collector* remote, TuiState& st,
               const std::function<void()>& fast_refresh = nullptr);

#endif // TUI_H
