1、分片在线迁移的入参校验流程
  （1） shard_id ：
    （1）在 src_node 存在且有效
    （2）不在 dst_node 上
    （3）不在当前的迁移任务中（分片/亲和分片）
  （2）src_node ：存在且有效
  （3）dst_node ：存在且有效
  （4）task_name ：唯一，也即不在当前的迁移任务中

2、分片在线迁移的可重入流程
  （1）入参校验：
       task_name : fdd_dist_move_task 表中存在
  （2）重入流程：
      （1） SHARD_DELETE_OLD: 在线迁移已完毕；文字提示，不作处理
      （2） SHARD_MOVE_END: 分片迁移已完毕；若分片处理模式为保留源分片，则文字提示，不作处理。检查任务是否在缓存中，若不存在则插入，确保该任务能被状态机调度
      （3） SHARD_REP_READY: 分片复制已完毕；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
      （4） SHARD_REP_SYNCDONE: 分片复制中；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
      （5） SHARD_REP_DATASYNC: 分片复制中；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
      （6） SHARD_REP_INIT: 分片复制中；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
      （7） SHARD_WAITE_POST: 分片迁移前期准备已完毕；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
      （8） SHARD_CREATED_TABLE: 目标节点创建表已完毕；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度
	  （9） SHARD_INITIAL: 复制任务处理进程分配已完毕；检查任务是否在缓存中，若存在，则文字提示，不作处理。若不存在则插入，确保该任务能被状态机调度


预处理进程初始化动作：
（1）若进程已完成初始化，则直接退出
（2）fork 创建进程
（3）将入参数据写入到自定义表中（fdd_dist_move_task 与 fdd_dist_move_shard 与 fdd_dist_move_worker）

调度进程初始化动作：
（1）若进程已完成初始化，则直接退出
（2）注册动态后台进程
（3）从自定义表中读取数据，并初始化相关全局缓存

表数据动作：
（1）根据启动在线迁移的入参信息，结合 pg_dist_node 与 pg_dist_shard 以及 pg_dist_placement 等元数据表内容，
	完成 fdd_dist_move_task 与 fdd_dist_move_shard 以及 fdd_dist_move_worker 数据插入

缓存赋值动作：
（1）若任务在缓存中已存在，则直接退出
（2）查询 fdd_dist_move_task 与 fdd_dist_move_shard 表数据生成一个 TaskMoveCacheEntry 插入 g_TaskMoveCacheList
（3）查询 pg_dist_shard 与 pg_dist_placement 表数据生成一个 ShardPostUnitEntry 。若 g_TaskMoveCacheList.TaskMoveCacheEntry.shard_state 状态小于 SHARD_REP_READY ，
	则插入 g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardWaitingList ；否则插入 g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardPostingHash
（4）根据 fdd_dist_move_shard 表数据生成若干个 MoveShardCacheEntry 插入 g_MoveShardCacheHash
（5）查询 fdd_dist_move_worker 表数据，生成一个 MoveScheduleCacheEntry 插入 g_MoveScheduleCacheHash



3、状态机调度
SHARD_INITIAL : 
（1）执行进程初始化、缓存赋值动作；若为初次创建，则执行表数据动作
（2）状态跳转为 SHARD_CREATED_TABLE
（3）更新 fdd_dist_move_task 与 fdd_dist_move_shard 表数据

SHARD_CREATED_TABLE : 
（1）若为重入，则执行进程初始化、缓存赋值动作
（2）根据 fdd_dist_move_worker.target_groupid 获取 pg_dist_node 表中目的端信息，在目的端创建对应的所有目的分片（g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardWaitingList.SortedColShardArray）
	（若分片已存在，怎么处理？）
（3）状态跳转为 SHARD_WAITE_POST
（4）更新 fdd_dist_move_task 与 fdd_dist_move_shard 表数据

