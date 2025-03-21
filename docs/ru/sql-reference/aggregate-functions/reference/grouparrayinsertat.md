---
slug: /ru/sql-reference/aggregate-functions/reference/grouparrayinsertat
sidebar_position: 112
---

# groupArrayInsertAt {#grouparrayinsertat}

Вставляет значение в заданную позицию массива.

**Синтаксис**

```sql
groupArrayInsertAt(default_x, size)(x, pos)
```

Если запрос вставляет вставляется несколько значений в одну и ту же позицию, то функция ведет себя следующим образом:

-   Если запрос выполняется в одном потоке, то используется первое из вставляемых значений.
-   Если запрос выполняется в нескольких потоках, то в результирующем массиве может оказаться любое из вставляемых значений.

**Аргументы**

-   `x` — значение, которое будет вставлено. [Выражение](../../syntax.md#syntax-expressions), возвращающее значение одного из [поддерживаемых типов данных](../../../sql-reference/data-types/index.md#data_types).
-   `pos` — позиция, в которую вставляется заданный элемент `x`. Нумерация индексов в массиве начинается с нуля. [UInt32](/sql-reference/data-types/int-uint#integer-ranges).
-   `default_x` — значение по умолчанию для подстановки на пустые позиции. Опциональный параметр. [Выражение](../../syntax.md#syntax-expressions), возвращающее значение с типом параметра `x`. Если `default_x` не определен, используются [значения по умолчанию](/sql-reference/statements/create/table).
-   `size` — длина результирующего массива. Опциональный параметр. При использовании этого параметра должно быть указано значение по умолчанию `default_x`. [UInt32](/sql-reference/data-types/int-uint#integer-ranges).

**Возвращаемое значение**

-   Массив со вставленными значениями.

Тип: [Array](/sql-reference/data-types/array).

**Примеры**

Запрос:

```sql
SELECT groupArrayInsertAt(toString(number), number * 2) FROM numbers(5);
```

Результат:

```text
┌─groupArrayInsertAt(toString(number), multiply(number, 2))─┐
│ ['0','','1','','2','','3','','4']                         │
└───────────────────────────────────────────────────────────┘
```

Запрос:

```sql
SELECT groupArrayInsertAt('-')(toString(number), number * 2) FROM numbers(5);
```

Результат:

```text
┌─groupArrayInsertAt('-')(toString(number), multiply(number, 2))─┐
│ ['0','-','1','-','2','-','3','-','4']                          │
└────────────────────────────────────────────────────────────────┘
```

Запрос:

```sql
SELECT groupArrayInsertAt('-', 5)(toString(number), number * 2) FROM numbers(5);
```

Результат:

```text
┌─groupArrayInsertAt('-', 5)(toString(number), multiply(number, 2))─┐
│ ['0','-','1','-','2']                                             │
└───────────────────────────────────────────────────────────────────┘
```

Многопоточная вставка элементов в одну позицию.

Запрос:

```sql
SELECT groupArrayInsertAt(number, 0) FROM numbers_mt(10) SETTINGS max_block_size = 1;
```

В результат этого запроса мы получите случайное целое число в диапазоне `[0,9]`. Например:

```text
┌─groupArrayInsertAt(number, 0)─┐
│ [7]                           │
└───────────────────────────────┘
```
