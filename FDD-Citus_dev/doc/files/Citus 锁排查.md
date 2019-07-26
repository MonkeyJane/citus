# 一、任务锁

### 1. LockJobResource <-

​	InitTaskDirectory(job以及task目录创建前调用:AccessExclusiveLock) 

​	task_tracker_assign_task(创建schema前调用:AccessExclusiveLock) 

​	task_tracker_cleanup_job(函数入口调用:AccessExclusiveLock) 

**作用：**对入参 jobId 上锁，调用PG函数 LockAcquire(dontWait=false, 表示若抢不到锁则阻塞)

**锁类型：**入参 lockMode ，所有调用者均传入 AccessExclusiveLock

**锁范围：**SET_LOCKTAG_JOB_RESOURCE

### 2. UnlockJobResource <-

​	InitTaskDirectory(job以及task目录创建后调用)

​	task_tracker_assign_task(创建schema后/schema已存在无需创建时调用)

​	task_tracker_cleanup_job(函数出口调用)

**作用：**对入参 jobId 解锁，调用PG函数 LockRelease

# 二、分区表锁

### 1. LockPartitionRelations <-

​	LockPartitionsInRelationList(入参 lockMode)

​	CoordinatorInsertSelectExecScan(RowExclusiveLock)

​	ExecuteSingleModifyTask(RowExclusiveLock)

​	ExecuteModifyTasks(RowExclusiveLock)

**作用：**若入参 relationId 为PG分区表，则找到它的所有子表的 relationId 上锁，调用PG 函数 LockRelationOid

**锁类型：**入参 lockMode

### 2. LockPartitionsInRelationList <-

​	RealTimeExecScan(AccessShareLock)

​	TaskTrackerExecScan(AccessShareLock) 

​	CitusModifyBeginScan(AccessShareLock)

​	RouterSelectExecScan(AccessShareLock)

**作用：**遍历入参 relationIdList，若 relationId 为PG分区表，则调用Citus函数 **LockPartitionRelations**(本节第1条)

**锁类型：**入参 lockMode

### 3. ExecuteSingleModifyTask <-

​	RouterSequentialModifyExecScan <- RouterModifyExecScan

​	ExecuteModifyTasksSequentiallyWithoutResults <- 

​		master_modify_multiple_shards

​		ExecuteDistributedDDLJob <- multi_ProcessUtility

**作用：**执行单个修改任务。

1. 获取任务涉及的shardId的分片分区数据、relationId
2. 获取到该分片所有placement所在节点的连接
3. 若该表是PG分区表，而且此任务类型为DML操作，则调用Citus函数 **LockPartitionRelations**(本节第1条)
4. 若任务类型为DML或者SELECT，则调用Citus函数 **AcquireExecutorShardLock**(下章3.1小节第4条)
5. 依次在步骤2所得placement执行任务，执行成功则退出

**锁类型：**RowExclusiveLock

### 4. ExecuteModifyTasks <-

​	ExecuteMultipleTasks <- 

​		CoordinatorInsertSelectExecScan

​		RouterMultiModifyExecScan <- RouterModifyExecScan

​		ExecuteModifyTasksWithoutResults <- 

​			master_modify_multiple_shards

​			ExecuteDistributedDDLJob <- multi_ProcessUtility

​			ProcessVacuumStmt <- multi_ProcessUtility

**作用：**执行批量修改任务。

1. 获取入参 taskList 中第一个任务，以及其涉及的 shardId
2. 若步骤1所得 shardId 所属表为PG分区表，而且此任务类型为DML操作，则调用Citus函数 **LockPartitionRelations**(本节第1条) 对该表上锁
3. 继续（目前存在CN事务）或开启CN事务
4. 调用Citus函数 **AcquireExecutorMultiShardLocks**(见 4.1.4)
5. 若，则设置CN事务启用2PC模式(CoordinatedTransactionUses2PC=true)
6. 开启连接执行所有任务后，重置日志级别并释放所有连接(claimedExclusively=false)

**锁类型：**RowExclusiveLock

### 5. lock_relation_if_exists <-

​	AcquireDistributedLockOnRelations <- LockTruncatedRelationMetadataInWorkers(AccessExclusiveLock) <- ProcessTruncateStatement <- multi_ProcessUtility

