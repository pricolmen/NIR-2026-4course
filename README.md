# Экспериментальный стенд НИР

Текущий этап: 6 — совместная реконфигурация, однократный сортированный возврат задач и защита результатов отозванных попыток. Реализованы также fixed/adaptive, RR/LL, нагрузка и реестр задач. Движок, физическая среда и чтение сценариев относятся к следующим этапам.

Правила модели: [контракт](docs/model_contract.md). Численные правила и API: [time.md](docs/time.md). Проверки: [аудит этапа 1](docs/stage_1_audit.md), [аудит этапа 2](docs/stage_2_audit.md), [аудит этапа 3](docs/stage_3_audit.md), [аудит этапа 4](docs/stage_4_audit.md), [аудит этапа 5](docs/stage_5_audit.md), [аудит этапа 6](docs/stage_6_audit.md). Реконфигурация: [reconfiguration.md](docs/reconfiguration.md). Детекторы: [detectors.md](docs/detectors.md). Распределение: [scheduling.md](docs/scheduling.md). Логическое состояние: [task_state.md](docs/task_state.md).

## Сборка из терминала VS Code

Нужны C++20-компилятор, CMake >= 3.20 и средство сборки; ниже используется Ninja. Текущий путь проекта содержит `NIR-KR`. При пути с двоеточием загрузка через FetchContent требует CMake >= 3.30 и генератора Ninja. Конкретная проверенная среда указана в отчёте аудита. На macOS компилятор предоставляет Xcode Command Line Tools; `cmake` и `ninja` должны быть доступны в `PATH`.

После переезда старые каталоги сборки сохраняют прежние абсолютные пути в CMakeCache.txt. Используйте новые каталоги либо повторите конфигурацию с `cmake --fresh` (CMake >= 3.24). Для смены компилятора также используйте отдельный каталог.

Из корня проекта:

```sh
cmake -G Ninja -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure

cmake -G Ninja -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure

./build/release/telemetry_sim --help
./build/release/telemetry_sim --version
./build/release/telemetry_sim --build-info
```

`--build-info` возвращает JSON с версией программы, компилятора, JSON-библиотеки и текущим этапом. Неподдерживаемые команды, лишние аргументы и вызов без аргументов завершаются кодом 2. Ошибка исполнения — кодом 1. Команды моделирования ещё не реализованы.

Для компилятора, поддерживающего запрошенные санитайзеры:

```sh
cmake -G Ninja -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure
```

Оба санитайзера по умолчанию выключены и включаются отдельно. Конфигурация проверяет компиляцию и линковку с выбранными флагами; неподдерживаемый запрос приводит к явной ошибке. UBSan завершает запуск при обнаружении ошибки. `BUILD_TESTING=OFF` исключает тестовый исполняемый файл из сборки.

ASan/UBSan проверены совместно на Homebrew LLVM 23.1.3: весь набор из 32 тестов этапа 6 прошёл, намеренный выход за границу выделенной памяти обнаружен ASan. Установленный Apple Clang из Command Line Tools 26.2 зависает до входа в `main()` даже на минимальной программе. Это согласуется с [известной проблемой старых инструментов на macOS 26](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_4-release-notes). Runtime присутствует; смена пути проекта сама по себе проблему не исправляет.

Проверенная отдельная сборка с LLVM:

```sh
cmake -G Ninja -S . -B build/asan-llvm -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON
cmake --build build/asan-llvm --parallel
ctest --test-dir build/asan-llvm --output-on-failure
```

LLVM установлен командой `brew install llvm` и выбирается для этой сборки через `CMAKE_CXX_COMPILER`. Во время проверки CMake 3.31.6 и Ninja 1.11.1 использовались из временных каталогов; команду настройки PATH см. в отчёте аудита. Не подменяйте отдельно dylib старого Apple Clang: проверена согласованная пара нового компилятора и его runtime.

## JSON и автономная сборка

По умолчанию FetchContent получает **nlohmann/json 3.12.0** из фиксированного релизного архива, проверяя SHA-256:

```text
42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
```

Первая конфигурация требует доступа к GitHub. После загрузки дерево зависимости хранится в каталоге сборки. Сторонние заголовки подключены как системные; предупреждения включены для собственного кода. Собственные алгоритмические цели не получают JSON-зависимость.

Для уже установленного CMake-пакета ровно этой версии:

```sh
cmake -G Ninja -S . -B build/system-json -DTELEMETRY_USE_SYSTEM_JSON=ON -DCMAKE_PREFIX_PATH=/path/to/json/install
```

