# Этап 8 Статистическая сводка и графики

Даты подготовки и завершения: 2026-11-09–2026-11-10. Выполнен только этап 8 новой очереди экспериментов. Добавлена обработка накопленных основных серий 2–7: средние/SD, percentile bootstrap-интервалы, парные различия и научные графики. Формулы и исходники C++, native-метрики, входы и журналы не менялись. DOCX/текст НИР не редактировались; этап 9 не запускался.

## Реализация

Стенд уже сохранял показатели каждого запуска, native comparison/summary, seed и хеши. Не хватало статистики между seed, явного парного анализа и графиков. Добавлены tools/run_statistics.py, пакет tools/telemetry_stats/{data,bootstrap,analysis,plots}.py, закреплённые tools/requirements-statistics.txt, tests/statistics_tests.py, план/инструкция и [методика](statistics.md). CMake дополнен выключенной по умолчанию опцией TELEMETRY_ENABLE_STATISTICS_TESTS. README/.gitignore обновлены. Обычная C++-сборка и прежние 93 CTest не приобретают зависимостей от NumPy/Matplotlib; с дополнительной группой проверены 94 CTest.

В отдельном окружении анализа использованы NumPy/Matplotlib; статистика не встроена в алгоритмическое ядро. Проектное окружение build/statistics-venv установлено по закреплённому requirements. Подготовка и первые проверки выполнялись в /private/tmp, затем полный анализ — в проекте.

## Фиксированный план и интерпретация

[План](../experiments/statistics_stage8/plan.json) явно задаёт шесть основных серий network/timeout/kappa/coefficient/window/load, 20000 resamples, bootstrap seed 20261109, nominal confidence 95%, окно 0. Технические повторы, пробные запуски этапа 1 и старые материалы главы 4 исключены. Это 180 ячеек и 7200 выполнений вариантов, но по десять seed на ячейку, а не одна выборка из 7200 независимых наблюдений.

Внутри каждого case/variant/metric рассчитываются среднее, SD с делителем n−1, min/max и percentile bootstrap-интервал среднего. Resampling выполняется по целым seed, без добавления задач/узлов в n. Неопределённые значения остаются пропусками; при n=0 нет оценки, при n=1 нет SD/CI. При постоянной выборке вырожденный эмпирический интервал не означает известную без ошибки генеральную величину.

Парные различия направлены left−right и рассчитываются по одинаковым seed; bootstrap применяется к разностям полных пар. Стороны не resample независимо. Для алгоритмов требуются одинаковые полные входы; для параметров — только разрешённое отличие при той же трассе; для нагрузки/производительности — совпадение работ/трасс и только явное отличие arrival_ticks/nodes. Проверяются нативные границы окна и seed mapping.

Опорные уровни T₀=20 мс, κ=2, c=2, M=3; equal и moderate для четырёхузловой серии заданы до анализа. Между разными этапами данные не объединяются. Время переводится из микросекунд в миллисекунды, остальные определения показателей сохраняются. Средний P95 запусков не равен объединённому P95 задач. Latency относится к завершённой части, Detection/Recovery — к определённым наблюдениям; анализа цензуры нет.