**作用：**若传入的 relationId 存在，则对其上锁，调用PG函数 RangeVarGetRelidExtended ，实际为 LockRelationOid

**锁类型：**入参 lockMode ，唯一调用者传入 AccessExclusiveLock

# 三、表锁

### 1. LockRelationOid <-

​	ActivateNode

​	AddNodeMetadata

​	AcquireDistributedLockOnRelations

​	BlockDistributedTransactions

​	ColocatedTableId

​	CreateShardsWithRoundRobinPolicy 

​	CreateColocatedShards

​	CreateReferenceTableShard

​	PrepareWorkerNodeCache

​	RemoveNodeFromCluster

​	ReplicateShardToAllWorkers

​	RangeVarCallbackForDropIndex

​	VacuumTaskList

​	LockPartitionRelations

​	master_drop_all_shards

​	master_create_empty_shard

​	master_disable_node

​	master_update_node

​	master_initialize_node_metadata



### 2. RemoveNodeFromCluster <-

**作用：**对 pg_dist_node 表上锁，调用PG函数 LockRelationOid 

**锁类型：**ExclusiveLock

### 3. AddNodeMetadata <-

​	master_add_node

​	master_add_secondary_node

​	master_add_inactive_node

​	master_initialize_node_metadata

**作用：**对 pg_dist_node 表上锁，调用PG函数 LockRelationOid 

**锁类型：**ExclusiveLock

### 4. ReplicateShardToAllWorkers <-

​	ReplicateSingleShardTableToAllWorkers <- upgrade_to_reference_table

**作用：**对 pg_dist_node 表上锁，调用PG函数 LockRelationOid 

**锁类型：**RowShareLock

### 5. LockPartitionRelations <-

​	(调用关系见章节二第1条)

### 6. AcquireDistributedLockOnRelations <-

​	(调用关系见章节二第5条)

**作用：**对入参 relationId 表上锁，调用PG函数 LockRelationOid 

**锁类型：**入参lockMode

### 7. VacuumTaskList <-

​	 ProcessVacuumStmt <- multi_ProcessUtility

**作用：**对入参 relationId 表上锁，调用PG函数 LockRelationOid 

**锁类型：**ShareUpdateExclusiveLock





# 四、分片锁

## 4.1 分片资源锁（阻塞）

### 1. LockShardResource <-

​	lock_shard_resources(入参 lockMode)

​	LockShardListResources(入参 lockMode)

​	AcquireExecutorMultiShardLocks(入参 lockMode)

​	LockRelationShardResources(入参 lockMode)

​	LockParentShardResourceIfPartition(入参 lockMode)

​	acquire_shared_shard_lock(ShareLock)

​	master_append_table_to_shard(ExclusiveLock)

​	upgrade_to_reference_table(ExclusiveLock)

​	worker_append_table_to_shard(AccessExclusiveLock)

**作用：**对入参 shardId 上锁，调用PG函数 LockAcquire (dontWait=false, 表示若抢不到锁则阻塞) 

**锁类型：**入参 lockMode

**锁范围：** SET_LOCKTAG_SHARD_RESOURCE

### 2. lock_shard_resources <-

​	LockShardListResourcesOnFirstWorker <- SerializeNonCommutativeWrites <-

​		CitusCopyDestReceiverStartup(RowExclusiveLock)

​		AcquireExecutorShardLock <- ExecuteSingleModifyTask <- (调用关系见章节二第3条)

**作用：**对传入的批量 shardId 上锁，调用Citus函数 **LockShardResource**(本节第1条)

**锁类型：**入参 lockMode ，唯一调用者传入 RowExclusiveLock

### 3. LockShardListResources <-

​	SerializeNonCommutativeWrites <- (调用关系见本节第2条)

**作用：**对入参 shardIntervalList 进行降序排列，遍历对所有 shardId 上锁，调用Citus函数 **LockShardResource**(本节第1条)

**锁类型：**入参 lockMode ，唯一调用者传入 RowExclusiveLock

### 4. AcquireExecutorMultiShardLocks <-

​	ExecuteModifyTasks <- (调用关系见章节二第4条)

