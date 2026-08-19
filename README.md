# sysmon-cpp

Компактный мониторинг ПК в C++ (TUI в стиле htop, без лишнего) + LLM-метрики llama.cpp.
C++-версия `sysmon.sh` — тот же набор данных, но с box-раскладкой и цветными барами.

## Что показывает
- **CPU:** загрузка % (бар), температура, load 1/5/15, ядра
- **RAM:** занято/всего GiB (%), swap
- **GPU:** NVIDIA (nvidia-smi) и AMD (sysfs) — util, VRAM, temp, clock, power
- **Сеть:** rx/tx MB/s по активным интерфейсам
- **LLM-серверы (llama.cpp /metrics):** порт, модель, ctx % (занято/лимит),
  gen tok/s, prefill tok/s, cache reuse %, spec accept %, busy slots, reqs

## Сборка
```
make            # нужен g++ (C++17), libncurses-dev, libcurl4-openssl-dev
make clean
```

## Использование
```
sysmon           # TUI, обновление каждые 2 сек
sysmon -w 5      # TUI, каждые 5 сек
sysmon -1        # один текстовый снимок (для пайпа/логов)
```
Клавиши (TUI): `q`/ESC/Ctrl+C — выход, `r`/пробел — пауза/продолжить.

## Зависимости (Ubuntu 24.04)
```
sudo apt install g++ libncurses-dev libcurl4-openssl-dev
```

## Примечания
- Температура CPU: thermal_zone* (x86_pkg_temp/coretemp/...), fallback — `sensors`
- Имя AMD-GPU: lspci; если в pci.ids нет имени ("Device xxxx") — «AMD GPU»
- Метрики LLM: один GET /metrics на сервер; имена метрик поддерживаются
  в виде `llamacpp:name` и `llamacpp_name`
- ctx % = min(n_tokens_max, ctx из cmdline) / ctx из cmdline (-c/--ctx-size)

## Локации (правило §1.9.6)
- dev-оригинал: oleg-ai `~/Projects/sysmon-cpp/` (+ GitHub)
- релиз-копии: .40 и др. — без .git