SHARD_WAITE_POST : 
（1）若为重入，则执行进程初始化、缓存赋值动作；并根据 g_MoveScheduleCacheHash.MoveScheduleCacheEntry 信息，在目的端查询分片是否存在
	（若分片不存在，怎么处理？）
（2）根据 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.WorkerNum 与 当前分片在 g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardWaitingList 中所处位置，决定是否投递当前分片；
	若不允许投递，则直接退出，不作处理
（3）将当前分片任务从 g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardWaitingList 转移到 g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardPostingHash 中
（4）若当前分片迁移的逻辑复制进程未启动，则根据 g_MoveScheduleCacheHash.MoveScheduleCacheEntry 信息，在源端创建发布者、目的端创建订阅者
（5）若逻辑复制进程已启动，则查看发布端的发布表是否包含当前分片或者其亲和分片。
	根据逻辑复制最优算法，决定是否在分片数组（g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardPostingHash.SortedColShardArray）中选择部分分片，进行发布
（6）根据 fdd_dist_move_worker 与 pg_dist_node 表数据，生成一个 MoveWorker 保存到 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.MoveWorkerArray 中，
	并累加体现到 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.WorkerNum 
（7）将本次发布的所有分片都保存到 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.MoveWorkerArray.PostedShardsHash 中
（8）状态跳转为 SHARD_REP_INIT
（9）更新 fdd_dist_move_task 与 fdd_dist_move_shard 与 fdd_dist_move_worker 表数据

SHARD_REP_INIT/SHARD_REP_DATASYNC/SHARD_REP_SYNCDONE : 
（1）若不是重入操作，则状态跳转以及刷新自定义表（fdd_dist_move_task 与 fdd_dist_move_shard 与 fdd_dist_move_worker）数据由分片逻辑复制状态决定。
	直接退出，不作处理
（2）执行 SHARD_WAITE_POST 的步骤（1-4）
（3）清空目的端分片数据
	（若分片不存在，怎么处理？）
（4）执行 SHARD_WAITE_POST 的步骤（5-9）

SHARD_REP_READY : 
（1）若为重入，则执行进程、缓存初始化动作
（2）查看分片数组（g_TaskMoveCacheList.TaskMoveCacheEntry.MoveShardPostingHash.SortedColShardArray）中是否还有未发布的分片。
	若是，则按照最优算法选择其中1个分片，在发布端由发布者进行发布，并刷新订阅端订阅者。对该新发布的分片执行 SHARD_WAITE_POST 的步骤（7-9）
（3）若不存在未发布的分片，则统计 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.MoveWorkerArray.PostedShardsHash 中所有分片状态，是否都为 SHARD_REP_READY 状态。
	若不是，则不作处理
（4）若全部为 READY 状态，则将当前任务从 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.MoveWorkerArray 中删除，并自减 g_MoveScheduleCacheHash.MoveScheduleCacheEntry.WorkerNum 
（5）根据 g_TaskMoveCacheList.TaskMoveCacheEntry.lock_mode 模式，判断是否自动上锁。若为手动模式，则文字提示，不作处理
（6）若为自动上锁模式，则根据 g_TaskMoveCacheList.TaskMoveCacheEntry.lock_mode 上锁粒度，判断是否符合上锁条件。
	若不符合，则直接退出，不作处理
（7）完成源端数据上锁，根据 g_TaskMoveCacheList.TaskMoveCacheEntry.check_pattern 对两端数据进行校验。若校验失败，则报警提示
（8）进行元数据切换
（9）状态跳转为 SHARD_MOVE_END

SHARD_MOVE_END :
（1）若为重入，则执行进程、缓存初始化动作
（2）根据 g_TaskMoveCacheList.TaskMoveCacheEntry.drop_method ，判断是否需要删除源端数据。若不需要，则文字提示；若需要，则删除源端数据
（3）状态跳转为 SHARD_DELETE_OLD

SHARD_DELETE_OLD :
（1）文字提示：在线迁移完毕


