# Экспериментальный стенд НИР

Однопоточный дискретно-событийный симулятор на C++20 для сравнения реконфигурации распределённой обработки задач. Читает сохранённый сценарий и трассу heartbeat, выполняет четыре сочетания `fixed_rr`, `fixed_ll`, `adaptive_rr`, `adaptive_ll`, сохраняет журнал и рассчитывает метрики. Этапы реализации 0–15 и новая очередь экспериментов 1–10 завершены; версия программы — 0.1.0, `implementation_stage` — 15.

## Сборка на macOS из терминала VS Code

Откройте папку проекта в VS Code и выберите **Terminal → New Terminal**. Все команды ниже выполняются из корня, где находится `CMakeLists.txt`; путь проекта в командах не зашит. Нужны Xcode Command Line Tools с C++20-компилятором, CMake ≥ 3.20, Ninja и Python ≥ 3.9 для тестов. Основные тесты используют стандартные модули Python. Для статистической обработки и её дополнительных тестов нужны зависимости tools/requirements-statistics.txt.

Проверенная среда: macOS 26.6.2 arm64, Apple Clang 17.0.0, CMake 4.4.4, Ninja 1.13.2, Python 3.14.5. Для ASan+UBSan проверен Homebrew LLVM 23.1.3. Linux/GCC и Windows не проверялись.

Если инструменты ещё не установлены, Xcode Command Line Tools устанавливаются через `xcode-select --install`; при наличии Homebrew CMake и Ninja — через `brew install cmake ninja`. Проверьте доступность инструментов:

```sh
clang++ --version
cmake --version
ninja --version
python3 --version
```

Первая настройка по умолчанию загружает nlohmann/json 3.12.0 с GitHub. Способ сборки без сети указан ниже. Новые имена каталогов обходят устаревшие CMakeCache после переноса проекта:

```sh
cmake -G Ninja -S . -B build/final-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/final-debug --parallel 4
ctest --test-dir build/final-debug --output-on-failure

cmake -G Ninja -S . -B build/final-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/final-release --parallel 4
ctest --test-dir build/final-release --output-on-failure

./build/final-release/telemetry_sim --help
./build/final-release/telemetry_sim --version
./build/final-release/telemetry_sim --build-info
```

В каждой обычной конфигурации — 95 тестов, включая конечную нагрузочную проверку. Проверки действуют и с `NDEBUG`. `--build-info` выводит версию, текущий этап, платформу, компилятор, стандартную библиотеку и версию JSON. `BUILD_TESTING=OFF` исключает все тестовые цели и зависимость от Python.

После перемещения проекта или смены компилятора выбирайте новый каталог сборки. Для пути с `:` используйте Ninja; загрузка через FetchContent в таком пути требует CMake ≥ 3.30 либо офлайн-подстановки/установленной зависимости.

## Первый запуск и сравнение

Используется Release из предыдущего блока. Baseline — два узла, четыре задачи, без нарушений; все времена в файлах — целые микросекундные тики.

```sh
./build/final-release/telemetry_sim validate --scenario examples/baseline/scenario.json
./build/final-release/telemetry_sim run --scenario examples/baseline/scenario.json --detector fixed --scheduler rr --out results/final-single
./build/final-release/telemetry_sim compare --scenario examples/baseline/scenario.json --out results/final-compare
./build/final-release/telemetry_sim metrics --run-dir results/final-single
```

После успешных `run`, `compare` и `batch` консоль показывает конфигурацию (узлы, задачи, производительности, heartbeat, детекторы, crash/blackout и окна), результаты и краткие наблюдения. Все длительности указаны в микросекундах; отсутствующие метрики не заменяются нулём. При полном завершении задач compare перечисляет все варианты с минимальным P95 на данном входе, включая равенства. При незавершённости или пустой выборке общий минимум не выделяется. В batch сводка выводится отдельно для каждого сценария/повтора; файлы остаются полным источником результатов.

В `results/final-single/metrics.json` ожидаются `completed=4`, `unfinished=0`, mean latency 4.5 тика, P95 8 тиков и throughput 200000 задач/с в `(0,20]`. Это ручной контроль корректности. В сравнении RR завершает задачи по ID в `[4,2,8,4]`, LL — в `[4,2,6,6]`.

| Путь внутри каталога запуска | Содержание |
|---|---|
| `manifest.json` | Вариант, параметры, среда, статус и контрольные суммы |
| `input/scenario.json`, `input/delivery.csv` | Точные снимки входов |
| `events.jsonl` | Канонический журнал с временем, фазой, попыткой и решением |
| `metrics.json` | Показатели, единицы, размеры выборок и инварианты |
| `tasks.csv`, `attempts.csv`, `nodes.csv` | Задачи, попытки и классификация узлов |