Интервалы pointwise, без поправки на множество сравнений. При десяти seed bootstrap может неточно описывать хвосты; номинальные 95% не являются доказанной гарантией покрытия. P-values, универсальный победитель и статистическая значимость по пересечению отдельных интервалов не заявляются. Методические источники: [SciPy bootstrap](https://docs.scipy.org/doc/scipy/reference/generated/scipy.stats.bootstrap.html), [NIST paired bootstrap](https://www.itl.nist.gov/div898/software/dataplot/refman1/auxillar/bootplot.htm).

## Команды и результаты

Из корня проекта:

```sh
python3 -m venv build/statistics-venv
./build/statistics-venv/bin/python -m pip install -r tools/requirements-statistics.txt
cmake -G Ninja -S . -B build/experiment-stage8-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python" -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/experiment-stage8-release --parallel 4
ctest --test-dir build/experiment-stage8-release --output-on-failure
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results --out results/experiment-stage8-statistics
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results --out results/experiment-stage8-statistics-repeat
```

Повтор команды требует нового --out. [Инструкция](../experiments/statistics_stage8/README.md). В results/experiment-stage8-statistics/ находятся report.md, observations.csv, summary.csv, contrasts.csv, paired_observations.csv, cases.json, source_hashes.json, figure_data.csv, figures.json, statistics_manifest.json и figures/*.png/*.svg. Служебный manifest может различаться путём выхода; содержательные таблицы/рисунки проверяются точно.

## Фактические проверки

Свежая Release-сборка с TELEMETRY_ENABLE_STATISTICS_TESTS=ON: **94/94 CTest**, время 51.20 с. В дополнительной группе семь Python unittest; без опции прежний набор содержит 93 теста. Фактически проверенные зависимости: NumPy 2.5.4, Matplotlib 3.11.2, Python 3.14.5.

Основной анализ complete: **6 серий, 180 ячеек, 7200 запусков** прочитаны без новых моделирований. Получены **8640 сводных строк**, **15552 парные сводки**, **155520 строк исходных пар** и **34 фигуры × PNG/SVG = 68 изображений**. Проверены 61566 файлов первоисточников и 7200 соответствий native metrics/CSV.

Повтор полного анализа в другой каталог сохранил хеши всех содержательных CSV/JSON/MD/PNG/SVG из artifacts_sha256, включая все 68 изображений. Служебный manifest не сравнивался из-за нового --out; кэш Matplotlib не является научным артефактом. Отдельная проверка стандартной библиотекой пересчитала средние/SD/линейные квантили в 12 контрольных группах, включая полные/неполные пары, n=0/1 и постоянные наблюдения.

Источники не изменились: сохранены 81022 прежних файлов results/ и все исходники этапов 1–7/модули C++. Время основного анализа 37.538 с; размер основных артефактов 54943750 байт — технические показатели. Логи/машинные проверки: build/experiment-stage8-release/{dependencies.log,verification.log,stage8_run.log,statistics_checks.json,independent_statistics_checks.json}.

В summary.csv **734 групп без определённых значений**, **238 частично определённых**. Большинство n=0 — Detection/Recovery в healthy, где физического отказа нет. Это не число ошибок и не повод заменить пропуски нулями.

Импорт сверил manifests/status/seed mapping, SHA-256 входов и пяти содержательных файлов каждого запуска, единицы, балансы, native CSV и причины пропусков. Общие totals выбраны один раз через window_index=0; окна не суммировались. Источники повторно сверены после анализа. Новые generate/run/batch/metrics в основной обработке не вызывались; native CLI использовался только в технических тестовых фикстурах.

Новая группа содержит семь Python unittest: точный малый resampling-пример с известными квантилями; пропуски/одиночное наблюдение/постоянная выборка/дубли; независимость от порядка чтения; пересечение seed для пар; полный импорт/PNG–SVG/точный повтор/сохранность; неверные планы и дубли источников; повреждённые native-данные с failed и без успешных таблиц. NumPy не заменяет определения метрик C++: анализируются уже сохранённые показатели запусков.

Для графиков проверены данные figure_data.csv, размеры изображений, обе формы экспорта, границы текстовых полей и отсутствие наложения x-tick labels. Все 34 фигуры просмотрены; частичные n вынесены справа от панелей, тесные отметки оси κ развёрнуты. Зазоры для n=0 не превращались в нулевые задержки. Счётчики/Detection RR и LL свёрнуты в одну линию только после проверки равенства.

Самопроверка выполнена основным агентом без нового субагента. Debug/ASan/UBSan повторно не запускались: C++ не менялся. Проверенная среда анализа — Python 3.14/macOS arm64; переносимость на другие окружения не удостоверяется. Ограничения исходных экспериментов и конечных потоков сохраняются.

## Примеры из парных сводок

| Условия load | Разность | Показатель | Средняя Δ, мс | 95% empirical CI, мс | n |
|---|---|---|---:|---|---:|
| heterogeneous_moderate_clean_healthy | fixed_ll−fixed_rr | latency_ms | -15.215 | [-16.041; -14.346] | 10 |
| equal_moderate_clean_crash | adaptive_ll−adaptive_rr | latency_ms | 0.082 | [0.035; 0.135] | 10 |
| equal_moderate_clean_crash | adaptive_rr−fixed_rr | detection_ms | -20.000 | [-20.000; -20.000] | 10 |

Отрицательная разность latency LL−RR в первом примере поддерживает наблюдавшееся преимущество LL именно в неоднородном moderate/clean/healthy. Второй пример показывает обратный знак в конкретной равной конфигурации. Интервалы разведочные, без поправок на множество сравнений: универсальная значимость не заявляется. Третий интервал вырожден, потому что все десять seed имеют одинаковую разность обнаружения в чистом канале; это не доказательство нулевой неопределённости в других условиях.

В summary.csv есть группы без определённых Detection/Recovery, в том числе healthy без физических отказов. Они не являются ошибкой моделирования и не получают нулевую задержку. Неполные/одиночные наблюдения и условные выборки обозначены явно.

Блокирующих сторонних ошибок не обнаружено. Этап 8 завершён; следующий — граничные времена heartbeat и отдельный анализ σ=0 (этап 9), только по отдельному поручению.
