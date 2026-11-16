# Сдача GUI курсовой работы

Четыре этапа GUI завершены. Расчётная часть telemetry_sim коммита 38f1b201, DOCX и прежние результаты НИР сохранены. Приложение запускает существующие generate/validate/run отдельным QProcess, отображает журнал и готовые метрики; новые алгоритмы расчёта не вводятся. [Подробное использование](../gui/README.md), [аудит и фактические результаты](gui_stage_4_audit.md).

## Сборка и запуск

Команды выполняются из корня проекта. Нужны C++20, CMake ≥3.22, Ninja, Qt 6 Widgets/Concurrent (Qt Test при тестировании), Python ≥3.9 для тестов. Проверено на macOS arm64. При отсутствии Qt: brew install qtbase. Первая настройка загружает nlohmann/json 3.12.0; при готовом офлайн-источнике добавьте -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/build/debug/_deps/nlohmann_json-src".

```sh
cmake -G Ninja -S gui -B build/kr-gui-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
cmake --build build/kr-gui-release --parallel 4
./build/kr-gui-release/telemetry_gui --scenario examples/crash/scenario.json
```

GUI: build/kr-gui-release/telemetry_gui. Backend: build/kr-gui-release/telemetry_sim. Аргумент --scenario только открывает файл. Выберите «Фиксированный», «Round Robin — по кругу», нажмите «Запустить расчёт». После успеха управляйте воспроизведением справа, выберите задачу в списке. Нижние итоги относятся ко всему расчёту. Для новой конфигурации используйте «Новый сценарий…», для копии готовой пары — «Отказы / копия…».

Обычные пользовательские данные сохраняются в results/gui-runs. Каждый запуск имеет собственный UUID; процесс и диагностика — process.log, запрос — request.json, результат — result/ с manifest.json, input/, events.jsonl, metrics.json и CSV. Не переносите JSON без связанной delivery_trace. Сохраняйте всю пару JSON/CSV либо каталог прогона. История окна хранится в памяти сеанса; после перезапуска откройте сохранённый сценарий заново.

## Проверки и данные для глав 3–4

```sh
ctest --test-dir build/kr-gui-release --output-on-failure
ctest --test-dir build/kr-gui-release -R '^gui_stage[1234]$' --output-on-failure
QT_QPA_PLATFORM=cocoa ./build/kr-gui-release/telemetry_acceptance_tests ./build/kr-gui-release/telemetry_sim . results/kr-gui-stage4/acceptance
```

По умолчанию 99 CTest. Чтобы повторить проверенные 100, включите прежний статистический тест при настройке: -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python". Для его окружения действуют прежние tools/requirements-statistics.txt и инструкция в корневом README. Qt Test управляет настоящими окнами автоматически; ручная проверка человеком не заявляется.

| Данные | Для чего использовать |
|---|---|
| results/kr-gui-stage4/final_checks.json | Команды, среда, сводка полных тестов и сохранности |
| results/kr-gui-stage4/acceptance/check-UUID/acceptance.json и .csv | 24 строки: конфигурация, ожидания, фактические результаты, способ проверки, пути и SHA |
| Вложенные <сценарий>/<вариант>/runs/run-UUID/result/ | Полные журналы, метрики, снимки входов для воспроизводимости |
| results/kr-gui-stage4/live-editor/check-UUID/ | Четыре вкладки формы, редактор отказов, созданная/сохранённая пара и повторные прогоны |
| results/kr-gui-stage4/live-replay/check-UUID/ | Пошаговый crash, таблица попыток, проверка ×1 за 8 секунд, timing.json |
| results/kr-gui-stage4/acceptance/check-UUID/<сценарий>/fixed_rr/window.png | Снимки шести проверочных условий в живом окне |
| results/kr-gui-stage4-frozen-check/ | Повторная проверка зафиксированных исследований НИР |

Для текста глав используйте значения из сохранённых metrics.json, а последовательности событий — из events.jsonl. Время в тиках соответствует мкс (tick_ns=1000). null сопровождается reason, не означает ноль. Проверка интерфейса на детерминированных примерах не доказывает универсального превосходства fixed/adaptive либо RR/LL.

## Состав кода

| Файлы gui/ | Ответственность |
|---|---|
| main.cpp, main_window.* | Запуск приложения, настройка, история сеанса и итоговые показатели |
| run_controller.*, run_data.*, async.hpp | Отдельные процессы и переходы generate/validate/run, фоновые операции, проверка протокола/снимков |
| replay.*, replay_data.cpp, playback_widget.* | Чтение/проекция событий, управление временем и показ узлов/попыток |
| scenario_files.*, scenario_dialogs.* | Форма конфигурации генератора и сохранение копии со своим списком отказов |
| tests/*, CMakeLists.txt | Проверки четырёх этапов, отдельная цель GUI, включение прежнего симулятора |

При ошибке доступна диагностика и повторный запуск. Загруженный журнал хранится в памяти; расход растёт с его размером. Сохранение JSON и CSV не является общей транзакцией при аварии питания. Qt нужен во время выполнения. Windows/Linux, упаковка .app и импорт старой истории при старте не проверялись/не реализованы; они не входят в согласованный объём.
