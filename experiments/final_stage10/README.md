# Итоговая фиксация экспериментов, этап 10

Это заключительный этап новой очереди 1–10. Исходная реализация стенда 0–15 остаётся версии 0.1.0. Нативные алгоритмы, формулы, время, фазы и метрики не менялись. [Итоговый аудит](../../docs/experiment_stage_10_audit.md).

## Что зафиксировано

`plan.json` перечисляет основные серии и ожидаемые размеры; `lock.json` содержит SHA-256 исполняемых исходников/тестов, JSON/CSV-конфигураций, зависимостей анализа и содержательного индекса каждой серии. Markdown и сам lock исключены из хеширования во избежание самоссылки. Lock не обновляется автоматически: новая версия кода или новый научный план требуют отдельной фиксации и проверки. Это контроль воспроизводимости, не криптографическая подпись автора.

| Этап | Каталог внутри results/ | Ячеек | Выполнений вариантов |
|---|---|---:|---:|
| 1: проверка инфраструктуры | experiment-stage1-seeds | 1, два seed | 8 |
| 2: сеть | experiment-stage2-network | 18 | 720 |
| 3: T₀ | experiment-stage3-timeout | 30 | 1200 |
| 4: κ | experiment-stage4-kappa | 30 | 1200 |
| 5: c | experiment-stage5-coefficient | 36 | 1440 |
| 6: M | experiment-stage6-window | 30 | 1200 |
| 7: нагрузка | experiment-stage7-load | 36 | 1440 |
| 8: статистика | experiment-stage8-statistics | 180 исходных | 0 новых |
| 9: границы | experiment-stage9-boundary | 60 | 240 |

Итого 7448 выполнений, включая технические демонстрации и детерминированные границы. В статистике только 7200 выполнений этапов 2–7, по десять seed 2001–2010 на ячейку. Связанные настройки и четыре варианта не увеличивают число независимых наблюдений. Этап 1 использует seed 101/202.

Итоговые условия остаются в исходных планах: [сеть](../network_stage2/plan.json), [T₀](../timeout_stage3/plan.json), [κ](../kappa_stage4/plan.json), [c](../coefficient_stage5/plan.json), [M](../window_stage6/plan.json), [нагрузка](../load_stage7/plan.json), [статистика](../statistics_stage8/plan.json), [границы](../boundary_stage9/plan.json). Базовые генераторы лежат рядом с сетевым и нагрузочным планами; демонстрация использует examples/generator.json.

Для двух узлов: по 1000000 ед./с, 40 задач, H=10 мс, T₀=20 мс, κ=2, c=2, M=3, min_intervals=2, горизонт 600 мс; физический crash при его наличии в 200 мс. H — период отправки heartbeat; T₀ — исходный тайм-аут; κ — множитель срока FAILED; c — коэффициент при стандартном отклонении интервалов; M — ёмкость истории интервалов; min_intervals — минимум интервалов для прогрева. q — вероятность потери heartbeat. Сеть: задержки [10;10]/[5;15]/[0;20] мс и q=0/0.05/0.15, все сочетания в этапе 2.

Параметрические серии используют шесть условий clean/medium/stressed × healthy/crash: T₀=10/15/20/30/40 мс; κ=1.25/1.5/2/3/4; c=0/0.5/1/2/3/4; M=2/3/5/10/20. Меняется только исследуемый параметр, входы взяты из этапа 2. T₀ участвует также в прогреве adaptive, κ — в обоих детекторах. В серии M минимум min_intervals=2 фиксирован.

Четыре узла: 120 задач, поступления через 4/3/2 мс; равная мощность 1/1/1/1 млн ед./с либо 0.5/0.75/1/1.75 млн. В crash отказывает узел 3 мощностью 1 млн в 200 мс; общая/потерянная мощность одинакова. Горизонт 600 мс. Это конечные потоки, а heartbeat задаются внешней трассой независимо от CPU.

Статистика: 20000 bootstrap-выборок целых seed, bootstrap_seed=20261109, уровень 95%, окно 0, парные разности по совпадающим seed. [Методика](../../docs/statistics.md). Граничные сценарии явные, без RNG, исследуют ±1 мкс, σ=0, равновременность, порядок номеров, округление и горизонт. σ — выборочное стандартное отклонение интервалов принятых heartbeat.

## Окружение и три проверяемые сборки

Все команды выполняются из корня проекта. Нужны CMake, Ninja, C++20 и Python. Проверенная среда: macOS 26.6.2 arm64, Apple Clang 17.0.0, CMake 4.4.4, Ninja 1.13.2, Python 3.14.5; санитайзеры — Homebrew LLVM 23.1.3. Статистические пакеты закреплены в tools/requirements-statistics.txt. Для точного сравнения рисунков используйте ту же среду; совпадение между платформами не обещается.

```sh
python3 -m venv build/statistics-venv
./build/statistics-venv/bin/python -m pip install -r tools/requirements-statistics.txt

cmake -G Ninja -S . -B build/experiment-stage10-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python"
cmake --build build/experiment-stage10-debug --parallel 4
ctest --test-dir build/experiment-stage10-debug --output-on-failure

cmake -G Ninja -S . -B build/experiment-stage10-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python"
cmake --build build/experiment-stage10-release --parallel 4
ctest --test-dir build/experiment-stage10-release --output-on-failure

cmake -G Ninja -S . -B build/experiment-stage10-sanitize -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python"
cmake --build build/experiment-stage10-sanitize --parallel 4
ctest --test-dir build/experiment-stage10-sanitize --output-on-failure
```