**作用：**遍历入参 taskList 中所有Task，对该任务涉及的 shardId 上锁。

1. 调用Citus函数 **LockParentShardResourceIfPartition**(本节第6条)
2. 调用Citus函数 **LockShardResource**(本节第1条)
3. 若该任务 RequiresConsistentSnapshot ，则调用Citus函数 **LockRelationShardResources**(本节第5条)

**锁类型：**

1. ExclusiveLock : 该任务涉及的placement个数不为1 且 AllModificationsCommutative 为false

2. ShareUpdateExclusiveLock : 不满足类型1场景，且 EnableDeadlockPrevention 为true

3. RowExclusiveLock : 不满足类型1&2场景

   > 需要注意的是：上述锁类型差异只用于步骤1&2，步骤3传入的锁类型固定为 ExclusiveLock 

### 5. LockRelationShardResources <-

​	AcquireExecutorMultiShardLocks <- ExecuteModifyTasks <- (调用关系见章节二第4条)
​	AcquireExecutorShardLock <- ExecuteSingleModifyTask <- (调用关系见章节二第3条)

**作用：**对入参 relationShardList 进行降序排列，遍历对所有 shardId 上锁，调用Citus函数 LockShardResource

**锁类型：**入参 lockMode

### 6. LockParentShardResourceIfPartition <-

​	AcquireExecutorMultiShardLocks <- ExecuteModifyTasks <- (调用关系见章节二第4条)

**作用：**若入参 shardId 所属表为PG分区表，则找到该表的父表中与该shard同index的分片(后续称：亲和分片)。对亲和分片 shardId 上锁，调用Citus函数 LockShardResource

**锁类型：**入参 lockMode

## 4.2 分片元数据锁（阻塞）

### 1. LockShardDistributionMetadata <-

​	LockShardListMetadata(入参 lockMode)

​	LockShardsInPlacementListMetadata(入参 lockMode)

​	LockReferencedReferenceShardDistributionMetadata(入参 lockMode)

​	lock_shard_metadata(入参 lockMode)

​	BlockWritesToShardList(ExclusiveLock)

​	ReplicateAllReferenceTablesToNode(ExclusiveLock)

​	DeleteAllReferenceTablePlacementsFromNodeGroup(ExclusiveLock)

​	RepairShardPlacement(ExclusiveLock)

​	RouterModifyTaskForShardInterval(ShareLock) <- CreateDistributedInsertSelectPlan <- CreateInsertSelectPlan <- CreateDistributedPlan

​	TwoPhaseInsertSelectTaskList(ShareLock) <- CreateDistributedInsertSelectPlan <- CreateInsertSelectPlan <- CreateDistributedPlan

​	CreateShardsWithRoundRobinPolicy(ExclusiveLock) <- CreateHashDistributedTableShards <- CreateDistributedTable

​	CreateReferenceTableShard(ExclusiveLock) <- CreateDistributedTable

​	AcquireMetadataLocks(ShareLock) <- CitusModifyBeginScan

​	master_append_table_to_shard(ShareLock)

​	upgrade_to_reference_table(ExclusiveLock)

**作用：**对入参 shardId 上锁，调用PG函数 LockAcquire (dontWait=false, 表示若抢不到锁则阻塞) 

**锁类型：**入参 lockMode

**锁范围：** SET_LOCKTAG_SHARD_METADATA_RESOURCE

### 2. LockShardsInPlacementListMetadata <-

​	master_update_node(AccessExclusiveLock)

**作用：**对入参 shardPlacementList 降序排列，遍历对 shardId 上锁，调用Citus函数 **LockShardDistributionMetadata**(本节第1条)

**锁类型：**入参 lockMode ，唯一调用者传入 AccessExclusiveLock

### 3. LockShardListMetadata <-

​	CitusCopyDestReceiverStartup(ShareLock)

​	ErrorIfShardPlacementsNotColocated(ShareLock) <- MarkTablesColocated <- mark_tables_colocated

​	CreateColocatedShards(ShareLock) <- CreateHashDistributedTableShards<- CreateDistributedTable

​	CreateIndexTaskList(ShareLock) <- PlanIndexStmt <- multi_ProcessUtility

