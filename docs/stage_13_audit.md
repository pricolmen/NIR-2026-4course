# Этап 13: демонстрационные сценарии и самоаудит

Дата: 2026-10-20. Завершён только этап 13; работа останавливается до итоговой проверки этапа 14.

## Объём

Завершён [каталог примеров](../examples/README.md): восемь случаев из плана и отдельный материализованный контроль генератора. Сохранены JSON/CSV для long_blackout, reordered, simultaneous, no_alive, unfinished; baseline, crash и blackout используются без изменения прежних входов. Каждый каталог имеет README с ручными сроками/ожиданиями и указанием, что пример не доказывает преимущества алгоритма.

Добавлен [generator_recovery.json](../examples/generator_recovery.json). Действующая команда generate до моделирования записала небольшой [generated_recovery](../examples/generated_recovery/README.md): scenario.json, полный delivery.csv, точную конфигурацию и generation_manifest.json с seed=1304, версией/средой/контрольными суммами. Постоянная работа и фиксированные доставки позволяют рассчитать результат вручную. Старый generator.json сохраняет демонстрацию случайных диапазонов и потерь.

[experiments.json](../examples/experiments.json) расширен до девяти сценариев — 36 отдельных запусков и по 36 строк comparison.csv/summary.csv. У каждого сценария один технический повтор, без статистического утверждения об независимых экспериментах. Обновлены README, CLI-документация, прослеживаемость контракта и номер этапа 13. Нативные библиотеки/генератор/CLI-команды не расширялись: функциональность генератора реализована на этапе 12 и использована по её API. Кодовые изменения — девять групп проверки примеров и обновление метаданных этапа.

## Ручные ожидания и результаты

| Случай | Проверенный результат во всех четырёх вариантах |
|---|---|
| baseline | Существующие ручные назначения/завершения, повторная загрузка и пересчёт |
| crash | Существующий crash 3, FAILED 20; сохранённые входы воспроизводят журнал/метрики |
| blackout | Существующий краткий перерыв: SUSPECT 15 → ALIVE 25; повторное чтение |
| long_blackout | Нет crash; SUSPECT 4, ложный FAILED 8, поздние HB не восстанавливают узел. RR завершения [18,1,21], два прежних результата отклонены; LL [18,1,4], один отклонённый. Старая RR-попытка 3 физически стартует в 10 и заканчивается в 13 после отзыва в 8 |
| reordered | HB 1 приходит в 7 после 2/3; HB 5 — в 14 после 6. Оба номера отклонены. В 7 контакт 6, поколение 2. Adaptive SUSPECT 10 → ALIVE 12; fixed остаётся ALIVE. Назначение задачи 3 идёт на узел 2/1 соответственно, завершения [4,4,11,13] |
| simultaneous | Crash узлов 1/2 в 3, оба FAILED в 10: один пакет, один dispatch после поступления 7. Detection=7. RR возвращает [1,2,4,5], завершает [20,29,1,31,34,2,35], Recovery 28/31. LL возвращает [1,2], завершает [20,29,1,3,6,7,30], Recovery 17/26 |
| no_alive | Crash обоих в 0, скрытый от ядра; исходные назначения в 0 не стартуют. Пустой ALIVE с SUSPECT в 4, FAILED 8. Итоговый FIFO по событиям [3,1,2,4], done=0, queued=unfinished=4, assigned=0, Detection=8, Recovery=null/recovery_incomplete |
| unfinished | Завершается только задача 2 в 1; done=1, assigned=unfinished=2. Задача 3 назначается в фазе 8 времени 10: RR ждёт на узле 1, LL стартует на узле 2 без завершения на горизонте. Latency mean=P95=1, throughput=100000 задач/с |
| generated_recovery | Сохранённая генерация: поступления 0/1/2, работа 6, crash 3, FAILED 8; завершения [14,7,20], Detection=5, Recovery=17, два повтора; полный CSV 40 строк, включая потенциальные отправки после crash |

## Тесты

Добавлены [tests/example_tests.py](../tests/example_tests.py) и девять CTest-групп: examples_catalog, examples_long_blackout, examples_reordered, examples_simultaneous, examples_no_alive, examples_unfinished, examples_reload, examples_generator, examples_batch. Используется уже имеющийся тестовый Python без новых зависимостей. Все условия явные, не отключаются оптимизацией Python/Release.

Для каждого сценария два отдельных compare заново читают исходные файлы и выполняют четыре варианта. Сравниваются байты manifest/events/metrics/трёх CSV, исходные/сохранённые JSON/CSV; затем выполняется независимый metrics из журнала. Проверяются нумерация и порядок ключей, включённый горизонт, фазовая пакетность, однократное принятие и баланс. Для новых случаев дополнительно проверены конкретные последствия из таблицы. Базовые примеры покрыты отдельной examples_reload.

