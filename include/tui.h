#ifndef TUI_H
#define TUI_H

#include "collector.h"
#include <string>

// Состояние TUI (управляется из main)
struct TuiState {
    int interval = 2;
    bool paused = false;
    std::string remote_addr;   // пуст — нет remote-ПК (sysmon -ip ADDR)
    int remote_mode = 2;       // 0=отключено 1=снизу 2=справа (клавиша m: справа→снизу→отключено)
    bool remote_ok = false;    // данные remote получены
};

void tui_init();
void tui_shutdown();

// Отрисовать кадр и ждать до st.interval секунд (или нажатия клавиши).
// false — выйти. Клавиши: q/ESC/Ctrl+C — выход, r/пробел — пауза,
// m — режим показа remote (снизу/справа/отключено).
// remote — nullptr или st.remote_addr пуст — remote не показывать.
bool tui_frame(const Collector& local, const Collector* remote, TuiState& st);

#endif // TUI_H