/*
 * 构造三个自定义表
 */
typedef struct FormData_fdd_dist_move_task
{
	Name taskname;
	int taskid;
	ShardRepMode rep_mode;
	ShardDropMode drop_method;
	LockMode lock_mode;
	DataCheckMode check_pattern;
	TimestampTz start_time;
	TimestampTz latest_end_time;
	int64 datasize;
	int shard_state;
} FormData_fdd_dist_move_task;

/* ----------------
 *      Form_fdd_dist_move_task corresponds to a pointer to a tuple with
 *      the format of fdd_dist_move_task relation.
 * ----------------
 */
typedef FormData_fdd_dist_move_task *Form_fdd_dist_move_task;

/* ----------------
 *      compiler constants for fdd_dist_move_task
 * ----------------
 */
#define Natts_fdd_dist_move_task 10
#define Anum_fdd_dist_move_task_taskname 1
#define Anum_fdd_dist_move_task_taskid 2
#define Anum_fdd_dist_move_task_repmode 3
#define Anum_fdd_dist_move_task_dropmode 4
#define Anum_fdd_dist_move_task_lockmode 5
#define Anum_fdd_dist_move_task_checkmode 6
#define Anum_fdd_dist_move_task_starttime 7
#define Anum_fdd_dist_move_task_lastendtime 8
#define Anum_fdd_dist_move_task_datasize 9
#define Anum_fdd_dist_move_task_shardstate 10


typedef struct FormData_fdd_dist_move_shard
{
	int shardid;
	int groupid;
	int repid;
	Oid tablename;
	int taskid
	int assign_shardid;
	int64 shard_size;
	TimestampTz state_begin_time;
	int shard_state;
} FormData_fdd_dist_move_shard;

/* ----------------
 *      Form_fdd_dist_move_task corresponds to a pointer to a tuple with
 *      the format of fdd_dist_move_shard relation.
 * ----------------
 */
typedef FormData_fdd_dist_move_shard *Form_fdd_dist_move_shard;

/* ----------------
 *      compiler constants for fdd_dist_move_shard
 * ----------------
 */
#define Natts_fdd_dist_move_shard 9
#define Anum_fdd_dist_move_shard_shardid 1
#define Anum_fdd_dist_move_shard_groupid 2
#define Anum_fdd_dist_move_shard_repid 3
#define Anum_fdd_dist_move_shard_tablename 4
#define Anum_fdd_dist_move_shard_taskid 5
#define Anum_fdd_dist_move_shard_assign_shardid 6
#define Anum_fdd_dist_move_shard_shard_size 7
#define Anum_fdd_dist_move_shard_start_time 8
#define Anum_fdd_dist_move_shard_shard_state 9


typedef struct FormData_fdd_dist_move_worker
{
	int repid;
	int pid;
	Name sub_name;
	Name pub_name;
	int source_groupid;
	int target_groupid;
	int rep_state;
} FormData_fdd_dist_move_worker;

/* ----------------
 *      FormData_fdd_dist_move_worker corresponds to a pointer to a tuple with
 *      the format of fdd_dist_move_worker relation.
 * ----------------
 */
typedef FormData_fdd_dist_move_worker *Form_fdd_dist_move_worker;

/* ----------------
 *      compiler constants for fdd_dist_move_worker
 * ----------------
 */
#define Natts_fdd_dist_move_worker 7
#define Anum_fdd_dist_move_worker_repid 1
#define Anum_fdd_dist_move_worker_pid 2
#define Anum_fdd_dist_move_worker_subname 3
#define Anum_fdd_dist_move_worker_pubname 4
#define Anum_fdd_dist_move_worker_source_groupid 5
#define Anum_fdd_dist_move_worker_target_groupid 6
#define Anum_fdd_dist_move_worker_rep_state 7


/*
 * 三个自定义表的查询函数
 */