Допускается стандартная офлайн-подстановка FetchContent: `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/absolute/path/to/extracted/json`. В этом случае архив должен быть проверен отдельно: подстановка исходников обходит загрузку и её проверку хеша. Обе возможности не требуют копировать библиотеку в исходники проекта.

## Сборочные границы

| Цель | Зависимости и текущий статус |
|---|---|
| `telemetry_common` / `telemetry::common` | Компилируемая библиотека времени; общие типы, C++20, информация о сборке; без модульных зависимостей |
| `telemetry_core` / `telemetry::core` | Компилируемые TaskStore, RR/LL, LoadLedger, Dispatcher с совместной реконфигурацией и Detector; только `telemetry_common`, без движка/JSON |
| `telemetry_engine` / `telemetry::engine` | Только `telemetry_common`; пока INTERFACE, реализация на этапе 7 |
| `telemetry_simulation` / `telemetry::simulation` | `telemetry_core`, `telemetry_engine`; пока INTERFACE, реализация среды позже |
| `telemetry_io` / `telemetry::io` | `telemetry_common`, приватная JSON-зависимость; запись сведений о сборке |
| `telemetry_sim` | Сборка CLI с `telemetry_simulation` и `telemetry_io` |
| `telemetry_build_smoke` | Проверка метаданных и точности JSON, только при `BUILD_TESTING=ON` |
| `telemetry_common_tests` | Три группы проверок времени, чисел и ID; зависит только от common |
| `telemetry_task_store_tests` | Пять групп логических проверок; зависит только от core |
| `telemetry_reconfiguration_tests` | Пять групп совместного возврата, результатов, доступности, аудита и неверных входов; зависит только от core |
| `telemetry_detector_tests` | Шесть групп fixed/adaptive, окна, поколений, границ и неверных входов; зависит только от core |
| `telemetry_scheduling_tests` | Пять групп RR/LL, нагрузки и интеграции; зависит только от core |

Оставшиеся INTERFACE-цели задают зависимости и требования к потребителям без фиктивных `.cpp` и пустых архивов. При появлении исходников они станут компилируемыми библиотеками. Текущая конфигурация проверяет, что core и engine зависят только от common. Проверки включают явные условия и остаются активны в Release.

`build/`, `results/`, `traces/` игнорируются Git. Исходный план и DOCX не меняются. Сборка и моделирование не должны создавать файлы в каталоге исходников вне выделенных каталогов результатов/сборки.

Справочные первоисточники: [CMake: стандарт и расширения](https://cmake.org/cmake/help/v3.20/manual/cmake-compile-features.7.html), [nlohmann/json: интеграция CMake](https://json.nlohmann.me/integration/cmake/). Семантику модели задаёт контракт и исходный план.

На этапе 2 добавлены группы `common_time`, `common_numeric`, `common_ids`: всего 11 тестов. Проверенные новые каталоги после переезда — `build/stage2-debug`, `build/stage2-release`, `build/stage2-sanitize`; последний использует LLVM и обе опции санитайзеров. Точные команды приведены в отчёте этапа 2.

На этапе 3 добавлены `tasks_queue`, `tasks_results`, `tasks_retries`, `tasks_inputs`, `tasks_audit`: всего 16 тестов. Новые каталоги: `build/stage3-debug`, `build/stage3-release`, `build/stage3-sanitize`. Сборочные команды — в отчёте этапа 3.

На этапе 4 добавлены `scheduling_rr`, `scheduling_ll`, `scheduling_integration`, `scheduling_availability`, `scheduling_numeric`: всего 21 тест. Каталоги проверенных сборок: `build/stage4-debug`, `build/stage4-release`, `build/stage4-sanitize`; команды — в отчёте этапа 4.

На этапе 5 добавлены `detector_fixed`, `detector_adaptive`, `detector_window`, `detector_generations`, `detector_boundaries`, `detector_inputs`: всего 27 тестов. Каталоги проверенных сборок: `build/stage5-debug`, `build/stage5-release`, `build/stage5-sanitize`; команды — в отчёте этапа 5. Детекторы пока проверяются ручными наблюдениями без очереди событий.

На этапе 6 добавлены `reconfiguration_batch`, `reconfiguration_results`, `reconfiguration_availability`, `reconfiguration_audit`, `reconfiguration_inputs`: всего 32 теста. Каталоги: `build/stage6-debug`, `build/stage6-release`, `build/stage6-sanitize`; команды — в отчёте этапа 6. Изменения одной отметки пока объединяет вызывающий код: автоматические фазы и физическая среда ещё не реализованы.
