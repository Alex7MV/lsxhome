# Промпт для Linux-машины: починить Debug-сборку движка (logestix)

Скопируй текст ниже целиком в сессию агента/разработчика на Linux-компьютере,
где есть репозиторий logestix и CUDA 13.4.

---

## Задача

В репозитории **logestix** (ветка `main`, состояние не старше `879e422`) почини
**Debug-конфигурацию**: она не компилируется из-за `--expt-relaxed-constexpr`.
Release собирается, его ломать нельзя.

## Что известно (измерено на Windows: nvcc V13.4.92 / LLVM 23, MSVC 14.51 / VS 2026, `/MDd`)

Компиляция `libs/lsxcommon/src/gemma_dense_inference.cu`,
`xing_moe_kernels.cu` и `xing_moe_inference.cu` падает в Debug **при обоих
положениях флага**:

1. `--expt-relaxed-constexpr` **включён** →
   ```
   <unnamed>: parse Invalid instruction with no BB (Producer: 'LLVM23.0.0' Reader: 'LLVM 23.0.0')
   ```
   (NVVM abort, без указания строки).

2. Флаг **выключен** (текущий guard
   `$<$<NOT:$<CONFIG:Debug>>:--expt-relaxed-constexpr>` в
   `libs/lsxcommon/CMakeLists.txt`) → обычные ошибки компиляции:
   ```
   gemma_dense_inference.cu(312): error : calling a constexpr __host__ function("Offset")
     from a __global__ function("gemma_dense_attn_kernel") is not allowed. The experimental
     flag '--expt-relaxed-constexpr' can be used to allow this ...
   ```
   Те же ошибки в `xing_moe_kernels.cu` (`xing_attn_kernel`, `xing_attn_split_kernel`)
   и `xing_moe_inference.cu` (`xing_write_cache_kernel`).

## Корень проблемы

`libs/lsxcommon/include/lsxcommon/lsx_kv_cache.h` объявляет методы геометрии
KV-кэша как `constexpr` **без** `__host__ __device__` и прямо опирается на флаг
(комментарий на строках 73–75):

```cpp
// constexpr, not __host__ __device__: --expt-relaxed-constexpr is already on
// (libs/lsxcommon/CMakeLists.txt:169), so host and device share ONE function
constexpr int64_t LayerStride() const { ... }
constexpr int64_t Elements() const { ... }
constexpr int64_t Offset(int64_t layer, int64_t head, int64_t pos, int64_t dim = 0) const { ... }
```

Пока флаг включён — устройство их видит. Стоит его убрать в Debug (что и делает
guard) — вызовы из `__global__` становятся недопустимыми. А включать флаг в
Debug нельзя из-за NVVM-аборта. Замкнутый круг; разрывать его нужно в коде.

## Что сделать

1. **Сделать device-видимыми** `constexpr`-методы, которые вызываются из
   `__global__`/`__device__`:
   - `lsx_kv_cache.h`: `LayerStride`, `Elements`, `Offset` — пометить
     `__host__ __device__` (или `_LIBCUDACXX_HOST_DEVICE`, если так принято в
     репозитории), сохранив `constexpr`.
   - Найти и исправить **все остальные** такие же места (не только KV-геометрию):
     ищи `constexpr`-функции без `__host__ __device__`, вызываемые из `.cu`.
     Начни с ошибок компилятора — они перечисляют конкретные функции/строки.
2. Обновить комментарий в `lsx_kv_cache.h` (73–75): он утверждает, что флаг
   «already on» — после фикса это не так, и комментарий станет ложным.
3. Решить, нужен ли после этого guard в `libs/lsxcommon/CMakeLists.txt`:
   если Debug собирается и без флага — оставь guard (он защищает от NVVM-бага);
   если где-то флаг всё ещё нужен — зафиксируй это комментарием и задачей.
4. Ничего не переименовывать и не менять публичные сигнатуры: только атрибуты
   и комментарии.

## Проверка (обязательные критерии приёмки)

На Linux (gcc/clang + nvcc 13.4):

```bash
cmake --preset linux-debug   && cmake --build --preset linux-debug
ctest --preset linux-debug                       # label fast, должен быть зелёным
cmake --preset linux-release && cmake --build --preset linux-release
```

Если в окружении есть Windows/MSVC — проверить и там (`/MDd`):

```powershell
cmake --preset windows-debug   # при наличии пресета
cmake --build --preset windows-debug
```

Дополнительно, на реальных весах:

- `gemma_dense` (gemma-4-31B-it) и `xing` (Xing4.0-29B-A4B): `--selfcheck`
  и FSI/CODE/DET должны пройти;
- **Release обязан остаться bit-exact**: `gen_tokens` и goldens не должны
  измениться. `-fmad=false` и прочие численные флаги не трогать.

## Ограничения

- Не «упрощать» алгоритмы, не менять формулы и не трогать `-fmad=false`.
- Не переносить вычисления из device в host ради обхода.
- Если NVVM-аборт всё же воспроизводится на Linux при включённом флаге —
  приложи `-Xcicc -print-after-all` или `-Xptxas -v` и запиши вывод: нужен
  минимальный воспроизводитель для отчёта в NVIDIA. Материалы по этому багу
  (триггер-матрица, среда) уже есть в lsxhome:
  `docs/repro/engine-encoding-audit/nvcc-13.4-invalid-instruction.md`.
- Отдельно (если будет время): проверить, воспроизводится ли на исходном
  движке симптом «первый вопрос работает, второй падает» для gemma — он был
  исправлен коммитом `a077d53` («decode to text the tokenizer can encode
  again»); прогон нужен, чтобы подтвердить фикс на Linux/реальных весах.

## Формат отчёта

1. Дифф (минимальный).
2. Вывод обеих сборок (Debug и Release) и `ctest`.
3. Результаты selfcheck на gemma_dense и xing до/после (должны совпасть).
4. Если остались места, требующие флага — список с обоснованием.
