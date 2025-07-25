---
description: 'Returns a table that is connected via JDBC driver.'
sidebar_label: 'jdbc'
sidebar_position: 100
slug: /sql-reference/table-functions/jdbc
title: 'jdbc'
---

# jdbc Table Function

:::note
clickhouse-jdbc-bridge contains experimental codes and is no longer supported. It may contain reliability issues and security vulnerabilities. Use it at your own risk. 
ClickHouse recommend using built-in table functions in ClickHouse which provide a better alternative for ad-hoc querying scenarios (Postgres, MySQL, MongoDB, etc).
:::

JDBC table function returns table that is connected via JDBC driver.

This table function requires separate [clickhouse-jdbc-bridge](https://github.com/ClickHouse/clickhouse-jdbc-bridge) program to be running.
It supports Nullable types (based on DDL of remote table that is queried).

## Syntax {#syntax}

```sql
jdbc(datasource, external_database, external_table)
jdbc(datasource, external_table)
jdbc(named_collection)
```

## Examples {#examples}

Instead of an external database name, a schema can be specified:

```sql
SELECT * FROM jdbc('jdbc:mysql://localhost:3306/?user=root&password=root', 'schema', 'table')
```

```sql
SELECT * FROM jdbc('mysql://localhost:3306/?user=root&password=root', 'select * from schema.table')
```

```sql
SELECT * FROM jdbc('mysql-dev?p1=233', 'num Int32', 'select toInt32OrZero(''{{p1}}'') as num')
```

```sql
SELECT *
FROM jdbc('mysql-dev?p1=233', 'num Int32', 'select toInt32OrZero(''{{p1}}'') as num')
```

```sql
SELECT a.datasource AS server1, b.datasource AS server2, b.name AS db
FROM jdbc('mysql-dev?datasource_column', 'show databases') a
INNER JOIN jdbc('self?datasource_column', 'show databases') b ON a.Database = b.name
```
