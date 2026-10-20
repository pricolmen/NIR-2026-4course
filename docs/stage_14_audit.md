# Этап 14: итоговые проверки и независимый аудит

Дата: 2026-10-20. Объём — раздел 12 плана, Debug/Release/ASan+UBSan, повторные запуски и конечный нагрузочный smoke. Работа останавливается до передачи/документации этапа 15.

## Проверяемый набор

Добавлены [final_verification.py](../tests/final_verification.py), [конфигурация smoke](../tests/data/load_smoke.json) и CTest final_load_smoke. Номер этапа CMake/CLI — 14. В CTest теперь 86 тестов: прежние 85 и новая проверка. Драйвер использует стандартную библиотеку Python, вызывает реальный CLI без оболочки и не отключает условия при оптимизации.

| Требование раздела 12 | Проверки |
|---|---|
| 12.1: RR, исключение следующего, пустой ALIVE, восстановление/совместные изменения | scheduling_rr, scheduling_availability, reconfiguration_availability, integration_simultaneous; примеры no_alive/simultaneous |
| 12.1: полная работа Load, влияние p, точное равенство/ID, немедленное обновление | scheduling_ll, scheduling_integration, scheduling_numeric |
| 12.1: однократное снятие нагрузки, прежние/повторные результаты, пустое назначение | tasks_results, tasks_retries, reconfiguration_results, scheduling_numeric, integration_blackout |
| 12.2: первый контакт, прогрев M/m_min, окно [10,12,8], sample std=2, пороги 14/28 | detector_fixed, detector_adaptive, detector_window |
| 12.2: нулевые/увеличенные интервалы, замороженные пороги, поколения, старый номер/FAILED | detector_window, detector_generations, detector_boundaries, examples_reordered/long_blackout |
| 12.3: HB до таймера, crash до завершения, результат до FAILED, нулевая задержка, края blackout | physical_crash/channel/horizon, engine_phases, integration_boundaries |
| 12.3: ложный FAILED не отменяет физическую очередь; общий возврат, один dispatch | physical_exclusion, reconfiguration_batch, integration_blackout/simultaneous, examples_long_blackout/simultaneous |
| 12.3: включённый горизонт/все фазы, технический лимит не успех | engine_horizon/limits, integration_errors/boundaries, examples_unfinished |
| 12.4: baseline RR [4,2,8,4], mean 4.5/P95 8, throughput 200000 | integration_baseline, metrics_baseline, commands_run/compare |
| 12.4: crash 3, FAILED 8, завершения 14/18 → Detection 5/Recovery 15; пустой и незавершённый R | metrics_recovery с сохранением/независимым чтением корректного журнала |
| 12.5: P95 (пусто/1/ранг), (t1,t2], FP до crash, FN у горизонта | metrics_boundaries, metrics_classification |
| 12.5: три назначения=два повтора, старые попытки не нарушают баланс; пустые задачи | metrics_retries, integration_blackout/boundaries, commands_compare/generate |
| 12.5: одинаковые разрешённые наблюдения при разном скрытом crash | integration_isolation; core не получает физический crash |
| 12.5: канонический повтор, обратный порядок вариантов, пересчёт файлов | metrics_reproducibility, commands_determinism, examples_*, final_load_smoke |
| 12.5: неверные ID/периоды/числа/дубликаты/трасса/переполнение | common_numeric/ids/time, detector_inputs, scenario_json/csv/ranges, commands_validate/generate/failures, metrics_errors |

## Конечный нагрузочный smoke

Входная конфигурация — техническая фикстура, без научного обоснования параметров: 6 узлов с разными p, 2000 задач, горизонт 8000 тиков, H=25, 1920 потенциальных heartbeat. Работа 4..18, поступления с периодом 2, задержка 0..6, q=0.01, seed=140014. Crash узла 1 в 3000, blackout узла 2 [2400,2700) создаёт ложный FAILED. Конфигурация и сгенерированные JSON/CSV записаны до моделирования.

В каждом каталоге сборки final_load_smoke выполняет generate, compare четырёх вариантов, отдельные run в обратном порядке и четыре независимых metrics. По каждому варианту совпадают SHA-256 manifest/events/metrics/трёх CSV, исходные и сохранённые входы. Проверяются успешный горизонт, порядок журнала, одна реконфигурация/dispatch отметки, уникальность принятия, баланс и согласование журнала с метриками. Корректно учтённая незавершённость допустима; значение результата не подменяется заранее заданной метрикой.

Полный машинный отчёт: build/stage14-{debug,release,sanitize}/stage14-load/report.json. В него входят размеры, фактическое число действий/записей/байт, счётчики, SHA-256, сведения о компиляторе/платформе и реальные wall-clock интервалы. Времена измеряются только в служебном отчёте, не в каноническом events.jsonl. Время compare включает загрузку, четыре моделирования и запись файлов; полная проверка — также обратные повторы, пересчёт и проверку файлов. Это не измерение преимущества алгоритма.