static HeapTuple
FDD_LookupMoveTaskTuple(char* taskName, bool missOk)
{
	Relation fddDistMoveTask = heap_open(FddDistMoveTaskRelationId(), AccessShareLock);
	const int scanKeyCount = 1;
	const bool indexOK = false;

	ScanKeyData scanKey[1];
	SysScanDesc scanDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	HeapTuple taskTuple = NULL;

	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_task_taskname,
				BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(taskName));
	scanDescriptor = systable_beginscan(fddDistMoveTask, InvalidOid, indexOK,
										NULL, scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		taskTuple = heap_copytuple(heapTuple);
	}
	else if (!missOk)
	{
		ereport(ERROR, (errmsg("could not find valid entry for task %s", taskName)));
	}

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveTask, NoLock);

	return taskTuple;
}


static HeapTuple
FDD_LookupMoveShardTuple(int shardId, int repId, bool missOk)
{
	Relation fddDistMoveShard = heap_open(FddDistMoveShardRelationId(), AccessShareLock);
	const int scanKeyCount = 2;
	const bool indexOK = false;

	ScanKeyData scanKey[2];
	SysScanDesc scanDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	HeapTuple shardTuple = NULL;

	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_shard_shardid
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(shardId));
	ScanKeyInit(&scanKey[1], Anum_fdd_dist_move_shard_repid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(repId));
	scanDescriptor = systable_beginscan(fddDistMoveShard, InvalidOid, indexOK,
										NULL, scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		shardTuple = heap_copytuple(heapTuple);
	}
	else if (!missOk)
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard %d in group %d", shardId, groupId)));
	}

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveShard, NoLock);

	return shardTuple;
}


static HeapTuple
FDD_LookupMoveWorkerTuple(int repid, bool missOk)
{
	Relation fddDistMoveWorker = heap_open(FddDistMoveWorkerRelationId(), AccessShareLock);
	const int scanKeyCount = 1;
	const bool indexOK = false;

	ScanKeyData scanKey[1];
	SysScanDesc scanDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	HeapTuple workerTuple = NULL;

	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_worker_repid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(repid));
	scanDescriptor = systable_beginscan(fddDistMoveWorker, InvalidOid, indexOK,
										NULL, scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		workerTuple = heap_copytuple(heapTuple);
	}
	else if (!missOk)
	{
		ereport(ERROR, (errmsg("could not find valid entry for move worker %d", repid)));
	}

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveWorker, NoLock);

	return workerTuple;
}


/*
 * 三个自定义表的DML函数
 */