В `results/final-compare` находятся четыре каталога вариантов, `comparison.csv` и `comparison_manifest.json`. Каждый вариант получает одни сохранённые входы и отдельное изменяемое состояние. `metrics` читает снимки и журнал, проверяет их и заново пишет производные отчёты без моделирования.

Повторная запись в непустой выходной каталог отклоняется. Для следующего опыта задайте новый `--out` либо явно добавьте `--overwrite`: прежний результат сохраняется в соседнем `.<name>.previous-N`. Код 0 означает успешную команду, 2 — ошибку синтаксиса CLI, 1 — ошибку входа/исполнения/записи. Успешно достигнутый горизонт с незавершёнными задачами допустим; смотрите `unfinished` и причины отсутствующих метрик.

## Генератор и серия примеров

```sh
./build/final-release/telemetry_sim generate --config examples/generator_recovery.json --out traces/final-recovery
./build/final-release/telemetry_sim compare --scenario traces/final-recovery/scenario.json --out results/final-generated
./build/final-release/telemetry_sim batch --manifest examples/experiments.json --out results/final-suite
./build/final-release/telemetry_sim metrics --run-dir results/final-suite/runs/000001/fixed_rr
```

`generate` сначала материализует JSON/CSV, конфигурацию и `generation_manifest.json`. `compare`/`run` генератор не вызывают. Девять случаев из [каталога примеров](examples/README.md) дают 36 запусков в `results/final-suite/runs/`; `batch_manifest.json` связывает номера каталогов с ID сценариев/повторов. `comparison.csv` хранит отдельные результаты, `summary.csv` — средние по определённым значениям запусков и размеры выборок. Один технический повтор не является статистическим исследованием.

Исходный сценарий должен содержать минимум один узел; поток задач может быть пустым. Сценарий — JSON версии 1 с `tick_ns=1000`, параметрами детектора, узлами, задачами, crash/blackout, окнами измерения и путём `delivery_trace`. Путь CSV разрешается относительно JSON. Трасса содержит все потенциальные отправки, включая будущие отправки после crash; пропуски/дубликаты отклоняются. Точная схема: [scenario_format.md](docs/scenario_format.md); готовый минимальный JSON/CSV: [baseline](examples/baseline/README.md). Конфигурация генератора и manifest серии: [cli.md](docs/cli.md).

## Серии на нескольких seed

Для новой очереди экспериментальных этапов подготовлена обёртка над существующими generate и batch. Пробный запуск использует два seed и уже имеющийся случайный шаблон:

```sh
python3 tools/run_seed_series.py --template examples/generator.json --seeds 101 202 --program ./build/experiment-stage1-release/telemetry_sim --out results/experiment-stage1-seeds
python3 tools/run_seed_series.py --reuse-series results/experiment-stage1-seeds --program ./build/experiment-stage1-release/telemetry_sim --out results/experiment-stage1-replay
```

Перед этими командами соберите программу в build/experiment-stage1-release по инструкции в [seed_series.md](docs/seed_series.md), либо укажите свой собранный telemetry_sim через --program. Первый вызов генерирует отдельные сохранённые реализации; второй заново исполняет те же входы без генерации и не добавляет независимых наблюдений. series_manifest.json связывает seed, хеши входов, команды и реальные каталоги batch. Скрипт отказывается перезаписывать существующий --out. [Отчёт этапа 1](docs/experiment_stage_1_audit.md) относится к организации серии; исследования сети/параметров и статистическая обработка выполняются отдельными этапами.

## Исследование jitter и потерь heartbeat

Этап 2 новой экспериментальной очереди: [план и условия](experiments/network_stage2/README.md), [проверка и результаты](docs/experiment_stage_2_audit.md). Запуск использует обёртку этапа 1 и существующие generate/batch:

```sh
python3 tools/run_network_series.py --plan experiments/network_stage2/plan.json --program ./build/experiment-stage2-release/telemetry_sim --out results/experiment-stage2-network
```

Программа предварительно собирается в build/experiment-stage2-release обычными командами CMake (см. инструкцию серии); --program можно заменить путём своей сборки. Фиксированы узлы, поток и параметры детекторов. Изменяются симметричный разброс задержки при заданном среднем 10 мс и вероятность потери. Сохранены все реализации; native summary объединяется без нового расчёта метрик. Старые серии не перезаписываются.

## Исследование начального тайм-аута fixed

Этап 3 использует сохранённые входы этапа 2 без generate. [План и ограничения](experiments/timeout_stage3/README.md), [отчёт](docs/experiment_stage_3_audit.md).

```sh
python3 tools/run_timeout_series.py --plan experiments/timeout_stage3/plan.json --source-network results/experiment-stage2-network --program ./build/experiment-stage3-release/telemetry_sim --out results/experiment-stage3-timeout
```

