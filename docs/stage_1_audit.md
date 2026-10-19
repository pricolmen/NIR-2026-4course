# Этап 1: сборка, проверки и самоаудит

Дата: 19 октября 2026, Europe/Moscow. Этап 2 не начат.

Актуализация: ограничение ASan устранено отдельной сборкой Homebrew LLVM; см. дополнение в конце. Первоначальные неуспешные результаты ниже сохранены как история проверки.

## Реализовано

- [CMakeLists.txt](../CMakeLists.txt): CMake >= 3.20, C++20 без расширений, отдельные цели common/core/engine/simulation/IO/CLI/теста, CTest и конфигурации Debug/Release.
- [ProjectOptions.cmake](../cmake/ProjectOptions.cmake): предупреждения только для собственных целей; независимые опции ASan/UBSan с проверкой компиляции и линковки, явной ошибкой при неподдерживаемом запросе; UBSan без продолжения после ошибки.
- [Dependencies.cmake](../cmake/Dependencies.cmake): nlohmann/json 3.12.0, фиксированный URL релиза и SHA-256; установленный пакет принимается через `find_package(... EXACT CONFIG REQUIRED)`. Поддержана стандартная офлайн-подстановка FetchContent.
- [CLI](../app/main.cpp) и [метаданные](../src/io/build_info.cpp): `--help`, `--version`, `--build-info`; неподдерживаемые команды дают код 2, исключения/ошибка вывода — код 1. Моделирование не имитируется: метаданные содержат `simulation_available: false`.
- [Содержательная проверка сборки](../tests/build_smoke.cpp): реальные проверки метаданных, версии JSON, сохранения целых INT64_MAX и 9007199254740993 без потери точности, различения дробного типа, round-trip и отклонения синтаксически неверного JSON.
- [Проверки CLI](../tests/check_cli.cmake): проверяются конкретные коды завершения и вывод, а не только «любой ненулевой код». Введены тайм-ауты тестов.
- [README](../README.md): команды сборки, текущие возможности, параметры, сборочные границы и ограничения. [Контракт](model_contract.md) обновлён только в части текущего состояния и реальных путей этапа 1.

Core, engine и simulation пока INTERFACE-цели: алгоритмические исходники относятся к последующим этапам. Пустые алгоритмы и фиктивные `.cpp` не добавлены. Это готовая основа сборки этапа 1, а не готовый экспериментальный стенд.

## Среда и фактические команды

Проверенная платформа: macOS / Darwin 25.6.0, arm64; Apple Clang 17.0.0.17000603, CMake 3.31.6, Ninja 1.11.1. CMake и Ninja изначально отсутствовали; они загружены с разрешением во временные каталоги, без системной установки. Python-пакет Ninja имеет версию 1.11.1.4.

Для повторения команд ниже в текущем окружении добавьте временные инструменты в PATH:

```sh
export PATH="/private/tmp/nir-stage1-tools/cmake/data/bin:/private/tmp/nir-stage1-ninja/bin:$PATH"
```

Это вспомогательная команда для пользователя; при проверке использовались абсолютные пути к CMake/CTest и `-DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja`. Ниже те же фактически выполненные действия записаны сокращённо. Временные инструменты не являются постоянной установкой и могут исчезнуть при очистке `/private/tmp`.

```sh
cmake -G Ninja -S . -B build/debug -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure

cmake -G Ninja -S . -B build/release -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure

cmake -G Ninja -S . -B build/ubsan -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/ubsan --parallel
ctest --test-dir build/ubsan --output-on-failure

./build/release/telemetry_sim --help
./build/release/telemetry_sim --version
./build/release/telemetry_sim --build-info
```

Debug загрузил релиз через FetchContent с проверкой закреплённого хеша. Дополнительно архив был загружен во временный каталог и проверен `shasum -a 256`: `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`. Остальные конфигурации использовали уже полученные исходники, без повторных сетевых загрузок. Запуск моделирования и генерация метрик не выполнялись.

## Результаты