void
UpdateTaskShardState(char* taskName, int shardState)
{
	Relation fddDistMoveTask = heap_open(FddDistMoveTaskRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(fddDistMoveTask);
	ScanKeyData scanKey[1];
	SysScanDesc scanDescriptor = NULL;
	const int scanKeyCount = 1;
	const bool indexOK = false;
	HeapTuple heapTuple = NULL;
	Datum values[Natts_fdd_dist_move_shard];
	bool isnull[Natts_fdd_dist_move_shard];
	bool replace[Natts_fdd_dist_move_shard];

	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_task_taskname,
				BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(taskName));
	scanDescriptor = systable_beginscan(fddDistMoveTask, InvalidOid, indexOK,
										NULL, scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard %d in repId %d",
							   shardId, repId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_fdd_dist_move_task_shardstate - 1] = Int32GetDatum(shardState);
	isnull[Anum_fdd_dist_move_task_shardstate - 1] = false;
	replace[Anum_fdd_dist_move_task_shardstate - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(fddDistMoveTask, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(FddDistMoveTaskRelationId());

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveTask, NoLock);
}


void
UpdateShardState(int shardId, int repId, int shardState)
{
	Relation fddDistMoveShard = heap_open(FddDistMoveShardRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(fddDistMoveShard);
	ScanKeyData scanKey[2];
	SysScanDesc scanDescriptor = NULL;
	const int scanKeyCount = 2;
	const bool indexOK = false;
	HeapTuple heapTuple = NULL;
	Datum values[Natts_fdd_dist_move_shard];
	bool isnull[Natts_fdd_dist_move_shard];
	bool replace[Natts_fdd_dist_move_shard];
	
	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_shard_shardid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(shardId));
	ScanKeyInit(&scanKey[1], Anum_fdd_dist_move_shard_repid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(repId));

	scanDescriptor = systable_beginscan(fddDistMoveShard, InvalidOid, indexOK,
										NULL, scanKeyCount, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard %d in repId %d",
							   shardId, repId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_fdd_dist_move_shard_shard_state - 1] = Int32GetDatum(shardState);
	isnull[Anum_fdd_dist_move_shard_shard_state - 1] = false;
	replace[Anum_fdd_dist_move_shard_shard_state - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(fddDistMoveShard, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(FddDistMoveShardRelationId());

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveShard, NoLock);
}


void
UpdateWorkerRepState(int repId, int repState)
{
	Relation fddDistMoveWorker = heap_open(FddDistMoveWorkerRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(fddDistMoveWorker);
	ScanKeyData scanKey[1];
	SysScanDesc scanDescriptor = NULL;
	const int scanKeyCount = 1;
	const bool indexOK = false;
	HeapTuple heapTuple = NULL;
	Datum values[Natts_fdd_dist_move_shard];
	bool isnull[Natts_fdd_dist_move_shard];
	bool replace[Natts_fdd_dist_move_shard];

	ScanKeyInit(&scanKey[0], Anum_fdd_dist_move_worker_repid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(repId));
	scanDescriptor = systable_beginscan(fddDistMoveWorker, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for shard %d in repId %d",
							   shardId, repId)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_fdd_dist_move_worker_rep_state - 1] = Int32GetDatum(repState);
	isnull[Anum_fdd_dist_move_worker_rep_state - 1] = false;
	replace[Anum_fdd_dist_move_worker_rep_state - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(fddDistMoveWorker, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(FddDistMoveWorkerRelationId());

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);
	heap_close(fddDistMoveWorker, NoLock);
}


/*
 * 全局变量初始化函数
 */



/*
 * 预处理进程启动与初始化
 */



/*
 * 调度进程启动与初始化
 */
typedef struct MoveShardRepWorker
{
	/* Time at which this worker was launched. */
	TimestampTz launch_time;
	
	/* Increased everytime the slot is taken by new worker. */
	uint16		generation;

	/* Pointer to proc array. NULL if not running. */
	PGPROC	   *proc;

	/* Database id to connect to. */
	Oid			dbid;

	/* User to use for connection (will be same as owner of subscription). */
	Oid			userid;
	
	bool        in_use;
} MoveShardRepWorker;

MoveShardRepWorker *MyMoveShardWorker = NULL;
extern MoveShardRepWorker *MyMoveShardWorker;


void
FDD_MoveShardWorkerShmemInit(void)
{
	bool found = false;
	Size workerSize = sizeof(MoveShardRepWorker);

	MyMoveShardWorker = (MoveShardRepWorker *)
		ShmemInitStruct("MoveShard Worker Data", workerSize, &found);

	if (!found)
	{
		MemSet(MoveShardRepWorker, 0, workerSize);
		MoveShardRepWorker->in_use = true;
	}
}


void
FDD_MoveShardWorkerLaunch(Oid dbid, Oid userid)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	MoveShardRepWorker args;
	MemoryContextCallback *workerCleanup = NULL;

	ereport(INFO, (errmsg("starting move shard worker")));

	/*
	 * We need to do the modification of the shared memory under lock so that
	 * we have consistent view.
	 */
	LWLockAcquire(MoveShardRepWorkerLock, LW_EXCLUSIVE);

	// 设置worker相关属性作为附加入参
	args.launch_time = GetCurrentTimestamp();
	args.generation++;
	args.proc = NULL;
	args.dbid = dbid;
	args.userid = userid;
	
	LWLockRelease(MoveShardRepWorkerLock);

	/* Register the new dynamic worker. */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |	BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "citus");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "FDD_MoveShardRepWorkerMain");
	
	snprintf(bgw.bgw_name, BGW_MAXLEN, "move shard worker");
	
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = Int32GetDatum(MyProcPid);
	Assert(sizeof(bgw.bgw_extra) >= sizeof(args));
	memcpy(bgw.bgw_extra, &args, sizeof(args));

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background worker slots"),
				 errhint("You might need to increase max_worker_processes.")));
	}
	
	workerCleanup = palloc0(sizeof(MemoryContextCallback));
	workerCleanup->func = FDD_MoveShardRepWorker_Cleanup;
	workerCleanup->arg = bgw_handle;

	MemoryContextRegisterResetCallback(CurrentMemoryContext, workerCleanup);

	return bgw_handle;
}


