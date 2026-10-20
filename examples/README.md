# Каталог демонстрационных сценариев

Все времена — целые микросекундные тики; работа условная, p=1 000 000 единиц/с. В каждом каталоге — полностью сохранённые scenario.json/delivery.csv и описание ручных ожиданий. Это технические проверки модели, не доказательство преимуществ детектора или планировщика.

| Сценарий | Проверяемое поведение |
|---|---|
| [baseline](baseline/README.md) | Без нарушений; RR завершения [4,2,8,4], LL [4,2,6,6] |
| [crash](crash/README.md) | Один crash в 3; FAILED в 20; RR Detection 17, Recovery 25 |
| [blackout](blackout/README.md) | Краткий перерыв [10,21): SUSPECT в 15, ALIVE в 25 до проверки FAILED |
| [long_blackout](long_blackout/README.md) | Длительный перерыв без crash: ложный FAILED в 8, физическая очередь продолжается, поздние результаты отклоняются |
| [reordered](reordered/README.md) | Старые номера heartbeat не меняют контакт/поколение; adaptive кратко SUSPECT в 10–12 |
| [simultaneous](simultaneous/README.md) | Два FAILED в 10, одна реконфигурация и один dispatch после поступления задачи 7 |
| [no_alive](no_alive/README.md) | Пустой ALIVE с 4, возврат задач в 8; все задачи учтены в FIFO, Recovery отсутствует |
| [unfinished](unfinished/README.md) | Работа остаётся на горизонте; поступление/назначение в 10 исполняется, положительная длительность не исчезает |
| [generated_recovery](generated_recovery/README.md) | Сохранённый контрольный результат генератора: seed 1304, Detection 5, Recovery 17 |

## Запуск и повторное чтение

Из корня проекта после сборки по [README](../README.md):

```sh
./build/final-release/telemetry_sim validate --scenario examples/long_blackout/scenario.json
./build/final-release/telemetry_sim compare --scenario examples/long_blackout/scenario.json --out results/final-long-blackout
./build/final-release/telemetry_sim compare --scenario examples/reordered/scenario.json --out results/final-reordered
./build/final-release/telemetry_sim batch --manifest examples/experiments.json --out results/final-suite
./build/final-release/telemetry_sim metrics --run-dir results/final-suite/runs/000001/fixed_rr
```

[experiments.json](experiments.json) перечисляет все девять сценариев: 36 запусков четырёх вариантов, общий comparison.csv и summary.csv. В серии один технический повтор каждого случая; это не статистическая выборка независимых экспериментов. Корректная незавершённость не останавливает серию. Метки не определяют путь: batch сортирует scenario_id/repeat_id и создаёт runs/000001, ...; соответствие сохраняется в batch_manifest.json.

## Генератор и материализованные входы

[generator.json](generator.json) задаёт работу/задержки из диапазонов и вероятность потерь. [generator_recovery.json](generator_recovery.json) — контроль с постоянной работой 6, нулевой задержкой и q=0, crash узла 1 в 3 и blackout [1,21), чтобы Detection/Recovery считались вручную.

```sh
./build/final-release/telemetry_sim generate --config examples/generator_recovery.json --out traces/final-recovery
./build/final-release/telemetry_sim compare --scenario traces/final-recovery/scenario.json --out results/final-generated
```

Сначала записываются JSON/CSV, точная конфигурация и generation_manifest.json, затем run/compare заново читают сохранённые входы. В generated_recovery лежит небольшой готовый снимок этого контроля; его JSON/CSV должны совпадать с повторной генерацией. Константные распределения делают ручные ожидания независимыми от случайных выборов; для произвольных диапазонов один seed не гарантирует побитовой одинаковости стандартных распределений на разных платформах. Метаданные сохраняют фактическую среду.

Девять групп examples_* проверяют каталог, сценарные последствия во всех вариантах, повторное чтение с идентичным журналом/метриками/CSV, независимый пересчёт и регенерацию сохранённого контроля. Команды не перезаписывают непустой результат без --overwrite. Форматы: [CLI](../docs/cli.md), [входы](../docs/scenario_format.md). Проверки: [самоаудит этапа 13](../docs/stage_13_audit.md).

Порядок подготовки независимых повторов и интерпретация сводки: [experiments.md](../docs/experiments.md). Итоговая передача: [этап 15](../docs/stage_15_audit.md).