| Проверка | Фактический результат |
|---|---|
| Debug: конфигурация, сборка, CTest | Успешно, 8/8 тестов |
| Release: конфигурация, сборка, CTest | Успешно, 8/8 тестов; в compile_commands.json подтверждён `-DNDEBUG`, проверки остаются активными |
| UBSan: конфигурация, сборка, CTest | Успешно, 8/8 тестов; диагностик UBSan нет |
| ASan + UBSan: конфигурация и сборка `build/sanitize` | Успешно; фактические флаги присутствуют при компиляции и линковке |
| ASan + UBSan: исполнение | Неуспешно: smoke test зависает, ограниченный прогон завершился тайм-аутом 15 с; повтор вне песочницы — тайм-аутом 20 с. Проверка `--help` вне песочницы также не завершилась за 10 с. Полные зависающие прогоны прерваны |
| Отдельная диагностика ASan | Копия CLI вне пути с двоеточием тоже зависает. Независимая программа `int main() { return 0; }`, собранная Apple Clang только с `-fsanitize=address`, не завершилась за 5 с. Verbose-лог отдельного запуска CLI дошёл до `AddressSanitizer: libc interceptors initialized`. Корневая причина не установлена |
| Установленный JSON и выключенные тесты | Из загруженной зависимости создан локальный установленный пакет в `build/json-install`; `build/system-json` с `TELEMETRY_USE_SYSTEM_JSON=ON`, `BUILD_TESTING=OFF` собран, метаданные выведены, CTest показывает 0 тестов |
| Флаги собственного кода | Проверены compile_commands.json: `-std=c++20`, набор предупреждений; `-isystem` только для файлов, использующих JSON; JSON include-путь не протёк в CLI |
| Путь с двоеточием | Ninja работает на исходном пути; попытка Makefiles отклоняется кодом 1 с понятной рекомендацией |
| Границы зависимостей | Конфигурация требует пустой список модульных зависимостей common и ровно common у core/engine. Эти цели не получают IO/JSON/сведения среды |
| Документация и состав изменений | `git diff --check` без замечаний; Python-проверка 14 собственных файлов и 15 локальных Markdown-ссылок прошла, незакрытых блоков кода и конфликтных маркеров нет; `git status` не показывает зависимости и результаты сборки |

Команды дополнительной проверки установленного пакета:

```sh
cmake -G Ninja -S build/debug/_deps/nlohmann_json-src -B build/json-package -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DJSON_BuildTests=OFF -DJSON_Install=ON -DCMAKE_INSTALL_PREFIX="$PWD/build/json-install"
cmake --install build/json-package
cmake -G Ninja -S . -B build/system-json -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTELEMETRY_USE_SYSTEM_JSON=ON -DCMAKE_PREFIX_PATH="$PWD/build/json-install"
cmake --build build/system-json --parallel
./build/system-json/telemetry_sim --build-info
ctest --test-dir build/system-json -N
```

Сетевые попытки из песочницы сначала не прошли; загрузки выполнены после разрешения. Первоначальная попытка Makefiles обнаружила ошибку преобразования пути с двоеточием внутри ExternalProject. В результате добавлены Ninja-рекомендация, поддержка CMP0168 при CMake >= 3.30 и ранняя диагностика неподходящей комбинации пути/генератора/версии. Для обычного пути минимум проекта остаётся CMake 3.20, но именно версия 3.20 не проверялась.

## Вывод самоаудита и ограничения

Критерии этапа 1 выполнены: проект конфигурируется и собирается, минимальные содержательные проверки проходят в Debug и Release, версия зависимости фиксирована, сборочные границы заданы, обе опции санитайзеров реализованы и проверены на компиляцию/линковку. Исполнение ASan в текущей среде остаётся неподтверждённым из-за зависания, воспроизводимого также вне кода проекта. Успешный ASan-прогон не заявляется; наличие этого ограничения не подменяется успехом конфигурации.

Это самоаудит основного агента, без субагента, по согласованному порядку до этапа 10. Модель/усилия не объявляются подтверждёнными настройками запуска. Проверялись собственный код, CMake-граф, параметры компиляции, результаты CTest и документация; полноценные алгоритмические границы заголовков будут проверяться по мере появления модулей.