examples_generator дважды регенерирует сохранённые входы и сравнивает их с сохранённым контролем в examples, проверяет конфигурацию и контрольные суммы метаданных независимым FNV-1a-64 в тесте, затем выполняет run по заново загруженным файлам. Его журнал/метрики/CSV совпадают с compare сохранённого контроля. Метаданные генератора отражают среду создания снимка; тест не требует одинаковых сведений о компиляторе при проверке другим компилятором.

examples_batch выполняет весь каталог, включая корректно незавершённые случаи, и проверяет 36 записей запусков/строк сравнения/сводки. Прежние 76 тестов сохранены. Итог — 85/85 в каждой конфигурации.

## Самоаудит

Самоаудит выполнен основным агентом. По разделу 2.2 плана субагент на этапе 13 не запускался; следующий независимый аудит предусмотрен после этапа 14. Фактические настройки модели/усилия основного агента не утверждаются.

Проверены: полнота восьми обязательных случаев; все потенциальные CSV-ключи, включая после crash; соответствие ручных сроков состояниям/фазам и исходным работам; окончательность ложного FAILED без физической отмены; отклонение старых HB без сброса контакта; объединение FAILED и порядок возврата/поступления; пустой ALIVE, причины null и незавершённость; воспроизведение по сохранённым входам, конфигурация/seed/версия/среда и метаданные генератора.

Материализованный контроль создан реальным generate, не подменён заранее записанными метриками. В исходники добавлены только небольшие входы и метаданные, без журналов/результатов экспериментов. Проверено совпадение исходных хешей всех нативных модулей и старых входов: они не менялись. Исправлений алгоритмов не потребовалось, подтверждённых незакрытых ошибок в проверенном объёме не обнаружено.

Границы: повторяемость проверена в одной сборке; для произвольных случайных диапазонов seed не обещает одинаковой реализации стандартных распределений на других платформах. Набор малый и демонстрационный. Умеренная нагрузочная проверка и независимый аудит этапа 14 здесь не выполнялись. Другие платформы, fuzzing, искусственное исчерпание памяти и конкурентная запись не проверялись.

## Сборки и команды

macOS 26.6.2 arm64; Apple Clang 17.0.0 для Debug/Release, Homebrew LLVM 23.1.3 для ASan+UBSan; CMake 3.31.6, Ninja 1.11.1.4 (версия пакета), Python 3.14.5, nlohmann/json 3.12.0. Linux/GCC/Windows не запускались. Использованы восстановленные на этапе 12 временные CMake/Ninja, дополнительных установок не потребовалось.

| Каталог | Результат |
|---|---|
| build/stage13-debug | CTest 85/85 |
| build/stage13-release | CTest 85/85 с NDEBUG |
| build/stage13-sanitize | CTest 85/85 с ASan+UBSan, без диагностик |

Сначала три конфигурации проверены в /private/tmp/nir-stage13-work, затем повторены на фактическом пути проекта:

```sh
cd /Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code
export PATH=/private/tmp/nir-stage12-tools/cmake/data/bin:/private/tmp/nir-stage12-tools/bin:$PATH

cmake -G Ninja -S . -B build/stage13-debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage13-debug --parallel 4
ctest --test-dir build/stage13-debug --output-on-failure

cmake -G Ninja -S . -B build/stage13-release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage13-release --parallel 4
ctest --test-dir build/stage13-release --output-on-failure

cmake -G Ninja -S . -B build/stage13-sanitize -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage13-sanitize --parallel 4
ctest --test-dir build/stage13-sanitize --output-on-failure
```

Дополнительно выполнены все восемь команд текущего раздела README «Запуск CLI» и validate из каталога примеров для long_blackout. Созданы results/stage13-single, results/stage13-long-blackout, results/stage13-reordered, results/stage13-suite, traces/stage13-recovery, results/stage13-generated. Журналы/метрики этих запусков игнорируются Git. Серия содержит 36 завершённых запусков с разрешённой незавершённостью отдельных задач; регенерированные JSON/CSV совпадают с examples/generated_recovery.

Логи: build/stage13-{debug,release,sanitize}/verification.log и build/stage13-release/cli_examples.log. Проверены C++20/предупреждения, NDEBUG/address,undefined, приватная граница JSON, Markdown-ссылки, форматирование, git diff --check и исходные хеши перед переносом. Номер этапа в CMake/--help/--build-info согласован. План и DOCX не менялись; коммит не создавался. Работа остановлена до этапа 14.