void 
FDD_MoveShardRepWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);
	MemoryContext oldctx;
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos;
	char	   *myslotname;
	WalRcvStreamOptions options;

	/* Attach to slot */
	logicalrep_worker_attach(worker_slot);

	/* Setup signal handling */
	pqsignal(SIGHUP, logicalrep_worker_sighup);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL,
											   "logical replication apply");

	/* Connect to our database. */
	BackgroundWorkerInitializeConnectionByOid(MyLogicalRepWorker->dbid,
											  MyLogicalRepWorker->userid);

	/* Load the subscription into persistent memory context. */
	ApplyContext = AllocSetContextCreate(TopMemoryContext,
										 "ApplyContext",
										 ALLOCSET_DEFAULT_SIZES);
	StartTransactionCommand();
	oldctx = MemoryContextSwitchTo(ApplyContext);
	MySubscription = GetSubscription(MyLogicalRepWorker->subid, false);
	MySubscriptionValid = true;
	MemoryContextSwitchTo(oldctx);


	if (!MySubscription->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" will not "
						"start because the subscription was disabled during startup",
						MySubscription->name)));

		proc_exit(0);
	}

	/* Keep us informed about subscription changes. */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONOID,
								  subscription_change_cb,
								  (Datum) 0);
}

/* 挂起等待关闭进程的信号触发 */
static void
FDD_WaitForMoveShardWorkerAttach(MoveShardRepWorker *worker,
							   uint16 generation,
							   BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int rc;

	for (;;)
	{
		pid_t pid;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(MoveShardRepWorkerLock, LW_SHARED);

		/* Worker either died or has started; no need to do anything. */
		if (worker->proc)
		{
			LWLockRelease(MoveShardRepWorkerLock);
			return;
		}

		LWLockRelease(MoveShardRepWorkerLock);

		/* Check if worker has died before attaching, and clean up after it. */
		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STOPPED)
		{
			LWLockAcquire(MoveShardRepWorkerLock, LW_EXCLUSIVE);
			/* Ensure that this was indeed the worker we waited for. */
			if (generation == worker->generation)
				logicalrep_worker_cleanup(worker);
			LWLockRelease(MoveShardRepWorkerLock);
			return;
		}

		/*
		 * We need timeout because we generally don't get notified via latch
		 * about the worker attach.  But we don't expect to have to wait long.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}

	return;
}


static void
FDD_MoveShardRepWorker_Cleanup(MoveShardRepWorker *worker)
{
	Assert(LWLockHeldByMeInMode(MoveShardRepWorkerLock, LW_EXCLUSIVE));

	worker->proc = NULL;
	worker->dbid = InvalidOid;
	worker->userid = InvalidOid;
}


/*
 * 进程的信号处理
 */
void FDD_MoveShardRepWorker_WakeUp(MoveShardRepWorker *worker)
{
	
}


/*
 * 三个自定义表的 Tuple 转换为结构体变量函数
 */
static Form_fdd_dist_move_task
TupleToMoveTask(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Form_fdd_dist_move_task moveTask = NULL;
	Datum datumArray[Natts_fdd_dist_move_task];
	bool isNullArray[Natts_fdd_dist_move_task];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);
	
	moveTask = CitusMakeNode(Form_fdd_dist_move_task);
	moveTask->taskname = DatumGetName(datumArray[Anum_fdd_dist_move_task_taskname - 1]);
	moveTask->taskid = DatumGetInt32(datumArray[Anum_pg_dist_shard_taskid - 1]);
	moveTask->rep_mode = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_repmode - 1]);
	moveTask->drop_method = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_dropmode - 1]);
	moveTask->lock_mode = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_lockmode - 1]);
	moveTask->check_pattern = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_checkmode - 1]);

	moveTask->start_time = DatumGetTimestampTz(datumArray[Anum_fdd_dist_move_task_starttime - 1]);
	moveTask->latest_end_time = DatumGetTimestampTz(datumArray[Anum_fdd_dist_move_task_lastendtime - 1]);
	moveTask->datasize = DatumGetInt64(datumArray[Anum_fdd_dist_move_task_datasize - 1]);
	moveTask->shard_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_shardstate - 1]);

	return moveTask;
}


