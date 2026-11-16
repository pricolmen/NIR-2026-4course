# GUI КР Этап 4 Проверка и сдача

Дата: 2026-11-16. Четыре этапа завершены. Исходная версия — 38f1b201. [Команды сборки/запуска и расположение данных](gui_handoff.md), [использование](../gui/README.md). DOCX и расчётные исходники не редактировались. Существенных расхождений с фактическим CLI в рассмотренных требованиях §1.3/1.4/2.4/2.5 и таблице 2 не выявлено. Значения состояний журнала нижнего регистра отображаются верхним; GUI не пересчитывает детектор/планирование/метрики.

## Изменения и проверка

Добавлена gui/tests/acceptance_tests.cpp и группа gui_stage4: 6 условий × 4 сочетания fixed/adaptive и RR/LL. В каждом случае запускается реальное MainWindow с настоящим backend, применяется журнал запись за записью и проверяются состояния/попытки/метрики. Отдельно проверены сохранность JSON/CSV и диагностический validate до расчёта. Добавлены снимки четырёх вкладок формы и проверка полной видимости последней строки истории для длинных путей. Высота таблиц формы ограничена; длинные пути перенесены в подсказку, результат однозначно обозначен полным UUID. Новые документы — gui_handoff.md и этот аудит; обновлены README и локальные правила игнорирования генерируемых данных.

Фактически выполнено: **100/100 CTest Release** (96 прежних, включая статистическую группу, и 4 GUI). Qt Test: **26 + 7 + 13 + 26 passed**, числа включают setup/cleanup. Повторены все четыре группы в живых Cocoa-окнах: те же **72 passed**, 0 failed. Это автоматические нажатия Qt Test; ручное прохождение человеком не заявляется. CTest использует offscreen. Непрерывное и пошаговое воспроизведение одного журнала сверены; timing.json содержит фактическое время ×1 для горизонта 8 секунд.

Отдельная Debug-сборка LLVM 23.1.3: **ASan+UBSan, 4/4 группы GUI**, 0 failed. Утечки отключены через detect_leaks=0; UB останавливает проверку. Установленный Qt и системные библиотеки не инструментировались. Release собрана Apple Clang 17, Qt 6.12.0, C++20 с NDEBUG, macOS 26.6.2 arm64, CMake 4.4.4. Компиляторных warning: нет; сообщения offscreen propagateSizeHints и необязательный WrapVulkanHeaders не являются ошибками Widgets. Windows/Linux и упаковка .app не проверены.

## Матрица фактических результатов

D — среднее время от физического отказа до FAILED; R — среднее время от физического отказа до последнего принятого результата возвращённого набора. Единица — мкс. FP — число ложных исключений. Незаполненное значение ниже указано с исходной reason, а не заменено нулём. Каждый сценарий проверен в четырёх режимах; таблица фиксирует все 24 случая.

| Сценарий | Детектор/стратегия | Завершено/незавершено | FP | D | R | Проверка |
|---|---|---:|---:|---|---|---|
| baseline | fixed/rr | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| baseline | fixed/ll | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| baseline | adaptive/rr | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| baseline | adaptive/ll | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| crash | fixed/rr | 4/0 | 0 | 17 | 25 | passed |
| crash | fixed/ll | 4/0 | 0 | 17 | 23 | passed |
| crash | adaptive/rr | 4/0 | 0 | 17 | 25 | passed |
| crash | adaptive/ll | 4/0 | 0 | 17 | 23 | passed |
| blackout | fixed/rr | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| blackout | fixed/ll | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| blackout | adaptive/rr | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| blackout | adaptive/ll | 4/0 | 0 | null: empty_sample | null: empty_sample | passed |
| long_blackout | fixed/rr | 3/0 | 1 | null: empty_sample | null: empty_sample | passed |
| long_blackout | fixed/ll | 3/0 | 1 | null: empty_sample | null: empty_sample | passed |
| long_blackout | adaptive/rr | 3/0 | 1 | null: empty_sample | null: empty_sample | passed |
| long_blackout | adaptive/ll | 3/0 | 1 | null: empty_sample | null: empty_sample | passed |
| no_alive | fixed/rr | 0/4 | 0 | 8 | null: empty_sample | passed |
| no_alive | fixed/ll | 0/4 | 0 | 8 | null: empty_sample | passed |
| no_alive | adaptive/rr | 0/4 | 0 | 8 | null: empty_sample | passed |
| no_alive | adaptive/ll | 0/4 | 0 | 8 | null: empty_sample | passed |
| invalid | fixed/rr | — | — | — | — | passed |
| invalid | fixed/ll | — | — | — | — | passed |
| invalid | adaptive/rr | — | — | — | — | passed |
| invalid | adaptive/ll | — | — | — | — | passed |

