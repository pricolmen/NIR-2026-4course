# Этап 10: интеграция и независимый аудит

Дата проверки: 2026-10-19. Объём — этап 10 плана: соединить ядро, движок и физическую среду, выполнить сквозные baseline/crash/blackout и первую независимую проверку. Работа останавливается до этапа 11.

## Что реализовано

Добавлены [Simulation](../include/telemetry/simulation/simulation.hpp), [реализация адаптера](../src/simulation/simulation.cpp) и семь групп [интеграционных тестов](../tests/integration_tests.cpp). API описан в [simulation.md](simulation.md). CMake/CLI-метаданные обозначают этап 10; обновлены README, прослеживаемость контракта и описания примеров. Данные JSON/CSV не менялись.

Адаптер выбирает fixed/adaptive и RR/LL, создаёт для запуска собственные изменяемые состояния на одном неизменяемом Scenario. Приём результата синхронен в фазе 2; приём heartbeat — в фазе 4; таймеры — в фазе 5. Все изменения доступности отметки объединяются в фазе 6, все запросы распределения — в один dispatch фазы 8 после всех поступлений фазы 7. Доступность строится исключительно по детекторам. Физический crash не передаётся ядру. Исходники core и engine не изменялись.

Типизированный синхронный поток наблюдения соединяет события ядра/среды и диагностику детектора/dispatch с ключом текущего действия. Исключения фатальны, повтор запуска запрещён. После успешного исполнения проверяются баланс, соответствие каталогов и статусов, логических/физических попыток и принятых завершений. Sink может проверять локальные инварианты в callbacks; межмодульные инварианты проверяются по завершении всей очереди.

## Проверки

| Группа | Наблюдаемый результат |
|---|---|
| integration_baseline | Все четыре варианта; RR назначения [1,2,1,2], завершения [4,2,8,4]; LL [1,2,2,1] и [4,2,6,6]; принятие однократно, баланс 4/4 |
| integration_crash | Fixed/adaptive RR завершают [24,2,28,4]; прерывание в 3, совместный отзыв/повтор в 20; старая локальная очередь не стартует после crash; назначение в фазе 8 самого crash допустимо, пока детектор ALIVE |
| integration_blackout | Четыре варианта: SUSPECT 15 → ALIVE 25 без FAILED; отдельно ложный FAILED не отменяет физический FIFO, оба старых результата отклонены и полная работа выполняется повторно |
| integration_simultaneous | Два FAILED в 8 дают одну реконфигурацию и один dispatch; глобальный порядок возвращённых задач [1,2,4], затем новое поступление 5; пустой ALIVE сохраняет очередь, возврат в 6 возобновляет RR |
| integration_boundaries | Результат в 8 принят до FAILED, следующая старая попытка немедленно стартует; crash при завершении подавляет результат; HB на FAILED/горизонте выигрывает; поступление/dispatch на горизонте исполняются; HB после crash в полёте и пустые задачи |
| integration_isolation | Повтор в обратном порядке четырёх вариантов совпадает по тестовой сигнатуре и историям; скрытый crash/его отсутствие при одинаковых разрешённых наблюдениях дают одинаковые логические решения |
| integration_errors | nullptr/неизвестный вариант; лимит событий; исключение sink; запрет повторного и вложенного запуска, в том числе после ошибки |
| 50 регрессионных тестов | IO/сценарии, физическая среда, движок, реконфигурация, оба детектора, RR/LL, TaskStore, численные границы, CLI-заглушка/метаданные |

Проверки используют явные условия, действующие с NDEBUG. Во всех наблюдаемых тестовых отметках контролируются порядок ключей, фазы логических/детекторных событий, не более одной реконфигурации и не более одного dispatch. Тестовая сигнатура сравнивает ключи, виды событий, решения/пороги и историю попыток; она не сериализует все поля окончательного журнала. Канонический файловый журнал и пересчёт метрик будут проверяться после реализации этапа 11.

## Независимая проверка

