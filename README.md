# Экспериментальный стенд НИР

Однопоточный дискретно-событийный симулятор на C++20 для сравнения реконфигурации распределённой обработки задач. Читает сохранённый сценарий и трассу heartbeat, выполняет четыре сочетания `fixed_rr`, `fixed_ll`, `adaptive_rr`, `adaptive_ll`, сохраняет журнал и рассчитывает метрики. Этапы 0–15 завершены; версия программы — 0.1.0, `implementation_stage` — 15.

## Сборка на macOS из терминала VS Code

Откройте папку проекта в VS Code и выберите **Terminal → New Terminal**. Все команды ниже выполняются из корня, где находится `CMakeLists.txt`; путь проекта в командах не зашит. Нужны Xcode Command Line Tools с C++20-компилятором, CMake ≥ 3.20, Ninja и Python ≥ 3.9 для тестов. Используются только стандартные модули Python.

Проверенная среда: macOS 26.6.2 arm64, Apple Clang 17.0.0, CMake 3.31.6, Ninja 1.11.1, Python 3.14.5. Для ASan+UBSan проверен Homebrew LLVM 23.1.3. Linux/GCC и Windows не проверялись.

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

В каждой конфигурации — 86 тестов, включая конечную нагрузочную проверку. Проверки действуют и с `NDEBUG`. `--build-info` выводит версию, текущий этап, платформу, компилятор, стандартную библиотеку и версию JSON. `BUILD_TESTING=OFF` исключает все тестовые цели и зависимость от Python.

После перемещения проекта или смены компилятора выбирайте новый каталог сборки. Для пути с `:` используйте Ninja; загрузка через FetchContent в таком пути требует CMake ≥ 3.30 либо офлайн-подстановки/установленной зависимости.

## Первый запуск и сравнение

Используется Release из предыдущего блока. Baseline — два узла, четыре задачи, без нарушений; все времена в файлах — целые микросекундные тики.

```sh
./build/final-release/telemetry_sim validate --scenario examples/baseline/scenario.json
./build/final-release/telemetry_sim run --scenario examples/baseline/scenario.json --detector fixed --scheduler rr --out results/final-single
./build/final-release/telemetry_sim compare --scenario examples/baseline/scenario.json --out results/final-compare
./build/final-release/telemetry_sim metrics --run-dir results/final-single
```

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

Сценарий — JSON версии 1 с `tick_ns=1000`, параметрами детектора, узлами, задачами, crash/blackout, окнами измерения и путём `delivery_trace`. Путь CSV разрешается относительно JSON. Трасса содержит все потенциальные отправки, включая будущие отправки после crash; пропуски/дубликаты отклоняются. Точная схема: [scenario_format.md](docs/scenario_format.md); готовый минимальный JSON/CSV: [baseline](examples/baseline/README.md). Конфигурация генератора и manifest серии: [cli.md](docs/cli.md).

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