Ожидания и факты по условиям: baseline — все ALIVE, по одному принятому результату каждой задачи; crash — физический отказ отдельно от FAILED, возврат/новый номер попытки; blackout — исправный SUSPECT → ALIVE без FAILED; long_blackout — ложный FAILED, отклонён результат отозванной попытки и принят результат другой; no_alive — все физически отказали, возвращённые задачи незавершены, по каждому узлу recovery_incomplete; invalid — неизвестный узел, validate отклонён, run не вызывался, причина видна. Настройки и отдельные переходы для каждого варианта сохранены в acceptance.json, полные входы — в result/input. В invalid изменён только node_id отказа в копии crash и сохранена исходная трасса.

Crash fixed/RR отдельно проверен группой replay: физический отказ 3 мкс, SUSPECT 10, FAILED 20, задачи 1/3 получают попытку 2 на узле 2, последний принятый результат возвращённого набора — 28. Итог D=17, R=25, 4 завершённых/0 незавершённых. При LL на том же примере R=23; это свойство выбранных входов, не универсальное преимущество. При no_alive среднее R null/empty_sample, а по узлам null/recovery_incomplete; оба уровня отображают исходные причины.

## Контрольные данные

Машинная сводка с полными командами, SHA исполняемых файлов и исходников: results/kr-gui-stage4/final_checks.json. Логи release.log, sanitize.log, acceptance.log, live-runner.log, live-replay.log, live-editor.log, preservation.log. Конкретные каталоги текущего запуска:

- live-runner: results/kr-gui-stage4/live-runner/check-b7e1b3f6-9bf6-4774-ab50-9652deb22bb7/
- live-replay: results/kr-gui-stage4/live-replay/check-eae3794b-c558-4ef3-9579-e981986d69b2/
- live-editor: results/kr-gui-stage4/live-editor/check-12c3815a-4c54-4b28-8dbd-75ab6824d805/
- acceptance: results/kr-gui-stage4/acceptance/check-d86abc3a-8dac-434e-93bc-2619621901f7/

В acceptance/check-UUID находятся acceptance.json/.csv, 20 полных результатов расчётов и 4 отклонённых входа, снимки окон шести условий. В live-editor/check-UUID — форма, редактор, generation и копии, повторно открытые/запущенные пары. В live-replay/check-UUID — снимки пошагового crash и timing.json. Эти данные пригодны для описания реализации и проверок в главах 3–4; текст КР оставлен отдельной задаче.

## Сохранность и ограничения

Все 166087 прежних файлов результатов сохранены по SHA-256. Расчётные исходники и исследовательский lock сохранены, повторный verify_experiments завершился complete: results/kr-gui-stage4-frozen-check. Прежний бинарник без Qt повторно прошёл validate crash; otool не показывает Qt. Корневые CMake/CLI/алгоритмы/пакет экспериментов не менялись. Результаты GUI игнорируются отдельными префиксами, прежний results не скрыт.

Открытый журнал хранится в памяти целиком, история сеанса не импортируется после перезапуска. Во время выполнения требуется Qt. Публикация CSV и JSON не образует общей транзакции при аварии питания. Эти ограничения описаны в инструкции, блокирующих дефектов текущих четырёх этапов не выявлено. Самопроверка основным агентом; новый субагент не запускался. Коммит не создан. Рекомендуемое название этапа 4: test(gui): verify acceptance scenarios and finalize application. После этапа остановка.