​	DropIndexTaskList(ShareLock) <- PlanDropIndexStmt <- multi_ProcessUtility

​	ModifyMultipleShardsTaskList(ShareLock) <- master_modify_multiple_shards

​	InterShardDDLTaskList(ShareLock) <- PlanAlterTableStmt <- multi_ProcessUtility

​	VacuumTaskList(ShareLock) <- ProcessVacuumStmt <- multi_ProcessUtility

​	DDLTaskList(ShareLock) <-

​		PlanRenameStmt <- multi_ProcessUtility

​		PlanAlterTableStmt <- multi_ProcessUtility

**作用：**对入参 shardIntervalList 降序排列，遍历对 shardId 上锁，调用Citus函数 **LockShardDistributionMetadata**(本节第1条)

**锁类型：**入参 lockMode ，所有调用者均传入 ShareLock

### 4. LockReferencedReferenceShardDistributionMetadata <-

​	RepairShardPlacement(ExclusiveLock) <- master_copy_shard_placement

​	BlockWritesToShardList(ExclusiveLock) <- (调用关系见本节第6条)

**作用：**若入参 shardId  所属表为分布表，则对该表通过外键关联的所有表上锁。

1. 若入参 shardId 所属表为分布表，则获取该表通过外键关联的所有表
2. 获取步骤1中所有表的所有分区分片信息
3. 若当前集群存在拥有元数据的节点，则调用Citus函数 **LockShardListMetadataOnWorkers** 对步骤2获取的所有分片上锁
4. 遍历步骤2获取的分片，调用Citus函数 **LockShardDistributionMetadata**(本节第1条) 上锁

**锁类型：**入参 lockMode ，所有调用者均传入 ExclusiveLock

#### 4.1 LockShardListMetadataOnWorkers <-

​	LockReferencedReferenceShardDistributionMetadata <- (调用关系见父项)

​	BlockWritesToShardList <- (调用关系见父项)

**作用：**构造SQL指令`SELECT lock_shard_metadata(%d, ARRAY[%lu, %lu, ..., %lu])` 发送给所有元数据节点执行。其中 %d 为锁类型，%lu 为 shardId

**锁类型：**入参 lockMode ，所有调用者均传入 ExclusiveLock

### 5. lock_shard_metadata <-

​	LockShardListMetadataOnWorkers <- (调用关系见本节第4条子项)

**作用：**遍历传入的批量 shardId 上锁，调用Citus函数 **LockShardDistributionMetadata**(本节第1条)

**锁类型：**入参 lockMode ，代码流程中唯一调用者传入 ExclusiveLock ，但作为UDF函数，可由用户直接调用。

### 6. BlockWritesToShardList <- 

​	ReplicateAllReferenceTablesToNode <- (调用关系见本节第7条)

​	DeleteAllReferenceTablePlacementsFromNodeGroup <- (调用关系见本节第8条)

**作用：**遍历入参 shardList 中所有 shardId ，进行上锁操作。

1. 调用Citus函数 **LockReferencedReferenceShardDistributionMetadata** (本节第4条)，对该shardId所属表通过外键关联的所有表的分片上锁
2. 调用 Citus函数 **LockShardDistributionMetadata** (本节第1条)，对该 shardId 上锁
3. 若 shardList 首个 shardId 所属表为参考表、流复制hash表，则调用Citus函数 **LockShardListMetadataOnWorkers**(本节第4条子项)，通知所有拥有元数据的节点对首个 shardId 上锁

**锁类型：**ExclusiveLock 。

### 7. ReplicateAllReferenceTablesToNode <- 

​	ActivateNode <-

​		master_activate_node

​		master_add_node

​		master_add_coordinator

**作用：**复制未部署到入参节点的所有参考表到该节点

1. 获取 pg_dist_partition 表中所有参考表 relationId
2. 对步骤1获取的所有 relationId 降序排列，遍历获取所有参考表分片分区数据
3. 若集群存在拥有元数据的节点，则调用Citus函数 **BlockWritesToShardList**(本节第6条) 对步骤2获取的参考表分片上锁
4. 遍历步骤2获取的参考表分片，调用Citus函数 **LockShardDistributionMetadata**(本节第1条) 上锁，调用Citus函数 ReplicateShardToNode 拷贝该分片到指定节点
5. 将步骤1获取的参考表中第一个表在 pg_dist_colocation 表中数据的属性 replicationfactor， 更新为当前集群所有Active Primary 节点数

