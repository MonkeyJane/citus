# 1. SQL命令

## 1.1 建表

### 1.1.1 create table

```shell
# 创建以int属性id为key，包含int属性name的表test1
create table test1(id int primary key, name int);
```

### 1.1.2 master_create_distributed_table

```shell
# 设置表test1为hash分布式表，以属性id为索引. 官方废弃函数，表的复制模式固定为'coordinator'
SELECT master_create_distributed_table('test1', 'id', 'hash');
```

### 1.1.3 create_distributed_table

```shell
# 设置表test1为hash分布式表，以属性id为索引. 表的复制模式由表的分片方式以及 citus.replication_model 决定
SELECT create_distributed_table('test1', 'id', 'hash');
```

### 1.1.4 master_create_worker_shards

```shell
# 设置表test1分片，shard数为2，shard的placement数为2
SELECT master_create_worker_shards('test1', 2, 2);
```

经上述命令创建的表数据如下：

```shell
postgres=# select * from pg_dist_partition;
 logicalrelid | partmethod |                                                        partkey                                                         | colocationid | repmodel 
--------------+------------+------------------------------------------------------------------------------------------------------------------------+--------------+----------
 test1        | h          | {VAR :varno 1 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnoold 1 :varoattno 1 :location -1} |           20 | c

postgres=# select * from pg_dist_shard;
 logicalrelid | shardid | shardstorage | shardminvalue | shardmaxvalue 
--------------+---------+--------------+---------------+---------------
 test1        |  102708 | t            | -2147483648   | -1
 test1        |  102709 | t            | 0             | 2147483647

postgres=# select * from pg_dist_placement;
 placementid | shardid | shardstate | shardlength | groupid 
-------------+---------+------------+-------------+---------
         731 |  102708 |          1 |           0 |      22
         732 |  102708 |          1 |           0 |      23
         733 |  102709 |          1 |           0 |      23
         734 |  102709 |          1 |           0 |      22

postgres=# select * from pg_dist_shard_placement;
 shardid | shardstate | shardlength |    nodename     | nodeport | placementid 
---------+------------+-------------+-----------------+----------+-------------
  102708 |          1 |           0 | 192.168.221.131 |     5432 |         731
  102708 |          1 |           0 | 192.168.221.132 |     5432 |         732
  102709 |          1 |           0 | 192.168.221.132 |     5432 |         733
  102709 |          1 |           0 | 192.168.221.131 |     5432 |         734
```

 

## 1.2 建集群

### 1.2.1 master_add_node

```shell
# 添加primary的Worker节点
select * from master_add_node('192.168.221.131', 5432);
select * from master_add_node('192.168.221.132', 5432);
```

### 1.2.2 master_add_secondary_node

```shell
# 添加primary节点的secondary节点
select * from master_add_secondary_node('192.168.221.135', 5432, '192.168.221.131', 5432);
select * from master_add_secondary_node('192.168.221.136', 5432, '192.168.221.132', 5432);
```

经上述命令的集群信息如下：

```shell
postgres=# select * from pg_dist_node;
 nodeid | groupid |    nodename     | nodeport | noderack | hasmetadata | isactive |  noderole  | nodecluster 
--------+---------+-----------------+----------+----------+-------------+----------+------------+-------------
     29 |      22 | 192.168.221.131 |     5432 | default  | f           | t        | primary    | default
     30 |      23 | 192.168.221.132 |     5432 | default  | f           | t        | primary    | default
     32 |      22 | 192.168.221.135 |     5432 | default  | f           | t        | secondary  | default
     33 |      23 | 192.168.221.136 |     5432 | default  | f           | t        | secondary  | default
```



### 1.2.3 master_add_coordinator

```
select * from master_add_coordinator('192.168.221.133',5432);
select * from master_add_coordinator('192.168.221.134',5432,'cn_rtap');
```



### 1.2.4 master_remove_node

```
select * from master_remove_node('192.168.221.131',5432);
```



### 1.2.5 start_metadata_sync_to_node

```
select * from start_metadata_sync_to_node('192.168.221.131', 5432);
```



### 1.2.6 stop_metadata_sync_to_node

```
select * from stop_metadata_sync_to_node('192.168.221.131', 5432);
```



## 1.3 配置属性



## 1.4 元数据查询

### 1.4.1 查询表relid

```sql
select
A.SCHEMANAME,
A.TABLENAME,
A.INDEXNAME,
A.TABLESPACE,
A.INDEXDEF,
B.AMNAME,
C.INDEXRELID,
C.INDRELID,
C.INDNATTS,
C.INDISUNIQUE,
C.INDISPRIMARY,
C.INDISCLUSTERED,
D.DESCRIPTION
from
PG_AM B left join PG_CLASS F on
B.OID = F.RELAM left join PG_STAT_ALL_INDEXES E on
F.OID = E.INDEXRELID left join PG_INDEX C on
E.INDEXRELID = C.INDEXRELID left outer join PG_DESCRIPTION D on
C.INDEXRELID = D.OBJOID,
PG_INDEXES A
where
A.SCHEMANAME = E.SCHEMANAME
and A.TABLENAME = E.RELNAME
and A.INDEXNAME = E.INDEXRELNAME
and E.SCHEMANAME = 'pg_catalog'
and E.RELNAME = 'pg_dist_node';
```