Проверка проведена отдельным субагентом stage10_audit в свежем контексте, без правок кода и без собственных субагентов. Настройки запуска подтверждены инструментом: GPT-6.1 Sol (`gpt-6.1-sol`), High (`high`), основной режим проверки по разделам 2.1–2.2 плана. Настройки модели/усилия основного агента отчётом не утверждаются.

Проверяющему переданы настоящий план, контракты, актуальная подготовленная реализация, тесты и результаты сборок. Он проверил границу core/engine, отсутствие скрытого знания о crash, время/округление, восемь фаз, поколения/токены, оба детектора, RR/LL, совместную реконфигурацию, старые попытки, одноразовый запуск и изоляцию вариантов. Самостоятельно повторил CTest в трёх подготовленных сборках: по 57/57; проверил C++20/предупреждения/NDEBUG/санитайзеры в compile_commands.json.

**Подтверждённых ошибок этапа 10 не найдено; исправления по замечаниям аудитора не потребовались.** Проверяющий отдельно отметил ограниченность тестовой сигнатуры выбранными полями и историями: она не доказывает тождественность будущего полного журнала/метрик. Документация явно отражает это ограничение. Нагрузочные проверки, fuzzing, исчерпание памяти и другие платформы аудитом не покрыты. Метрики и журнал проверены только как корректно отложенный объём этапа 11, CLI — этапа 12.

## Сборки и фактически выполненные команды

Платформа: macOS 26.6.2 / Darwin 25.6.0, arm64. Debug/Release — Apple Clang 17.0.0; ASan+UBSan — Homebrew LLVM 23.1.3. CMake 3.31.6, Ninja 1.11.1, JSON 3.12.0. Linux/GCC и иные платформы на этом этапе не запускались.

| Каталог репозитория | Результат |
|---|---|
| build/stage10-debug | CTest 57/57 |
| build/stage10-release | CTest 57/57, NDEBUG |
| build/stage10-sanitize | CTest 57/57, ASan+UBSan, без диагностик |

Сначала те же три конфигурации собраны и проверены в подготовленной копии `/private/tmp/nir-stage10-work`, затем повторены на фактическом пути проекта. Для автономной сборки использовано уже загруженное дерево фиксированной версии JSON:

```sh
cd /Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code
export PATH=/private/tmp/nir-stage1-tools/cmake/data/bin:/private/tmp/nir-stage1-ninja/bin:$PATH

cmake -G Ninja -S . -B build/stage10-debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage10-debug --parallel 4
ctest --test-dir build/stage10-debug --output-on-failure

cmake -G Ninja -S . -B build/stage10-release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage10-release --parallel 4
ctest --test-dir build/stage10-release --output-on-failure

cmake -G Ninja -S . -B build/stage10-sanitize -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON
cmake --build build/stage10-sanitize --parallel 4
ctest --test-dir build/stage10-sanitize --output-on-failure

./build/stage10-sanitize/telemetry_sim --build-info
git diff --check
```

В compile_commands.json проверены C++20 и все включённые предупреждения, NDEBUG Release и оба sanitizer-флага. JSON include остаётся только у build_info.cpp, scenario_loader.cpp, build_smoke.cpp и scenario_tests.cpp; новый адаптер и integration_tests.cpp его не получают. Зависимости core/engine по-прежнему только common, контролируются конфигурацией CMake. Относительные Markdown-ссылки и отсутствие конфликтных маркеров/лишних пробелов проверены отдельно. Логи повторной сборки и CTest: build/stage10-{debug,release,sanitize}/verification.log.

## Границы результата

Файловый журнал, метрики, CLI run/compare, генератор и исследовательское сравнение не реализовывались. simulation_available=false в --build-info сохраняется до появления CLI моделирования. API четырёх вариантов и малые ручные прогоны проверены; это не вывод об их научном преимуществе. Прогон нагрузки и итоговый аудит остаются этапу 14. План и DOCX не изменялись; коммит автоматически не создавался.
