# Этап 8 Статистическая сводка и графики

[Методика и ограничения](../../docs/statistics.md), [аудит](../../docs/experiment_stage_8_audit.md), [фиксированный план](plan.json). Анализ использует основные сохранённые серии этапов 2–7, без генерации и новых моделирований. Технические replays, пробная серия этапа 1 и прежние материалы главы 4 исключены.

## Окружение и запуск из корня

Проверено на Python 3.14/macOS arm64. Пакеты закреплены в [requirements-statistics.txt](../../tools/requirements-statistics.txt). Это отдельное окружение анализа; нативная программа не приобретает зависимости от NumPy/Matplotlib.

```sh
python3 -m venv build/statistics-venv
./build/statistics-venv/bin/python -m pip install -r tools/requirements-statistics.txt
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results --out results/experiment-stage8-statistics
```

--results-root содержит каталоги из plan.json; --out обязан быть новым и вне источников. Если прежние серии удалены, восстановите именно сохранённые материалы этапов 2–7. Анализ не регенерирует их скрыто. Параметры resamples=20000, seed=20261109, CI=95%, window_index=0 заданы до анализа.

Запуск всех тестов с дополнительной статистической группой:

```sh
cmake -G Ninja -S . -B build/experiment-stage8-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_STATISTICS_TESTS=ON -DPython3_EXECUTABLE="$PWD/build/statistics-venv/bin/python"
cmake --build build/experiment-stage8-release --parallel 4
ctest --test-dir build/experiment-stage8-release --output-on-failure
```

Для автономной настройки добавьте `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$PWD/build/debug/_deps/nlohmann_json-src"`, если сохранённая зависимость доступна там. Опция статистики выключена по умолчанию: обычный набор сохраняет 93 CTest, с ней — 94. Нижеописанный анализ может выполняться без нового C++-бинарника: он читает старые результаты.

## Содержимое результата

| Путь | Содержание |
|---|---|
| statistics_manifest.json, plan.json | Статус, настройки, версии, хеши и счётчики |
| observations.csv | 7200 строк native-показателей по seed с причинами пропусков |
| summary.csv | 8640 сводок средних/SD/95%-ных интервалов, n_expected/n_defined/n_missing |
| contrasts.csv | 15552 сводки парных различий left−right, включая размеры обеих сторон |
| paired_observations.csv | 155520 исходных пар/пропусков, без суммирования связанных условий |
| cases.json, source_hashes.json | Разметка условий и контрольные суммы первоисточников |
| figure_data.csv, figures.json | Точные данные графиков и их список |
| figures/*.png, figures/*.svg | 34 фигуры в двух форматах, всего 68 файлов |
| report.md | Краткая сводка и ограничения интерпретации |

Это число строк текущего плана, а не число независимых реализаций. На ячейку остаются десять seed. Неопределённые Detection/Recovery не заменяются нулём. Интервалы условны на доступные наблюдения; для парных сравнений используется пересечение определённых seed. Средний P95 запусков не равен объединённому P95 задач.

Направление всех разностей — left−right, поэтому LL−RR<0 для задержки означает меньшую задержку LL. Это не утверждение универсального превосходства. Нулевой вырожденный интервал FP не доказывает нулевой риск. Интервалы pointwise, без поправок на множество сравнений; при малом n покрытие может быть неточным. Подробнее — в методике.

Графики охватывают сеть, T₀, κ, c, M и четырёхузловую нагрузку. n для неполных выборок показаны справа от панели в порядке x. Полные CSV сохраняют все варианты, включая неизменные контроли. Источники и их содержательные файлы проверяются по SHA-256 до/после анализа. Служебные manifests не требуют побитового равенства разных выходных каталогов.

Повторите анализ в новый каталог для проверки воспроизводимости:

```sh
./build/statistics-venv/bin/python tools/run_statistics.py --plan experiments/statistics_stage8/plan.json --results-root results --out results/experiment-stage8-statistics-repeat
```

Этап 9 (граничные времена heartbeat и отдельный анализ σ=0) не выполняется этим скриптом.