# 2. 配置项

## 2.1 

### 2.1.1 ReplicationModel

配置项名：citus.replication_model 

初始值：REPLICATION_MODEL_COORDINATOR('c')

取值范围：

REPLICATION_MODEL_COORDINATOR('c')  -- statement

REPLICATION_MODEL_STREAMING('s')        -- streaming

REPLICATION_MODEL_2PC('t')

REPLICATION_MODEL_INVALID('i')

设置范围：

REPLICATION_MODEL_COORDINATOR('c')

REPLICATION_MODEL_STREAMING('s')

功能：

参考：`set citus.replication_model=streaming;`

### 2.1.2 ShardCount

配置项名：citus.shard_count

初始值：32

取值范围：[1, 64000]

设置范围：[1, 64000]

功能：

参考：`set citus.shard_count=2;`

### 2.1.3 ShardReplicationFactor

配置项名：citus.shard_replication_factor

初始值：1

取值范围：[1, 100]

设置范围：[1, 100]

功能：

参考：`set citus.shard_replication_factor=2;`

### 2.1.4 CurrentCluster

配置项名：citus.cluster_name

初始值：'default'

取值范围：

设置范围：

功能：



### 2.1.5 WritableStandbyCoordinator

配置项名：citus.writable_standby_coordinator

初始值：false

取值范围：

设置范围：

功能：



### 2.1.6 MultiShardConnectionType

配置项名：citus.multi_shard_modify_mode

初始值：PARALLEL_CONNECTION

取值范围：

设置范围：

功能：



### 2.1.7 TaskExecutorType

配置项名：citus.task_executor_type

初始值：MULTI_EXECUTOR_REAL_TIME

取值范围：

设置范围：

功能：



### 2.1.8 ShardPlacementPolicy

配置项名：citus.shard_placement_policy

初始值：SHARD_PLACEMENT_ROUND_ROBIN

取值范围：

设置范围：

功能：



### 2.1.9 ReadFromSecondaries

配置项名：citus.use_secondary_nodes

初始值：USE_SECONDARY_NODES_NEVER

取值范围：

设置范围：

功能：



### 2.1.10 EnableRouterExecution

配置项名：citus.enable_router_execution

初始值：true

取值范围：

设置范围：

功能：



### 2.1.11 ExplainAllTasks

配置项名：citus.explain_all_tasks

初始值：false

取值范围：

设置范围：

功能：



### 2.1.12 NextShardId

配置项名：citus.next_shard_id

初始值：

取值范围：

设置范围：

功能：



### 2.1.13 NextPlacementId

配置项名：citus.next_placement_id

初始值：

取值范围：

设置范围：

功能：

### 

### 2.1.14 BinaryMasterCopyFormat

配置项名：citus.binary_master_copy_format

初始值：false

取值范围：

设置范围：

功能：



### 2.1.15 BinaryWorkerCopyFormat

配置项名：citus.binary_worker_copy_format

初始值：false

取值范围：

设置范围：

功能：



创建分布式表过程中：

在 DistColocationRelationId() 中，也即 pg_dist_colocation 表，根据shardcount(分片数)、replicationfactor(分片placement数)、distributioncolumntype(分布列类型)，查找当前待创建的表是否存在colocation 表。若不存在，则根据待创建的表对应属性数据生成1条新纪录，插入 pg_dist_colocation 表中。colocationid通过GetNextColocationId()递增

```
postgres=# select * from pg_dist_colocation;
 colocationid | shardcount | replicationfactor | distributioncolumntype 
--------------+------------+-------------------+------------------------
           17 |         32 |                 1 |                     23
           18 |          8 |                 1 |                     23
(2 rows)
```

基于relationId(表id)、distributionMethod(分片方式)、distributionColumn(分片字段)、colocationId(亲和id)、replicationModel(表复制模式)生成1条新数据，插入 pg_dist_partition 表中。

```
postgres=# select * from pg_dist_partition;
 logicalrelid | partmethod |                                                        partkey                                                         | colocationid | repmodel 
--------------+------------+------------------------------------------------------------------------------------------------------------------------+--------------+----------
 test1        | h          | {VAR :varno 1 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnoold 1 :varoattno 1 :location -1} |            0 | c
 test2        | h          | {VAR :varno 1 :varattno 1 :vartype 23 :vartypmod -1 :varcollid 0 :varlevelsup 0 :varnoold 1 :varoattno 1 :location -1} |            0 | c
(2 rows)
```

