# sysmon-cpp

Компактный мониторинг ПК в C++: TUI в стиле htop (без лишнего) + метрики LLM-серверов llama.cpp.
Одна программа, ноль зависимостей в рантайме (ncurses + curl), сборка одной командой.

C++-переработка bash-скрипта `sysmon.sh`: тот же набор данных, но с box-раскладкой,
плавными цветными барами и одним HTTP-запросом на LLM-сервер.

## Скриншот

```
┌─ sysmon — oleg-ai────────────────────────────────────────────────────────────┐
│ 2026-08-20 01:22:25   up 0d 10h 15m   refresh 1s                             │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ CPU─────────────────────────────────────────────────────────────────────────┐
│  26%   ██████░░░░░░░░░░░░░░░░░░  load 4.54 6.36 6.24   32 cores             │
│  73°C   █████████████████░░░░░░░  100°C                                      │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ RAM─────────────────────────────────────────────────────────────────────────┐
│  51%   ████████████░░░░░░░░░░    62.0 /  121.5 GiB  swap 0.00 / 8.00 GiB    │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ GPU 1 — AMD GPU─────────────────────────────────────────────────────────────┐
│   3%   █░░░░░░░░░░░░░░░░░░░░░░░  vram   3.8/  4.0 GiB ( 94%)  89 W          │
│  44°C   ███████████░░░░░░░░░░░░░  100°C                                      │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ LLM : 8082 — qwen38-27b-abliterated─────────────────────────────────────────┐
│ ctx  45% (117k/262k tok)   gen 10.0 t/s   prefill 155.6 t/s                  │
│ cache reuse —   spec accept —   busy 1   reqs 0                              │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ Network─────────────────────────────────────────────────────────────────────┤
│ enp196s0   ↓     0.0 MB/s  ↑     0.0 MB/s                                    │
├──────────────────────────────────────────────────────────────────────────────┤
┌─ Sensors─────────────────────────────────────────────────────────────────────┐
│ nvme Composite          34°C   ████████░░░░░░░░░░░░░░  100°C                 │
│ eno1 PHY Temperature    54°C   ██████████████░░░░░░░░  100°C                 │
└──────────────────────────────────────────────────────────────────────────────┘
 q — выход   r — пауза/продолжить
```

## Что показывает

| Секция | Данные | Источник |
|---|---|---|
| **CPU** | загрузка % (бар), температура (бар 0–100°C), load 1/5/15, ядра | `/proc/stat`, `/proc/loadavg`, thermal_zone / hwmon (k10temp, coretemp) / `sensors` |
| **RAM** | занято/всего GiB, %, swap | `/proc/meminfo` |
| **GPU** (NVIDIA + AMD, все карты) | util % (бар), VRAM GiB, температура (бар), clock MHz, power W | `nvidia-smi` (CSV), `/sys/class/drm/*/device` + hwmon (amdgpu) |
| **LLM** (все llama-server) | порт, модель, ctx % (занято/лимит tok), gen tok/s, prefill tok/s, cache reuse %, spec accept %, busy slots, requests | `/proc/*/cmdline` + один GET `/metrics` на сервер |
| **Network** | rx/tx MB/s по каждому активному интерфейсу | `/sys/class/net/*/statistics` (дельты) |
| **Sensors** | температуры hwmon-датчиков с барами (NVMe, сетевые PHY/MAC и др.) | `/sys/class/hwmon/*` |

Оформление: весь текст белый, цвет — только у процентов и баров.
Бары — плавный градиент 256-цветной палитры: **синий (0%) → циан → зелёный → жёлтый → красный (100%)**;
цвет цифры % совпадает с цветом бара на её уровне.

## Сборка

Требования: g++ (C++17), make, libncursesw, libcurl.

**Ubuntu/Debian:**
```
sudo apt install g++ make libncursesw6 libncurses-dev libcurl4-openssl-dev
make
```

**Arch:** `sudo pacman -S base-devel ncurses libcurl`
**Fedora:** `sudo dnf install gcc-c++ make ncurses-devel libcurl-devel`

