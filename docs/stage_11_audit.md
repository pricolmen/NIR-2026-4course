# Этап 11: журнал, метрики и самоаудит

Дата: 2026-10-19. Завершён только этап 11; работа останавливается до CLI и сравнения этапа 12.

## Реализация

Добавлены [JournalWriter/read_journal](../include/telemetry/io/journal.hpp), [пакет результатов и пересчёт](../include/telemetry/io/run_output.hpp), [Analyzer и нативные отчёты](../include/telemetry/metrics/analysis.hpp), их реализации в src/io и src/metrics, девять групп [metrics_tests.cpp](../tests/metrics_tests.cpp). Форматы/API/ограничения: [journal_metrics.md](journal_metrics.md).

Поток JSONL сохраняет все события и диагностику адаптера, с целыми временем/фазой/ID/поколениями, ключом движка и номером записи. Вещественные поля журнала кодируются десятичными строками long double с max_digits10 без промежуточного double. Записываются начало/успешное окончание; оборванный поток не принимается за завершённый.

Каждый успешный write_run создаёт manifest.json, events.jsonl, tasks.csv, attempts.csv, nodes.csv, metrics.json и точные снимки input/scenario.json/input/delivery.csv. Контрольные суммы FNV-1a-64 стабильны, явно названы и не являются криптографической защитой. Явный override CSV при загрузке снимка позволяет перенести результаты, не изменяя числа или исходный JSON. Manifest остаётся incomplete до успеха всех операций. Непустой каталог не перезаписывается.

Analyzer получает только сценарий и события, без чтения состояния Simulation. Независимый пересчёт читает сохранённый журнал и снимки, проверяет целостность и не доверяет прежним метрикам/CSV. Вычисляются Detection, FP/FN, crash после ложного исключения, Recovery по фиксированному R, throughput, latency/P95, повторы и отклонённые отозванные результаты. Проверяются баланс и множества задач/попыток. Новая библиотека metrics не получает JSON; core/engine/simulation и примеры JSON/CSV не менялись. IO зависит от metrics с транзитивной simulation; CLI получает IO без повторной линковки simulation.

## Фактические проверки

| CTest | Результат и существенные границы |
|---|---|
| metrics_baseline | 4 завершены, latency mean=4.5, P95=8 тиков, throughput=200000 задач/с; пустые Detection/Recovery |
| metrics_recovery | Crash 3, FAILED 8, окончательные завершения 14/18: Detection=5, Recovery=15; пустой R даёт 5, незавершённый R — null/recovery_incomplete; назначения после crash входят; при следующем crash и третьей попытке исходное R остаётся прежним |
| metrics_classification | FP в 8 и crash в 12 сохраняют FP, дают отдельный счётчик и причину, без FN/Detection; crash на горизонте без FAILED — FN; crash и FAILED одной отметки — корректное обнаружение с нулевым Detection |
| metrics_boundaries | Пустая/единичная выборка; безопасный P95 для 19/20/21 и SIZE_MAX; начало окна исключено, конец включён; незавершённость на горизонте; тики 9007199254740993 и UINT64_MAX ID без double |
| metrics_retries | Три назначения — два повтора; два результата старых отозванных физических попыток отклонены, баланс задач не удваивается |
| metrics_journal | Полная каноническая запись/чтение и независимый пересчёт трёх примеров во всех четырёх вариантах; сохранение raw/rounded порогов, статистики, поколений, потерь/blackout/горизонта, старого номера и heartbeat после FAILED |
| metrics_files | Все шесть файлов и снимки; повторный анализ, восстановление испорченного metrics.json из журнала; запрет перезаписи; перенос исходников/каталога результата; сохранение исходных байтов и nextafter-точности работы |
| metrics_errors | Отсутствие/лишние записи после footer, сбой нумерации, float-тиковые поля, дубликаты ключей, неизвестные поля/enum/виды, неверная фаза; ошибка output stream; incomplete manifest, изменённые журнал/CSV; закрытый Analyzer и nullptr |
| metrics_reproducibility | Повтор в обратном порядке четырёх вариантов даёт равные Analysis и побитово одинаковые manifest, events, metrics и три CSV в одной сборке |
| 57 регрессионных тестов | Полный прежний набор этапов 1–10, включая сквозные фазовые сценарии и независимость скрытого crash |

Во всех новых сквозных прогонах helper сравнивает Analysis живого потока с отдельным Analyzer, прочитавшим JSONL. Условия тестов остаются активны в Release. Файлы фикстур/результатов размещаются внутри каждого каталога сборки, исходные примеры не переписываются.

