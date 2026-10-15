# Промпт для правки движка (logestix): CMAKE_PROJECT_INCLUDE сбрасывает политики CMake

> **Статус: решено в движке — `78da023`** («build: stop lowering nested
> projects' policy level (breaks VS configure)»). Задача ниже выполнена
> апстримом именно так, как предложено (Вариант A): хак `CMAKE_PROJECT_INCLUDE`
> заменён на `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)`, скилл обновлён. Пин
> lsxhome переведён на `78da023`, обходные `CMAKE_POLICY_DEFAULT_CMP0076/0091`
> из потребителя удалены; Release и Debug собираются, 4/4 тестов зелёные.
> Текст сохранён как запись о дефекте и о проверенном A/B.

Скопируй текст ниже целиком в сессию агента/разработчика в репозитории
**logestix** (ветка `main`, состояние не старше `2212086`). Машина может быть
любой — симптом воспроизводится и на Windows/MSVC, и (ожидаемо) на Linux; там,
где сборка идёт, нужно проверить обе платформы.

---

## Задача

Починить **конфигурацию CMake** движка: вложенные проекты Arrow 26.0.0
(`boost`, `uriparser`) получают сброшенный уровень политик и падают на
configure. Release/Debug собирать нельзя ломать; численные результаты не
затрагиваются (это чисто configure-time правка).

## Что известно (измерено: Windows, MSVC 14.51 / VS 18 2026, CMake 4.3, движок `2212086`, Arrow 26.0.0)

В корневом `CMakeLists.txt` движка (строки ~154–162) есть хак совместимости с
CMake 4.x:

```cmake
# CMAKE 4.x COMPATIBILITY HACK (for xsimd/Arrow):
# Ensures CMake 4.x does not block dependencies that require a policy below 3.5
# We use CMAKE_PROJECT_INCLUDE so the file applies to the project() of any nested
# ExternalProject (including xsimd_ep), not just the current project.
set(CMAKE_PROJECT_INCLUDE "${CMAKE_BINARY_DIR}/cmake_policy.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/cmake_policy.cmake"
    "cmake_minimum_required(VERSION 3.5)\n"
    "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n"
)
```

С этим состоянием конфигурация **завершается ошибкой exit=1**, первой падает
`uriparser`:

```
CMake Error at build/.../_deps/uriparser-src/CMakeLists.txt:113 (message):
  Policy CMP0091 behavior is '' rather than 'NEW', mis-compilation detected
```

(на другом порядке обхода первым падает Boost: `Cannot find source file` на
относительных `extra/*.natvis` в INTERFACE-цели.)

## Корень проблемы

`CMAKE_PROJECT_INCLUDE` подключает этот файл **в конце каждого вложенного
`project()`** — то есть в области видимости Arrow, boost, uriparser и т.д.
Вызов `cmake_minimum_required(VERSION 3.5)` внутри файла **понижает уровень
политик** этой области до 3.5. Все политики, введённые после 3.5, становятся
`UNSET`, то есть ведут себя как `OLD`:

- `CMP0076` (3.13) — относительные пути в `target_sources()`; INTERFACE-цель
  Boost 1.92 перечисляет `extra/*.natvis` относительно, в OLD-поведении файл не
  находится → `Cannot find source file`.
- `CMP0091` (3.15) — выбор MSVC runtime через `CMAKE_MSVC_RUNTIME_LIBRARY`;
  uriparser 1.0.2 явно проверяет, что политика `NEW`, и аварийно завершает
  configure.

При этом строка `cmake_minimum_required(VERSION 3.5)` **не нужна** для исходной
цели хака: CMake 4.x разрешает вложенным проектам с `cmake_minimum_required`
ниже 3.5 именно через переменную `CMAKE_POLICY_VERSION_MINIMUM` (её же ставит
и хак, и пресеты движка: `"CMAKE_POLICY_VERSION_MINIMUM": "3.5"`). Лишним
является именно вызов `cmake_minimum_required`, потому что он меняет уровень
политик.

## Что сделать

Понижать уровень политик во вложенных проектах перестать. Два равнозначных
варианта (проверены A/B на измеренной выше среде — оба дают exit=0 на чистом
потребителе без обходных `CMAKE_POLICY_DEFAULT_*`):

**Вариант A (предпочтительный, минимальный)** — убрать строку
`cmake_minimum_required` из записываемого файла, оставить только переменную:

```cmake
set(CMAKE_PROJECT_INCLUDE "${CMAKE_BINARY_DIR}/cmake_policy.cmake")
file(WRITE "${CMAKE_BINARY_DIR}/cmake_policy.cmake"
    "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n"
)
```

**Вариант B** — оставить установку уровня политик, но не откатывать её ниже
современной:

```cmake
file(WRITE "${CMAKE_BINARY_DIR}/cmake_policy.cmake"
    "cmake_minimum_required(VERSION 3.5...3.31)\n"
    "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n"
)
```

Требования к любой из правок:

1. Обновить комментарий над хаком: он должен объяснять, что уровень политик
   вложенных проектов трогать нельзя, и почему достаточно
   `CMAKE_POLICY_VERSION_MINIMUM`.
2. Обновить/починить редакцию скилла `cuda-cmake-build-gotchas`
   (`.opencode/skills/cuda-cmake-build-gotchas/SKILL.md`) — там хак
   `CMAKE_POLICY_VERSION_MINIMUM` для xsimd описан; после правки описание не
   должно расходиться с кодом.
3. Если в движке (или в его тестах/suite-guard) есть подходящее место — добавить
   регресс-проверку на то, что configure проходит при Arrow 26 без внешних
   `CMAKE_POLICY_DEFAULT_*`. Соответствие текущей схеме guard-тестов движка
   (`test_*_guard.cpp`, `tools/test_router.cmake`) — на усмотрение; если
   подходящего класса тестов нет, достаточно зафиксировать факт в комментарии и
   в `docs/OPEN_TASKS.md`.

## Проверка (обязательные критерии приёмки)

Ключевой тест — **движок без внешних обходных переменных**. Убедись, что в кэше
нет `CMAKE_POLICY_DEFAULT_CMP0076`/`CMAKE_POLICY_DEFAULT_CMP0091` (то есть
потребитель их не подставил), и что конфигурация проходит с чистого кэша:

```bash
# Linux
rm -rf build/linux-release
cmake --preset linux-release && cmake --build --preset linux-release
ctest  --preset linux-release          # label fast — зелёный

cmake --preset linux-debug   && cmake --build --preset linux-debug
```

```powershell
# Windows / MSVC (если есть)
Remove-Item -Recurse -Force build\windows-release
cmake --preset windows-release        # должен пройти configure (exit 0)
cmake --build --preset windows-release
```

Дополнительно:

- `Arrow 26` должен конфигурироваться без ошибок `uriparser`/Boost (см. выше).
- Проверить, что `xsimd` (та самая исходная цель хака) не сломался: конфигурация
  не должна падать на нём после удаления `cmake_minimum_required`.
- **Release обязан остаться bit-exact** (правка configure-time, кода не
  касается): `gen_tokens`/goldens не меняются.

Для воспроизведения до/после удобно смотреть только configure, не собирая всё:

```bash
cmake --preset linux-release 2>&1 | grep -E "Generating done|CMake Error at"
```

## Ограничения

- Не «лечить» симптом правкой Arrow/boost/uriparser — источник в корневом
  `CMakeLists.txt` движка.
- Не менять численные флаги, `lsx_tune`/LTO-гейтинг и т.п.
- Не удалять `CMAKE_POLICY_VERSION_MINIMUM 3.5` — именно он удерживает
  совместимость с зависимостями, требующими CMake < 3.5 (xsimd).
- Никаких параллельных агентов; все правки — напрямую в репозитории.

## Формат отчёта

1. Дифф (минимальный; выбранный вариант A или B).
2. Вывод configure (Linux, и Windows при наличии) с чистого кэша — без
   `CMAKE_POLICY_DEFAULT_*` — и вывод `ctest` (label fast).
3. Подтверждение, что `xsimd` конфигурируется.
4. Итоговый текст комментария и обновлённый фрагмент скилла
   `cuda-cmake-build-gotchas`.

## Контекст потребителя (lsxhome)

lsxhome на своей стороне временно ставит обходные переменные
(`CMAKE_POLICY_DEFAULT_CMP0076/CMP0091 = NEW`, cache+FORCE) в своём
`CMakeLists.txt` и держит пин движка на `2212086`. После этой правки движка
обходные строки в lsxhome можно удалить — сообщи об этом в отчёте, чтобы там их
снять отдельным коммитом.
