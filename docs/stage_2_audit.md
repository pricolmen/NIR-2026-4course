# Этап 2: время, базовые типы и самоаудит

Дата: 19 октября 2026, Europe/Moscow. Этап 3 не начат.

## Результат

Созданы отдельные `SimTime`/`SimDuration` на int64, положительные типизированные `NodeId`, `TaskId`, `HeartbeatSeq`, `AttemptNo` на uint64, безопасные операции времени и инкремента счётчиков. Отсутствующие моменты/сроки представлены `optional`; ноль остаётся действительным временем.

Реализованы ceil-перевод вычисленной длительности, формула обработки с минимумом одного тика и отдельное округление математических порогов с обязательным тиком между стадиями. Математические пороги сохраняются для будущей диагностики. Ни детекторы, ни очереди задач, ни движок на этом этапе не реализуются.

Реальные файлы:

- [time.hpp](../include/telemetry/common/time.hpp), [time.cpp](../src/common/time.cpp): время, арифметика, горизонт, округление и обработка.
- [ids.hpp](../include/telemetry/common/ids.hpp), [checked_integer.hpp](../include/telemetry/common/checked_integer.hpp): разные типы ID и проверки до преобразования/инкремента.
- [common_tests.cpp](../tests/common_tests.cpp): три группы содержательных проверок.
- [time.md](time.md): API, ошибки, единицы, точность и численные решения главы 3.

`telemetry_common` стала компилируемой библиотекой. Core и engine по-прежнему зависят только от common; common не зависит от JSON/IO/engine/core. В CMake добавлены три теста, а метаданные CLI обновлены до этапа 2. В `.gitignore` добавлены только исключения для новых документов. Исходный план, DOCX, отчёты этапов 0 и 1 сохранены.

## Проверки

| Группа | Что проверено |
|---|---|
| `common_time` | 0, INT64_MAX, целое 9007199254740993, границы signed/unsigned, отрицательные значения, точные операции, переполнение, обратная разность, граница горизонта, запрос за горизонт без сложения с переполнением, optional и единицы |
| `common_numeric` | Целый и дробный ceil; соседнее вещественное значение выше целого без epsilon; точная граница 2^63 до cast; обработка короче тика; NaN/inf/неположительные входы; переполнение и underflow; пороги 10.1/20.2 → 11/21, близкие 10.1/10.11 → 11/12, сохранение исходных порогов |
| `common_ids` | Положительность и порядок, разные типы, полный диапазон uint64, первый/следующий номер, переполнение попытки/heartbeat/счётчика, отрицательный счётчик |
| Проверки компиляции | Нет конструктора времени/ID из вещественного типа и bool; нет смешения ID; нет неявного превращения отметки в интервал; корректный constexpr |
| Регрессия этапа 1 | 8 прежних проверок метаданных, JSON и CLI, согласованных с текущим этапом |

Проверки поведения используют явные условия и исключения, не `assert`; в Release они активны. `static_assert` относится только к контрактам типов/constexpr. Формат файлов и отклонение дробного JSON-поля относятся к этапу 9: сейчас дробный конструктор запрещён компилятором, загрузчик не имитируется.

## Фактически выполненные сборки

Платформа: macOS 26.6.2 / Darwin 25.6.0, arm64. CMake 3.31.6, Ninja 1.11.1; Apple Clang 17.0.0.17000603 для Debug/Release, Homebrew LLVM 23.1.3 для ASan+UBSan. JSON 3.12.0 переиспользован из уже загруженного дерева.

| Каталог | Результат |
|---|---|
| `build/stage2-debug` | Конфигурация/сборка успешны, CTest 11/11 |
| `build/stage2-release` | Конфигурация/сборка успешны, CTest 11/11, проверки активны при NDEBUG |
| `build/stage2-sanitize` | Конфигурация/сборка успешны, CTest 11/11, диагностик ASan/UBSan на этих тестах нет |

До записи в перемещённый репозиторий те же изменения проверены в `/private/tmp/nir-stage2-work`, сборки `/private/tmp/nir-stage2-{debug,release,sanitize}`: также по 11/11. Запись по новому пути потребовала разрешения песочницы, так как её исходный writable root всё ещё содержал `NIR:KR`. Перед переносом проверены контрольные суммы исходных файлов, чтобы не затереть изменения пользователя.

Команды из корня проекта (при проверке CMake/CTest вызывались абсолютными путями из `/private/tmp/nir-stage1-tools/cmake/data/bin`):

```sh
export PATH="/private/tmp/nir-stage1-tools/cmake/data/bin:/private/tmp/nir-stage1-ninja/bin:$PATH"
cmake -G Ninja -S . -B build/stage2-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/stage2-debug --parallel
ctest --test-dir build/stage2-debug --output-on-failure

cmake -G Ninja -S . -B build/stage2-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/stage2-release --parallel
ctest --test-dir build/stage2-release --output-on-failure

cmake -G Ninja -S . -B build/stage2-sanitize -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/stage2-sanitize --parallel
ctest --test-dir build/stage2-sanitize --output-on-failure
```

PATH-команда выше дана для повторения; при конфигурации также явно передан путь `-DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja`. Для чистого репозитория с сетевой загрузкой опцию FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON можно опустить.

## Самоаудит

Проверены соответствие разделу 5 плана и критерию этапа 2, все преобразования до сужения, операции до переполнения, корректность ветки за горизонтом, отсутствие epsilon в ceil/времени, сохранение исходных порогов и границы сборочных зависимостей. Дополнительно проверяются compile_commands.json, локальные Markdown-ссылки, форматирование и `git diff --check`.

В ходе самоаудита до переноса исправлены два граничных случая: инкремент счётчика теперь проверяет отрицательный аргумент до unsigned-cast; проверка наибольшего безопасного вещественного значения учитывает различие 53-битного и более точного long double. Обе ситуации включены в тесты. На текущей платформе long double имеет 53 бита мантиссы; ветка тестов для мантиссы >=64 предусмотрена, но на другой платформе не запускалась.

Смысл Load/Recovery, детекторов и фаз не менялся. Содержательных противоречий с планом не найдено. Это самоаудит основным агентом без субагента, согласно принятому порядку до этапа 10; независимый аудит и уровень High не заявляются.

Ограничения: Linux/GCC/x86 и иная точность long double не проверены; чтение сценариев, статистика окна, политика порогов, реестр задач и симулятор ещё не реализованы. На этом этапе нет экспериментальных результатов или утверждений о преимуществе алгоритма. Коммит и staging не выполнялись. После завершения этапа работа останавливается.