Linux, GCC, Windows, CMake 3.20, отдельный успешный ASan-прогон и поведение будущего симулятора не проверены. Исходный план, DOCX и материалы этапа 0 сохранены; коммит и staging не выполнялись. Каталоги сборки/зависимостей/результатов игнорируются Git, документация этапа 1 включена точечными исключениями.

Следующий этап после нового указания пользователя — время и базовые типы. На этапе 1 они не реализуются.


## Дополнение: успешный ASan-прогон после переезда

19 октября 2026 пользователь переместил проект в `/Users/mac/MGTU/NIR-KR/NIR-4course/NIR-code` и поручил завершить ASan-проверку. Работа осталась в рамках этапа 1.

Диагностика установила наличие ASan runtime в Command Line Tools 26.2.0.0.1.1764812424. Apple Clang 17.0.0 (`clang-1700.6.3.2`) по-прежнему зависает до наблюдаемого входа в `main()`: это воспроизведено на независимой минимальной программе и свежей сборке `/private/tmp/nir-asan-newpath` без двоеточия в путях. Та же минимальная программа без ASan завершилась с кодом 0. Изменение ASAN_OPTIONS не устранило зависание.

[Apple описывает зависания санитайзеров со старыми инструментами](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_4-release-notes). [В LLVM зарегистрировано совпадающее воспроизведение и успешная проверка с новым LLVM](https://github.com/llvm/llvm-project/issues/200447). Локальное сравнение согласуется с этой проблемой toolchain; собственный backtrace внутреннего deadlock не снимался.

С разрешения пользователя установлен Homebrew LLVM 23.1.3 (формула 23.1.3_2) вместе с его runtime. Первая загрузка оборвалась по сети. Повторная загрузка официального Homebrew bottle выполнена частями, общий SHA-256 проверен перед передачей в кеш Homebrew: `100c1b67a834daa56b5ebe88cedef1b06caf085613f95c04b4b5a1f318315688`. Внешние диагностические программы и загрузчик находятся в `/private/tmp`; они не добавлены в проект.

Проверки новой пары компилятора/runtime:

- Минимальная программа с ASan вошла в `main()` и завершилась с кодом 0.
- Независимая программа с намеренным выходом за границу heap завершилась с ненулевым кодом и сообщением `AddressSanitizer: heap-buffer-overflow`. Это подтверждает работу проверки памяти, а не только успешный старт runtime.
- Проект заново сконфигурирован в `build/asan-llvm` с `CMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++`, Debug, BUILD_TESTING=ON и обеими опциями санитайзеров ON. Конфигурация и сборка успешны, CTest: **8/8**, диагностик ASan/UBSan на штатных тестах нет.
- Метаданные CLI и параметры компиляции проверены для новой сборки. Старые каталоги сборки с прежними абсолютными путями не переиспользовались.

Для воспроизведения из корня проекта (после настройки PATH для CMake/Ninja выше):

```sh
cmake -G Ninja -S . -B build/asan-llvm -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_MAKE_PROGRAM=/private/tmp/nir-stage1-ninja/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DTELEMETRY_ENABLE_ASAN=ON -DTELEMETRY_ENABLE_UBSAN=ON -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:PATH="$PWD/build/debug/_deps/nlohmann_json-src"
cmake --build build/asan-llvm --parallel
ctest --test-dir build/asan-llvm --output-on-failure
```

При проверке использована уже загруженная JSON 3.12.0; для чистой загрузки можно опустить FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON, как в README. Вызовы выполнялись абсолютными путями к CMake/CTest. Старый runtime Apple Clang остался воспроизводимо проблемным; успешный результат относится к новой LLVM-сборке. Это не проверка утечек отдельным LeakSanitizer и не итоговая проверка ещё не реализованных алгоритмов.

Обновлены README, этот отчёт и формулировка о путях в контракте; алгоритмический код не менялся. Этап 2 не начат. Самоаудит выполнен основным агентом без субагента.