**锁类型**：ExclusiveLock 。

### 8. DeleteAllReferenceTablePlacementsFromNodeGroup <-

​	RemoveNodeFromCluster <- master_remove_node

​	master_disable_node

**作用：**删除 pg_dist_placement 表中部署在某 group 节点上所有参考表 placement 的记录

1. 获取 pg_dist_partition 表中所有参考表 relationId
2. 对步骤1获取的所有 relationId 降序排列，若集群存在拥有元数据的节点，则先遍历获取这些参考表的所有分片分区数据，然后调用Citus函数 **BlockWritesToShardList**(本节第6条) 对其上锁
3. 遍历步骤2 排序后的所有参考表，获取所有部署在指定 groupId 节点上的 placement 。先调用Citus函数 **LockShardDistributionMetadata**(本节第1条) 对此 placement 对应的分片上锁，然后删除 pg_dist_placement 表中该placement 对应的记录，并通过Citus函数 SendCommandToWorkers 通知集群所有拥有元数据节点同步 pg_dist_placement 表删除操作。

**锁类型：**ExclusiveLock 。

### 9. RepairShardPlacement <-

​	master_copy_shard_placement

**作用：**修复指定节点上的指定分片。

1. 校验当前用户是否为该shard所属表的owner，若不是则退出
2. 校验该shard所属表是否为 RELKIND_FOREIGN_TABLE 表，若是则报错退出
3. 校验该shard是否有多个placement的分片，若是则报错退出
4. 调用Citus函数 **LockReferencedReferenceShardDistributionMetadata**(本节第4条) ，对该shard所属表通过外键关联的所有表上锁
5. 调用Citus函数 **LockShardDistributionMetadata**(本节第1条) ，对该shard上锁
6. 确认该shard 在源节点上状态正常(FILE_FINALIZED)、且在目标节点上状态异常(FILE_INACTIVE)。若任一节点上不存在此shard 或者shard 状态不符合要求，则报错退出
7. 生成一系列的拷贝命令，并在目标节点上新开启一个transaction ，执行这些指令
8. 获取该shard的所有placement，并获取部署在目标节点的placement。若不存在此placement，则报错退出
9. 刷新 pg_dist_placement 表中该placement的状态为 FILE_FINALIZED

**锁类型：**ExclusiveLock 。



## 4.3  分片元数据锁（非阻塞）

### 1. TryLockShardDistributionMetadata <-

​	ShardIntervalsOnWorkerGroup(ShareLock) <- DistributedTableSizeOnWorker <- DistributedTableSize <-

​		citus_total_relation_size

​		citus_table_size

​		citus_relation_size

**作用：**对入参 shardId 上锁，调用PG函数 LockAcquire (dontWait=true, 表示若抢不到锁则立即返回) 

**锁类型：**入参 lockMode ，唯一调用者传入 ShareLock

**锁范围：** SET_LOCKTAG_SHARD_METADATA_RESOURCE



# 五、共享内存锁

### 1. LockBackendSharedMemory <-

​	InitializeBackendData(函数入口处调用:LW_EXCLUSIVE) <- StartupCitusBackend <- CitusHasBeenLoaded

​	LockLockData(函数入口处调用:LW_SHARED) <- BuildLocalWaitGraph(创建锁图前调用)

​	StoreAllActiveTransactions(读取backendManagementShmemData前调用:LW_SHARED) <- 

​		get_all_active_transactions

​		get_global_active_transactions

**作用：**对共享内存全局变量 backendManagementShmemData 上锁，调用PG函数 LWLockAcquire

**锁类型：**入参 lockMode 

### 2. UnlockBackendSharedMemory <-

​	InitializeBackendData(函数出口处调用) <- StartupCitusBackend <- CitusHasBeenLoaded

​	UnlockLockData(函数出口处调用) <- BuildLocalWaitGraph(函数出口处调用)

​	StoreAllActiveTransactions(函数出口处调用) <- 