static Form_fdd_dist_move_shard
TupleToMoveShard(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Form_fdd_dist_move_shard moveShard = NULL;
	Datum datumArray[Natts_fdd_dist_move_shard];
	bool isNullArray[Natts_fdd_dist_move_shard];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);
	
	moveShard = CitusMakeNode(Form_fdd_dist_move_shard);
	moveShard->shardid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_shardid - 1]);
	moveShard->groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_groupid - 1]);
	moveShard->repid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_repid - 1]);
	moveShard->tablename = DatumGetObjectId(datumArray[Anum_fdd_dist_move_shard_tablename - 1]);
	moveShard->taskid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_taskid - 1]);
	
	moveShard->assign_shardid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_assign_shardid - 1]);
	moveShard->shard_size = DatumGetInt64(datumArray[Anum_fdd_dist_move_shard_shard_size - 1]);
	moveShard->start_time = DatumGetTimestampTz(datumArray[Anum_fdd_dist_move_shard_start_time - 1]);
	moveShard->shard_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_shard_state - 1]);

	return moveShard;
}


static Form_fdd_dist_move_worker
TupleToMoveWorker(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Form_fdd_dist_move_worker moveWorker = NULL;
	Datum datumArray[Natts_fdd_dist_move_worker];
	bool isNullArray[Natts_fdd_dist_move_worker];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);
	
	moveWorker = CitusMakeNode(Form_fdd_dist_move_worker);
	moveWorker->repid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_repid - 1]);
	moveWorker->pid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_pid - 1]);
	
	
	moveWorker->sub_name = DatumGetName(datumArray[Anum_fdd_dist_move_worker_subname - 1]);
	moveWorker->pub_name = DatumGetName(datumArray[Anum_fdd_dist_move_worker_pubname - 1]);
	moveWorker->source_groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_source_groupid - 1]);
	moveWorker->target_groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_target_groupid - 1]);
	moveWorker->rep_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_rep_state - 1]);
	
	return moveWorker;
}


/*
 * fdd_move_shard_placement UDF函数入参校验
 */
