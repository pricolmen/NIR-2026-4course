# Этап 12: CLI, сравнение и самоаудит

Дата: 2026-10-20. Реализован только этап 12; после кода, проверок и самоаудита работа останавливается до этапа 13.

## Объём

В [app/main.cpp](../app/main.cpp) реализованы validate, run, compare, metrics, generate, batch, глобальные сведения и --help. Разбор параметров строгий, ошибки синтаксиса дают код 2, ошибки данных/выполнения/записи — 1; корректная незавершённость допускает 0. simulation_available=true. Сведения о сборке расширены платформой и версией стандартной библиотеки.

[write_comparison](../include/telemetry/io/run_output.hpp) сохраняет общий вход и загружает один const Scenario для четырёх новых Simulation. Их состояние раздельно, сеть не перегенерируется. Возвращённые Summary и comparison.csv имеют явные единицы, выборки и причины пустых значений. Варианты сохраняют самостоятельные входы/журналы для пересчёта.

[Генерация/batch](../include/telemetry/io/experiments.hpp): периодические задачи, постоянная/равномерная работа, целые задержки, q, seed, явные crash/blackout и полный CSV потенциальных отправок. Строгий SAX-читатель long double вынесен из загрузчика в общий приватный scenario_json.hpp; прежние проверки и формат не изменены. Генератор сохраняет конфигурацию/версию/среду/контрольные суммы и валидирует записанный сценарий. Batch читает сохранённые сценарии, не генерирует сеть, сохраняет метрики повторов и средние по определённым значениям запусков с отдельными числами узловых наблюдений.

[OutputDirectory](../include/telemetry/io/output_directory.hpp) готовит результат в соседнем workspace и публикует после успеха. Явный --overwrite сохраняет старый каталог как backup; без флага непустой результат не меняется. Защищены входы, рабочий каталог и конечные symlink. Добавлены небольшие [generator.json](../examples/generator.json) и [experiments.json](../examples/experiments.json), позволяющие выполнить все команды README. Это реализация команд этапа 12, расширенный набор демонстраций остаётся этапу 13.

Документация: [cli.md](cli.md). Исторические отчёты этапов 0–11 сохранены.

## Проверки

В [tests/cli_tests.py](../tests/cli_tests.py) добавлены десять групп end-to-end через subprocess без оболочки. Драйвер использует только стандартную библиотеку Python >=3.9; явные require не отключаются оптимизацией. Python нужен лишь при BUILD_TESTING=ON; библиотека/CLI по-прежнему C++20.

| CTest | Существенные наблюдаемые результаты |
|---|---|
| commands_parsing | Неизвестные/повторные/пропущенные параметры, неверные варианты, запрет флагов не той команды, help шести команд, актуальный build-info; неверный вызов не создаёт результат |
| commands_validate | Исходный baseline из другого cwd, число задач, отрицательные/дробные времена с контекстом поля, отсутствующий файл |
| commands_run | Реальный baseline: latency 4.5, P95 8, throughput 200000; Unicode/пробелы/кавычка/$/backtick в пути без оболочки; отказ повторной записи; незавершённая задача — нормальный результат |
| commands_compare | Все четыре варианта и одинаковые входы; P95 [8,6,8,6]; независимый metrics каждого; несколько окон и пустые окна/задачи с причинами |
| commands_metrics | Восстановление испорченного metrics.json из журнала; повреждённый журнал отклоняется, отчёты не меняются |
| commands_generate | UINT64_MAX seed, точные периодические поступления/постоянная работа, q=0/1, диапазоны случайных работы/задержки, повторные файлы с одним seed, полный CSV после crash, пустой поток; неверные seed/диапазоны/периоды/узлы/окна/поля отклоняются |
| commands_batch | Относительные пути из другого cwd, стабильный порядок, корректное CSV-экранирование меток с запятой/кавычкой/переносом; запрет повторной пары ID; пустая выборка исключается из среднего; среднее Detection 16.75 по двум запускам при трёх узловых наблюдениях |
| commands_overwrite | Отказ без флага; явная замена сохраняет прежние файлы в backup; входы/корень проекта/конечные symlink не заменяются; корректен trailing slash |
| commands_failures | Некорректные входы при --overwrite сохраняют прежний каталог; ошибка записи под обычным файлом; отсутствующий manifest/run-dir; workspace не остаётся после обычной ошибки |
| commands_determinism | Два compare crash дают одинаковые manifest/events/metrics/CSV каждого варианта и общий CSV в одной сборке |
| 66 регрессионных тестов | Весь прежний набор этапов 1–11, включая независимый анализ, баланс, физические очереди, обе политики и восемь фаз |