Сборка: `make` (флаг `-O2`, линкуется `-lncursesw -lcurl`), очистка: `make clean`.
Бинарник переносим между машинами с одинаковой glibc (проверено: Ubuntu 24.04.4 → 24.04.1).

## Использование

```
sysmon           # TUI, обновление каждые 2 сек
sysmon -w 5      # TUI, каждые 5 сек
sysmon -1        # один текстовый снимок (для пайпа/логов/скриптов)
sysmon -h        # справка
```

Клавиши (TUI): `q` / ESC / Ctrl+C — выход, `r` / пробел — пауза/продолжить.

Текстовый режим (пример):
```
userver  |  2026-08-20 01:49:47  |  up 0d 05h
CPU:    3%  @ 44°C   load: 1.11 0.96 0.73   cores: 24
RAM:  12.4 / 31.2 GiB  ( 40%)   Swap: 0.00 / 8.00 GiB
  GPU0  NVIDIA CMP 170HX              util   0%  vram  42.5/ 64.0 GiB (66%)  49°C  1140 MHz  43 W
  LLM  port  1234  qwen38-27b-uncensored   ctx  87% (229k/262k tok)
```

## Как устроено

```
include/collector.h   структуры данных (CPU/RAM/GPU/Sensor/Net/LLM) + класс Collector
src/collector.cpp     сбор данных: /proc, /sys, nvidia-smi, /proc/*/cmdline + curl /metrics
src/tui.cpp           ncurses-рендер: box-секции, градиентные бары, 256-цвет
src/textout.cpp       текстовый снимок (-1)
src/main.cpp          аргументы, сигналы (SIGINT/SIGTERM → чистый выход), curl_global_init
Makefile              g++ -std=c++17 -O2, -lncursesw -lcurl
```

Один цикл: `collect()` (все источники, ~10 мс) → отрисовка кадра → `getch()` с таймаутом интервала.
Без потоков, без аллокаций в горячем цикле (кроме строковых буферов), без внешних зависимостей
помимо ncurses/curl.

## Примечания

- **Температура CPU:** сначала `thermal_zone*` (x86_pkg_temp/cpu_thermal/soc_thermal/coretemp),
  затем hwmon (k10temp/coretemp/x86_pkg_temp), fallback — `sensors | grep Tctl`.
- **NVIDIA:** `nvidia-smi --format=csv,noheader,nounits` (VRAM приходит в MiB);
  работает с кастомными драйверами (проверено на CMP 170HX, драйвер 610.43.03 ручной сборки —
  отдаёт util/VRAM/temp/clocks/power).
- **AMD:** утилизация/VRAM/температура из `/sys/class/drm/cardN/device`, power из hwmon
  (amdgpu power1_input); имя — через lspci, если в pci.ids нет имени («Device xxxx») — «AMD GPU».
- **LLM:** серверы находятся по `llama-server` в `/proc/*/cmdline`; порт и лимит контекста
  (`-c`/`--ctx-size`) — из аргументов; метрики — один GET `/metrics` (порт по cmdline),
  имена поддерживаются в виде `llamacpp:name` и `llamacpp_name`;
  ctx % = min(n_tokens_max, лимит) / лимит.
- **Sensors:** все hwmon-датчики, кроме CPU (k10temp/coretemp) и GPU (amdgpu) — они уже
  в своих секциях; отсев мёртвых зон (acpitz, ACAD) и значений вне 1–150°C.
- **Сеть:** показываются только интерфейсы в `operstate=up`, без lo/veth/br-/docker/tailscale.

## Релизы

- **v1.0.0** (20.08.2026) — TUI: градиентные бары 256-цвет, цвет % = цвет бара,
  температуры CPU/GPU со своими барами, секция Sensors (NVMe/NIC), GPU power,
  LLM-метрики (gen/prefill tok/s, cache reuse, spec accept), текстовый режим,
  NVIDIA+AMD, переносимый бинарник.

## Локации (правило §1.9.6)
- dev-оригинал: oleg-ai `~/Projects/sysmon-cpp/` (этот репозиторий)
- релиз-копии: .40 `~/Projects/sysmon-cpp/` (без .git) и др.