Для офлайн-сборки к каждому configure добавьте `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/build/debug/_deps/nlohmann_json-src"`, если проверенный JSON 3.12.0 уже там. Именно такая подстановка использована в итоговой проверке. При другой установке LLVM укажите его реальный путь. Основной набор содержит 95 CTest; с опцией статистики — 96. Для повторения после переноса проекта используйте новые каталоги сборки.

## Одна команда полного повтора

Нужны девять основных каталогов из таблицы. Они не хранятся в Git: вместе с кодом сохраните их отдельным архивом, включая manifests, inputs, журналы и отчёты. Технические `*-repeat`, `*-replay-*`, старые stage12/13/14 и пользовательские пробы в итоговый научный набор не входят. Их наличие не требуется этой команде. Для сохранения конкретного результата сохраняйте весь каталог серии, а не только metrics.json.

```sh
./build/statistics-venv/bin/python tools/verify_experiments.py --results-root results --program ./build/experiment-stage10-release/telemetry_sim --reproduce --replay-metrics --out results/my-final-check
```

Выберите новый --out. Скрипт сначала сверит код/конфигурации и все эталонные серии с lock; при расхождении остановится. Затем заново выполнит generate/batch через прежние обёртки, построит статистику, создаст границы, сравнит все входы и содержательные результаты, перечитает все 7448 журналов командой native metrics. Метрики пересчитываются только в новом повторе. Результаты будут в `results/my-final-check/reproduction/`, отчёт — `final_manifest.json`, подробные контрольные индексы — `reference_index.json` и `reproduction_index.json`, команды/вывод — `reproduction.log` и `metrics_replay.log`.

Без нового моделирования и зависимостей статистики:

```sh
python3 tools/verify_experiments.py --results-root results --out results/my-frozen-check
```

Этот режим проверяет сохранённые входы, варианты, статусы, балансы, хеши, таблицы/графики и lock. Он не проверяет заново бинарник и не добавляет новые реализации. Завершённость определяется `status=complete`; `failed`/`incomplete` не являются успешным экспериментом. Служебные manifests могут иметь другие пути, Git/build provenance и хеши родительских manifests; для сравнения используются входы и events.jsonl/metrics.json/tasks.csv/attempts.csv/nodes.csv, native comparison/summary, статистические CSV/figures.json и все 68 изображений. Служебные файлы проверяются на целостность внутри своей серии, но их побитовое равенство между деревьями не требуется.

## Восстановление серий с нуля без архива results/

Если исходные серии удалены, сначала создайте их следующими командами в новом пустом каталоге `results/restored-final`. Здесь каждый --out должен отсутствовать. После этого режим проверки с --results-root results/restored-final сопоставит их с сохранённым lock. Архив остаётся нужен для восстановления прежних сведений о происхождении; повторные вычисления создадут новые manifests.

```sh
./build/statistics-venv/bin/python tools/run_seed_series.py --template examples/generator.json --seeds 101 202 --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage1-seeds
./build/statistics-venv/bin/python tools/run_network_series.py --plan experiments/network_stage2/plan.json --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage2-network
./build/statistics-venv/bin/python tools/run_timeout_series.py --plan experiments/timeout_stage3/plan.json --source-network results/restored-final/experiment-stage2-network --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage3-timeout
./build/statistics-venv/bin/python tools/run_kappa_series.py --plan experiments/kappa_stage4/plan.json --source-network results/restored-final/experiment-stage2-network --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage4-kappa
./build/statistics-venv/bin/python tools/run_coefficient_series.py --plan experiments/coefficient_stage5/plan.json --source-network results/restored-final/experiment-stage2-network --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage5-coefficient
./build/statistics-venv/bin/python tools/run_window_series.py --plan experiments/window_stage6/plan.json --source-network results/restored-final/experiment-stage2-network --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage6-window
./build/statistics-venv/bin/python tools/run_load_series.py --plan experiments/load_stage7/plan.json --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage7-load
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results/restored-final --out results/restored-final/experiment-stage8-statistics
./build/statistics-venv/bin/python tools/run_boundary_study.py --plan experiments/boundary_stage9/plan.json --program ./build/experiment-stage10-release/telemetry_sim --out results/restored-final/experiment-stage9-boundary
python3 tools/verify_experiments.py --results-root results/restored-final --out results/restored-final-check
```

## Выводы и пределы

В чистом канале адаптивное обнаружение может выигрывать 20 мс, но результат зависит от распределения интервалов и параметров. Ни adaptive, ни fixed, ни RR/LL не имеют универсального преимущества. LL выигрывает в части неоднородных условий; для равных узлов встречается преимущество RR. Увеличение тайм-аутов уменьшает агрессивность исключения ценой скорости реакции. Более быстрое Detection не гарантирует более быстрое Recovery или завершение большего числа задач.

Detection — время от физического отказа до FAILED. Recovery — время от физического отказа до принятия всех необходимых результатов восстановления; оно может не завершиться до горизонта. Пропуски нельзя заменять нулями или сравнивать условные средние без числа определённых наблюдений. Ложные исключения и незавершённость анализируются рядом с задержками.

При σ=0 добавка cσ равна нулю, поэтому увеличение c не создаёт дополнительный запас. Принятый свежий heartbeat ровно на сроке таймера обрабатывается раньше проверки и обновляет поколение; после срока на один тик FAILED уже окончателен. Это подтверждённая семантика существующей формулы. α/минимальная σ не вводились.

Десять seed на ячейку и разведочные точечные bootstrap-интервалы без поправки на множественные сравнения не подтверждают редкие вероятности и универсальную оптимальность. Вырожденный интервал на константных данных не доказывает нулевую неопределённость. Проверено моделирование, а не реальная распределённая сеть; межплатформенное побитовое равенство не заявляется. DOCX/текст НИР не редактировались.