| Сборка | generate, с | compare ×4, с | Вся проверка, с | Артефакты, байт |
|---|---:|---:|---:|---:|
| debug | 0.757 | 2.546 | 12.764 | 54640017 |
| release | 0.344 | 0.376 | 2.133 | 54640051 |
| sanitize | 0.593 | 9.086 | 43.826 | 54639928 |

Фактические счётчики Release (число действий движка отличается от числа записей журнала):

| Вариант | Действия | Записи | Байт журнала | Завершено | Осталось | Повторы | FP / FN |
|---|---:|---:|---:|---:|---:|---:|---:|
| fixed_rr | 13300 | 23114 | 6392835 | 2000 | 0 | 60 | 1 / 0 |
| fixed_ll | 13263 | 22967 | 6356384 | 2000 | 0 | 5 | 1 / 0 |
| adaptive_rr | 13348 | 23154 | 6635478 | 2000 | 0 | 56 | 1 / 0 |
| adaptive_ll | 13278 | 22980 | 6596002 | 2000 | 0 | 4 | 1 / 0 |

## Независимый аудит

Проверяющий запущен в отдельном свежем контексте: GPT-6.1 Sol (`gpt-6.1-sol`), усилие High (`high`); параметры подтверждены фактическим вызовом запуска. Основной агент выполнил реализацию и проверки; модель/усилие основного агента этим отчётом не удостоверяются. Проверяющему переданы полный исходный план, контракты, подготовленный код и результаты прогонов. Исходники он не менял и своих агентов не создавал.

Подтверждённых ошибок и существенных пробелов обязательных проверок не обнаружено. Проверены границы core/engine/IO, отсутствие скрытого crash в алгоритмах, целочисленное время, восемь фаз, RR/LL, fixed/adaptive, совместный отзыв, прежние физические попытки, метрики, CLI и изоляция вариантов.

Независимые проверки подготовленной копии:

- Debug: 86/86 CTest, 14.92 с; Release: 86/86, 3.31 с.
- ASan+UBSan: 85/85 без повторного нагрузочного smoke, 14.55 с, без диагностик. Полный набор 86/86 с нагрузкой отдельно выполнен основным агентом.
- Собственная Python-модель восьми фаз: 100 детерминированных сценариев × 4 варианта = 400 запусков Release. Точно совпали назначения, номера попыток, принятые завершения и состояния. Независимо сверены FP/FN, crash после ложного исключения, Detection/Recovery, P95 и throughput. Менялись производительности, потери/задержки/перестановки heartbeat, blackout/crash и пустые потоки задач.

Временные материалы аудитора: /private/tmp/nir-stage14-review/differential.py, case000…case099; они не являются частью поставки и могут быть удалены системой. Проверяющий использовал подготовленные бинарники, новой сборки с нуля не выполнял. Собственные сценарии небольшие и используют обычные численные диапазоны; экстремальные границы проверены существующими тестами и просмотром кода. Итоговый повтор сборок в основном каталоге выполнен основным агентом после аудита.

## Сборки

Платформа: macOS 26.6.2 arm64. Debug/Release — Apple Clang 17.0.0, ASan+UBSan — Homebrew LLVM 23.1.3. CMake 3.31.6, Ninja 1.11.1.4 (пакет), Python 3.14.5, nlohmann/json 3.12.0. Linux/GCC/Windows не проверялись.

| Каталог | Результат |
|---|---|
| build/stage14-debug | 86/86 CTest |
| build/stage14-release | 86/86 CTest с NDEBUG |
| build/stage14-sanitize | 86/86 CTest с ASan+UBSan, без диагностик |

Подготовленная копия /private/tmp/nir-stage14-work проверена до переноса; затем сборки повторены в фактическом проекте. Команды:

```sh
cd /Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code
export PATH=/private/tmp/nir-stage12-tools/cmake/data/bin:/private/tmp/nir-stage12-tools/bin:$PATH

cmake -G Ninja -S . -B build/stage14-debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage14-debug --parallel 4
ctest --test-dir build/stage14-debug --output-on-failure

cmake -G Ninja -S . -B build/stage14-release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage14-release --parallel 4
ctest --test-dir build/stage14-release --output-on-failure

cmake -G Ninja -S . -B build/stage14-sanitize -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage12-tools/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage14-sanitize --parallel 4
ctest --test-dir build/stage14-sanitize --output-on-failure
```

Логи основного каталога: build/stage14-{debug,release,sanitize}/verification.log. Проверены C++20/все предупреждения, NDEBUG и address,undefined, границы JSON/common/core/engine, сохранение исходных изменений, Markdown-ссылки и git diff --check.

## Границы

Эта проверка покрывает конечную фикстуру, известные крайние случаи и одну программную среду. Fuzzing, исчерпание памяти и конкурентные записи не выполнялись; они не заявляются проверенными. Полная межплатформенная побитовая одинаковость вещественных расчётов/стандартных распределений не обещается. Канонический поток не содержит wall-clock времени. Показатели демонстраций не являются выводом о научном превосходстве.

Итоговая передача, architecture.md/experiments.md и оформление всей документации относятся к этапу 15. План/DOCX не изменялись; коммит автоматически не создавался. После завершения этапа 14 работа останавливается.