Все три конфигурации прошли 76/76. В compile_commands.json подтверждены C++20/предупреждения, NDEBUG и address,undefined; JSON include только у IO и прежних JSON-тестов. app/main.cpp, core/engine/simulation/metrics и их нативные тесты JSON include не получают. Исходники алгоритмов, движка, среды, метрик и прежние JSON/CSV примеры не изменялись.

## Самоаудит

Проведён основным агентом по разделу 2.2 плана; субагент на этапе 12 не запускался. Следующий независимый аудит предусмотрен после этапа 14. Фактические модель/усилие основного агента отчётом не утверждаются.

Проверены: общая неизменяемая сеть и раздельные состояния четырёх запусков; отсутствие RNG в run/compare; чтение файлов относительно их источников; сохранение целых чисел/long double при генерации; q=0/1 и отправки после crash; перезапись и исключения; причины null; непуленное усреднение повторов; различение числа запусков и узловых наблюдений; детерминизм CSV, locale и сведений о среде. Служебные метки не используются как имена каталогов.

При подготовке исправлены нормализация завершающего разделителя каталога, крайние вероятности потери и исходные пути происхождения внутри comparison manifest. Общие JSON/file/CSV-функции вынесены в приватные IO-заголовки без дублирования алгоритмов. Подтверждённых незакрытых ошибок в проверенном объёме не осталось.

Ограничения: нет обещания идентичных стандартных распределений разных библиотек; проверена одна среда macOS. Параллельная запись в один результат/конкурентная правка входов не поддерживаются. Два rename с rollback при обычном исключении не являются полной файловой транзакцией при аварийном завершении процесса; backup сохраняется для восстановления. Пересчёт metrics обновляет только производные отчёты с прежними ограничениями этапа 11. Нагрузочное тестирование, fuzzing, искусственное исчерпание памяти и итоговый независимый аудит остаются будущими проверками.

## Среда и сборки

macOS 26.6.2 arm64; Debug/Release — Apple Clang 17.0.0; ASan+UBSan — Homebrew LLVM 23.1.3. CMake 3.31.6, Ninja 1.11.1.4 (версия пакета), Python 3.14.5, nlohmann/json 3.12.0. Linux/GCC/Windows не запускались.

Прежние временные CMake/Ninja отсутствовали. Они восстановлены с разрешённой сетевой загрузкой только в /private/tmp/nir-stage12-tools:

```sh
python3 -m pip install --target /private/tmp/nir-stage12-tools cmake==3.31.6 ninja==1.11.1.4 --disable-pip-version-check --no-warn-script-location
```

| Каталог | Результат |
|---|---|
| build/stage12-debug | CTest 76/76 |
| build/stage12-release | CTest 76/76 с NDEBUG |
| build/stage12-sanitize | CTest 76/76 с ASan+UBSan, без диагностик |

Сначала проверены подготовленные исходники /private/tmp/nir-stage12-work, затем три сборки повторены в фактическом проекте. Команды:

```sh
cd /Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code
export PATH=/private/tmp/nir-stage12-tools/cmake/data/bin:/private/tmp/nir-stage12-tools/bin:$PATH

cmake -G Ninja -S . -B build/stage12-debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage12-debug --parallel 3
ctest --test-dir build/stage12-debug --output-on-failure

cmake -G Ninja -S . -B build/stage12-release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage12-release --parallel 3
ctest --test-dir build/stage12-release --output-on-failure

cmake -G Ninja -S . -B build/stage12-sanitize -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage12-sanitize --parallel 3
ctest --test-dir build/stage12-sanitize --output-on-failure
```

Дополнительно реально выполнены все семь команд раздела README «Запуск CLI»: validate, run, два compare (baseline/crash), metrics, generate, batch. Результаты: results/stage12-single, results/stage12-baseline, results/stage12-crash, traces/stage12-sample, results/stage12-series. Эти файлы игнорируются Git. Проверены baseline, четыре варианта, общий CSV, complete статусы и сводка серии. Логи: build/stage12-{debug,release,sanitize}/verification.log и build/stage12-release/cli_examples.log.

Проверены Markdown-ссылки, форматирование, git diff --check и исходные хеши перед переносом. План и DOCX не менялись; коммит не создавался. Полученные числа — технические демонстрации, не вывод о преимуществе алгоритма. Работа остановлена до этапа 13.
