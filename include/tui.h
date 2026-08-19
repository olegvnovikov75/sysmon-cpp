#ifndef TUI_H
#define TUI_H

#include "collector.h"

void tui_init();
void tui_shutdown();

// Отрисовать кадр и ждать до interval_sec секунд (или нажатия клавиши).
// false — выйти. Клавиши: q/ESC/Ctrl+C — выход, r/пробел — пауза.
bool tui_frame(const Collector& c, int interval_sec, bool& paused);

#endif // TUI_H