Перед запуском соберите программу в build/experiment-stage3-release обычными командами CMake, либо укажите свою сборку через --program. Меняется только tau0; задачи и delivery.csv сохраняются. Основные выводы — по fixed RR/LL. В существующем формате tau0 задаёт также стартовые пороги adaptive, поэтому сопровождающие adaptive-запуски не считаются неизменным контролем. Исходная сетевая серия должна быть доступна; новый выходной каталог не перезаписывается.

## Исследование коэффициента κ

Этап 4 меняет только kappa на сохранённых задачах и трассах этапа 2. Исследуются fixed/adaptive и RR/LL при исходном T₀=20 мс. [План и инструкция](experiments/kappa_stage4/README.md), [аудит](docs/experiment_stage_4_audit.md).

```sh
python3 tools/run_kappa_series.py --plan experiments/kappa_stage4/plan.json --source-network results/experiment-stage2-network --program ./build/experiment-stage4-release/telemetry_sim --out results/experiment-stage4-kappa
```

Предварительно соберите программу в build/experiment-stage4-release либо укажите свою через --program. Выходной каталог должен быть новым. Формула SUSPECT не меняется; κ масштабирует raw-порог FAILED у обоих детекторов, включая прогрев adaptive. Прежние серии не перезаписываются.

## Исследование коэффициента c адаптивного детектора

Этап 5 меняет только adaptive.c на сохранённых задачах и трассах этапа 2. T₀=20 мс, κ=2 и M=3 сохраняются. Fixed служит неизменным контролем, c=2 — контролем исходных результатов. [План и инструкция](experiments/coefficient_stage5/README.md), [аудит](docs/experiment_stage_5_audit.md).

```sh
python3 tools/run_coefficient_series.py --plan experiments/coefficient_stage5/plan.json --source-network results/experiment-stage2-network --program ./build/experiment-stage5-release/telemetry_sim --out results/experiment-stage5-coefficient
```

Сначала соберите программу в build/experiment-stage5-release либо укажите свою через --program. Выходной каталог должен быть новым. Коэффициент c умножает выборочное стандартное отклонение при расчёте адаптивных порогов после прогрева. Формулы и прежние серии сохраняются.

## Исследование размера окна M

Этап 6 меняет только adaptive.window на сохранённых задачах и трассах этапа 2. Минимум для прогрева min_intervals=2, T₀=20 мс, κ=2 и c=2 сохраняются. [План и инструкция](experiments/window_stage6/README.md), [аудит](docs/experiment_stage_6_audit.md).

```sh
python3 tools/run_window_series.py --plan experiments/window_stage6/plan.json --source-network results/experiment-stage2-network --program ./build/experiment-stage6-release/telemetry_sim --out results/experiment-stage6-window
```

Сначала соберите программу в build/experiment-stage6-release либо укажите свою через --program. Выход должен быть новым. M ограничивает число последних интервалов в истории, а не число интервалов до прогрева; fixed остаётся неизменным контролем. Формулы и прежние серии сохраняются.

## Четыре узла, интенсивность и производительность

Этап 7 задаёт три интенсивности потока и два профиля производительности при одинаковой суммарной мощности и одинаковой мощности отказавшего узла. [План и инструкция](experiments/load_stage7/README.md), [аудит](docs/experiment_stage_7_audit.md).

```sh
python3 tools/run_load_series.py --plan experiments/load_stage7/plan.json --program ./build/experiment-stage7-release/telemetry_sim --out results/experiment-stage7-load
```

Сначала соберите программу в build/experiment-stage7-release либо укажите свою через --program. Генерация новых входов четырёх узлов выполняется существующим CLI. Проверяются одинаковые работы задач и трассы при смене интенсивности, производительности и режима отказа. Выход должен быть новым; прежние серии сохраняются.

## Статистическая сводка и графики

Этап 8 читает основные результаты этапов 2–7 без новых моделирований: сводки по seed, парные различия и 95%-ные bootstrap-интервалы, графики PNG/SVG. [Методика](docs/statistics.md), [запуск](experiments/statistics_stage8/README.md), [аудит](docs/experiment_stage_8_audit.md).

```sh
python3 -m venv build/statistics-venv
./build/statistics-venv/bin/python -m pip install -r tools/requirements-statistics.txt
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results --out results/experiment-stage8-statistics
```

Технические повторы и демонстрации исключены; условия не объединяются в искусственно большую независимую выборку. Пропуски Detection/Recovery не заменяются нулём. Зависимости NumPy/Matplotlib относятся только к анализу. Обычный набор содержит 95 CTest; `-DTELEMETRY_ENABLE_STATISTICS_TESTS=ON` и `-DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python"` добавляют группу статистических тестов (всего 96).

## Границы heartbeat и σ = 0

