#ifndef TEXTOUT_H
#define TEXTOUT_H

#include "collector.h"

// Plain-текстовый снимок в стиле sysmon.sh (цвета, если stdout — tty)
void render_text(const Collector& c);

#endif // TEXTOUT_H