void FDD_CheckMoveShardParams(int shardId, char* sourceNodeName, int sourceNodePort, char* targetNodeName, int targetNodePort, char* taskName)
{
	List *shardPlacementList = ShardPlacementList(shardId);
	ShardPlacement *sourcePlacement = NULL;
	ShardPlacement *targetPlacement = NULL;
	bool missingSourceOk = true;
	bool missingTargetOk = true;
	
	// 后续校验源分片时已经确认源节点是否存在, 此时只需校验目的节点是否存在
	if (FindWorkerNodeAnyCluster(targetNodeName, targetNodePort) == NULL)
	{
		ereport(ERROR, (errmsg("node(%s,%u) does not exist", targetNodeName, targetNodePort)));
	}
	
	// 分片应存在于源节点且状态正常
	sourcePlacement = SearchShardPlacementInList(shardPlacementList, sourceNodeName, sourceNodePort, missingSourceOk);
	if (sourcePlacement == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("source node(%s,%u) doesn't have shard %d", sourceNodeName, sourceNodePort, shardId)));		
	}
	else if (sourcePlacement->shardState != FILE_FINALIZED)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("source placement for shard %d must be in finalized state", shardId)));
	}
	
	// 分片不应存在于目的节点
	targetPlacement = SearchShardPlacementInList(shardPlacementList, targetNodeName, targetNodePort, missingTargetOk);
	if (targetPlacement != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("target node(%s,%u) already has the shard %d", targetNodeName, targetNodePort, shardId)));
	}
	
	// 分片不应存在于已有的迁移任务(分片/亲和分片), 查询 fdd_dist_move_shard
	if (FDD_LookupMoveShardTuple(sourcePlacement->groupId, sourcePlacement->shardId, true) != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("shard(%d) has already in the move list", shardId)));
	}
		
	// 任务名称应唯一不重复, 查询 fdd_dist_move_task
	if (FDD_LookupMoveTaskTuple(taskName, true) != NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("task name(%s) should be unique", taskName)));
	}
}


/*
 * 重启在线迁移函数入参校验
 */
void FDD_RestartMoveTask(char *taskName)
{
	HeapTuple taskTuple = FDD_LookupMoveTaskTuple(taskName, true);
	if (taskTuple == NULL)
	{
		ereport(ERROR, (errmsg("could not find valid entry for task %s", taskName)));
	}
	
	Relation distMoveTaskRelation = heap_open(FddDistMoveTaskRelationId(), AccessShareLock);
	TupleDesc moveTaskTupleDesc = RelationGetDescr(distMoveTaskRelation);

	Form_fdd_dist_move_task moveTask = TupleToMoveTask(taskTuple, moveTaskTupleDesc);
	int errCode = moveTask->shard_state % 100;
	int shardState = moveTask->shard_state - errCode;
	
	switch(shardState)
	{
		case SHARD_DELETE_OLD:
		{
			ereport(INFO, (errmsg("task %s has already finished", taskName)));
			break;
		}
		case SHARD_MOVE_END:
		{
			if (moveTask->drop_method == SHARD_RESERVED)
			{
				ereport(INFO, (errmsg("task %s has already finished, source node still keep the shard %d", 
										taskName, moveTask->shardid)));
				break;
			}
			// 检查当前任务是否在缓存中, 若不存在则插入, 让它进入迁移任务状态机. 状态机将对源分片上锁, 并删除源分片
			break;
		}
		case SHARD_REP_READY:
		{
			// 检查当前任务是否在缓存中, 若不存在则插入
			
			// 检测是否符合上锁条件, 若不满足条件则直接提示信息并返回
			
			// 若满足条件 且 为手动上锁模式, 则提示信息并返回
			if (moveTask->lock_mode >= MANUAL_SHARD_MODE)
			{
				break;
			}
			
			// 若满足条件 且 为自动模式, 则进入下一个状态
			break;
		}
		case SHARD_REP_SYNCDONE:
		case SHARD_REP_DATASYNC:
		case SHARD_REP_INIT:
		{
			// 检查当前任务是否在缓存中, 若不存在则插入, 让它进入迁移任务状态机. 状态机将顺序完成后续流程
			break;
		}
		case SHARD_WAITE_POST:
		{
			break;
		}
		case SHARD_CREATED_TABLE:
		{
			break;
		}
		default:
		{
			break;			
		}
		
	}
	
	if (errCode)
	{
		
	}
}