Этап 9 проверяет S/F±1 микросекундный тик, равенство доставки/таймера, устаревшие сообщения, прогрев, σ=0, округление F и включённый горизонт. [План и запуск](experiments/boundary_stage9/README.md), [аудит](docs/experiment_stage_9_audit.md).

```sh
python3 tools/run_boundary_study.py --plan experiments/boundary_stage9/plan.json --program ./build/experiment-stage9-release/telemetry_sim --out results/experiment-stage9-boundary
```

Используются явные детерминированные scenario.json/delivery.csv и прежние validate/batch. Это семантические проверки, не новые независимые seed-повторы. Бинарник предварительно собирается либо указывается через --program; выход должен быть новым. Формулы и прежние серии сохраняются.

## Итоговая проверка экспериментальной очереди

Конфигурации, входы и содержательные результаты этапов 1–9 закреплены SHA-256 в experiments/final_stage10/lock.json. [Полная инструкция](experiments/final_stage10/README.md), [итоговый аудит](docs/experiment_stage_10_audit.md). Все 7448 выполнений заново воспроизведены свежим Release; обычные и дополнительные тесты проверены в Debug, Release и ASan+UBSan.

```sh
./build/statistics-venv/bin/python tools/verify_experiments.py --results-root results --program ./build/experiment-stage10-release/telemetry_sim --reproduce --replay-metrics --out results/my-final-check
```

Предварительно соберите бинарник и установите статистическое окружение по инструкции. Нужны исходные основные серии этапов 1–9; старые результаты читаются, повтор создаётся внутри нового --out. Для проверки сохранённых данных без моделирования достаточно `python3 tools/verify_experiments.py --results-root results --out results/my-frozen-check`. Удалённые основные серии требуется восстановить из архива. Это техническое повторение тех же seed, а не увеличение научной выборки.

## Санитайзеры

Проверенная для этой macOS пара — Homebrew LLVM и его runtime (`brew install llvm`, если LLVM отсутствует):

```sh
cmake -G Ninja -S . -B build/final-sanitize -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON
cmake --build build/final-sanitize --parallel 4
ctest --test-dir build/final-sanitize --output-on-failure
```

Путь `/opt/homebrew` соответствует проверенному arm64 Homebrew; при другой установке укажите свой `clang++`. Обе опции по умолчанию выключены. Старый Apple Clang из установленных Command Line Tools 26.2 зависал до `main()`; переход на LLVM проверен в [отчёте этапа 1](docs/stage_1_audit.md). Санитайзеры этапа 14 и итоговой передачи: [этап 14](docs/stage_14_audit.md), [этап 15](docs/stage_15_audit.md).

## JSON и сборка без сети

Зависимость закреплена на nlohmann/json **3.12.0**, SHA-256 релизного архива:

```text
42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
```

Для уже установленного CMake-пакета ровно этой версии добавьте к настройке `-DTELEMETRY_USE_SYSTEM_JSON=ON -DCMAKE_PREFIX_PATH=/path/to/json/install`. Для распакованных исходников добавьте `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/absolute/path/to/extracted/json`. Эти пути заменяются фактическими; офлайн-подстановка обходит загрузку и её проверку SHA-256, поэтому архив нужно проверить до распаковки. Код библиотеки не копируется в исходники проекта.

Например, если проверенная зависимость уже загружена предыдущей сборкой в `build/debug/_deps/nlohmann_json-src`, автономная настройка Debug выглядит так (такую же подстановку добавьте для Release и санитайзеров):

```sh
cmake -G Ninja -S . -B build/final-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/build/debug/_deps/nlohmann_json-src"
```

## Документация и границы результата

[Архитектура и таблица «правило → модуль → проверка»](docs/architecture.md), [время и округление](docs/time.md), [порядок воспроизведения экспериментов](docs/experiments.md), [контракт модели](docs/model_contract.md), [CLI](docs/cli.md), [журнал и метрики](docs/journal_metrics.md).

Все исходные данные малых демонстраций находятся в `examples/`. Каталоги `build/`, `results/`, `traces/` игнорируются Git. Сохраняйте весь каталог запуска вместе с `input/` и журналом, если нужен последующий пересчёт. Метод подсчёта собственного кода и границы поставки указаны в [отчёте передачи](docs/stage_15_audit.md).

Полный набор раздела 12 плана, повторы и конечная нагрузка проверены на этапе 14; независимый проверяющий GPT-6.1 Sol / High подтверждённых дефектов не обнаружил. Итоговая передача проверена отдельно на этапе 15. Побитовая одинаковость вещественных вычислений и случайных распределений между платформами не обещается. Демонстрационные результаты и нагрузочный smoke не доказывают научного превосходства алгоритмов. Разработка GUI, настоящей сети/кластера и написание главы 3 НИР в выполненный объём не входят.