Реально созданный baseline: build/stage11-debug/stage11-tests/files/baseline/. Ручной Recovery с временами 3/8/14/18: build/stage11-debug/stage11-tests/files/run-moved/. Файлы игнорируются Git. В сохранённом crash-примере fixed RR дополнительно подтверждены Detection=17 и Recovery=25 тиков.

## Самоаудит

Самоаудит выполнен основным агентом по разделу 2.2 плана: независимый субагент на этапе 11 не запускался. План предусматривает следующий независимый аудит после этапа 14. Фактические настройки модели/усилия основного агента не утверждаются.

Проверены: отсутствие обратных зависимостей в core/engine; потоковый характер записи/чтения; сохранение целых значений и вещественной точности; семантика FP перед crash, FN и пустого/незавершённого Recovery; неизменное членство R; уникальность принятия и баланс; различение неполного запуска и незавершённых задач; одноразовость Analyzer, обработка ошибок записи; детерминированные порядок/locale/вывод и переносимость снимков.

Во время реализации исправлено лишнее промежуточное преобразование при throughput: используется эквивалентная count*1e6/duration_ticks с одним делением, чтобы не вносить погрешность округления секунд в ручной baseline. Снимок JSON сохраняется без DOM-пересериализации, чтобы не потерять точность long double. Устранено предупреждение линкера о повторной библиотеке. В тесте трёх попыток уточнена трасса: второй узел должен оставаться ALIVE на первом переназначении; его последующий FAILED проверяется отдельно. Финальные сборки без предупреждений и диагностик санитайзеров.

Границы самоаудита: анализатор проверяет согласованность журнала/метрик, не переисполняет выбор RR/LL или статистический алгоритм; он не служит доказательством подлинности намеренно переписанного набора файлов. Полный журнал в памяти не хранится, но реестры задач/попыток и выборка latency для P95 занимают память. Сбой перезаписи производных отчётов может оставить часть CSV/metrics обновлёнными — журнал остаётся источником восстановления, исключение не скрывается.

## Сборки

macOS 26.6.2, arm64; Apple Clang 17.0.0 для Debug/Release; Homebrew LLVM 23.1.3 для ASan+UBSan; CMake 3.31.6, Ninja 1.11.1, nlohmann/json 3.12.0. Linux/GCC и другие платформы не запускались.

| Каталог | Результат |
|---|---|
| build/stage11-debug | 66/66 CTest |
| build/stage11-release | 66/66 CTest, NDEBUG |
| build/stage11-sanitize | 66/66 CTest, ASan+UBSan, без диагностик |

Сначала три конфигурации проверены в /private/tmp/nir-stage11-work, затем повторены в фактическом проекте. Точные команды повторной проверки (использована кешированная фиксированная зависимость):

```sh
cd /Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code
export PATH=/private/tmp/nir-stage1-tools/cmake/data/bin:/private/tmp/nir-stage1-ninja/bin:$PATH

cmake -G Ninja -S . -B build/stage11-debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage11-debug --parallel 4
ctest --test-dir build/stage11-debug --output-on-failure

cmake -G Ninja -S . -B build/stage11-release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src
cmake --build build/stage11-release --parallel 4
ctest --test-dir build/stage11-release --output-on-failure

cmake -G Ninja -S . -B build/stage11-sanitize -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH=/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code/build/debug/_deps/nlohmann_json-src -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON
cmake --build build/stage11-sanitize --parallel 4
ctest --test-dir build/stage11-sanitize --output-on-failure

./build/stage11-sanitize/telemetry_sim --build-info
git diff --check
```

Проверены compile_commands.json: C++20, все предупреждения, NDEBUG, флаги address,undefined; JSON-заголовки только у build_info.cpp, scenario_loader.cpp, journal.cpp, run_output.cpp, build_smoke.cpp и scenario_tests.cpp. Новый analysis.cpp и metrics_tests.cpp JSON include не получают. Проверены Markdown-ссылки, форматирование и исходные хеши перед переносом файлов. Логи основного каталога: build/stage11-{debug,release,sanitize}/verification.log.

## Оставшийся объём

CLI validate/run/compare/metrics, генерация/batch и общие comparison/summary остаются этапам 12–13. simulation_available=false пока обозначает отсутствие CLI моделирования. Нагрузочная серия, fuzzing, исчерпание памяти и итоговый независимый аудит на этом этапе не проводились. Числа демонстрационных примеров не доказывают преимущества алгоритмов. План/DOCX не менялись, коммит не создавался.