​		get_all_active_transactions

​		get_global_active_transactions

**作用：**对共享内存全局变量 backendManagementShmemData 解锁，调用PG函数 LWLockRelease



### 3. BackendManagementShmemInit <-

​	InitializeBackendManagement

**作用：**作为 shmem_startup_hook 钩子函数，创建并初始化共享内存全局变量 backendManagementShmemData 。调用PG函数 LWLockAcquire 以及 LWLockRelease 对PG变量AddinShmemInitLock 加解锁

**锁类型：**LW_EXCLUSIVE 



### 4. InitializeMaintenanceDaemonBackend <-

​	StartupCitusBackend

**作用：**用于初始化并启动后台进程CitusMaintenanceDaemonMain，并将它插入全局Hash表MaintenanceDaemonDBHash中。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量MaintenanceDaemonControl加解锁

**锁类型：**LW_EXCLUSIVE



### 5. CitusMaintenanceDaemonMain <-

​	InitializeMaintenanceDaemonBackend <- StartupCitusBackend

**作用：**Citus后台维护进程，定时进行一系列的数据采集、事务恢复等监控活动。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量MaintenanceDaemonControl加解锁

**锁类型：**LW_SHARED



### 6. MaintenanceDaemonShmemInit <-

​	InitializeMaintenanceDaemon

**作用：**作为 shmem_startup_hook 钩子函数，创建并初始化共享内存全局变量 MaintenanceDaemonControl。调用PG函数 LWLockAcquire 以及 LWLockRelease 对PG变量AddinShmemInitLock 加解锁。

**锁类型：**LW_EXCLUSIVE



### 7. StopMaintenanceDaemon <-

​	multi_ProcessUtility(DropdbStmt时调用)

**作用：**用于关闭后台进程CitusMaintenanceDaemonMain，并将它从全局Hash表MaintenanceDaemonDBHash中删除。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量MaintenanceDaemonControl加解锁

**锁类型：**LW_EXCLUSIVE



### 8. TrackerCleanupJobSchemas <-

​	TaskTrackerMain

**作用：**用于关闭并清空所有DB上的任务。遍历DB列表，生成与之相关的RESERVE任务，插入全局Hash表TaskTrackerTaskHash中等待执行。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**备注：**实际代码中，DB列表为空，此函数不作为

**锁类型：**LW_EXCLUSIVE



### 9. TrackerRegisterShutDown <-

​	TaskTrackerMain

**作用：**用于关闭TaskTracker进程。生成SHUTDOWN任务，插入全局Hash表TaskTrackerTaskHash中等待执行。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_EXCLUSIVE



### 10. TaskTrackerShmemInit <-

​	TaskTrackerRegister

**作用：**作为shmem_startup_hook钩子函数，初始化全局变量WorkerTasksSharedState、Hash表TaskTrackerTaskHash。调用PG函数 LWLockAcquire 以及 LWLockRelease 对PG变量AddinShmemInitLock加解锁。

**锁类型：**LW_EXCLUSIVE



### 11. ManageWorkerTasksHash <-

​	TaskTrackerMain

**作用：**用于管理全局共享内存Hash表TaskTrackerTaskHash中所有任务的执行调度。先后两处调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_SHARED 与 LW_EXCLUSIVE



### 12. task_tracker_assign_task

**作用：**用于往全局共享内存Hash表TaskTrackerTaskHash中插入或更新入参任务。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_EXCLUSIVE



### 13. task_tracker_task_status

**作用：**用于从全局共享内存Hash表TaskTrackerTaskHash中获取入参任务状态。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_SHARED



### 14. task_tracker_cleanup_job

**作用：**用于从全局共享内存Hash表TaskTrackerTaskHash中删除入参jobId涉及的所有任务。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_EXCLUSIVE



### 17. TaskTrackerRunning

**作用：**用于检查TaskTracker进程是否仍在运行。从全局共享内存Hash表TaskTrackerTaskHash中查找是否存在SHUTDOWN任务，若存在则表示进程已停止运行。调用PG函数 LWLockAcquire 以及 LWLockRelease 对共享内存全局变量WorkerTasksSharedState加解锁。

**锁类型：**LW_SHARED