#include "storage/s_lock.h"
#include "postmaster/bgworker.h"
#include "datatype/timestamp.h"
#include "nodes/pg_list.h"
#include "storage/lwlock.h"
#include "storage/proc.h"



//任务类型
typedef enum e_TaskerType
{
   TASKER_INVALID = 0, //非法状态
   TASKER_SCHEDULE,    //调度任务 
   TASKER_NEW,         //新的分片迁移任务
   TASKER_CANCEL       //分片迁移取消任务
}TaskerType;

//迁移操作模式
typedef enum e_DistMoveMode
{
   MOVEMODE_INVALID = 0, //非法状态
   MOVEMODE_SHARD,       //分片迁移
   MOVEMODE_TABLE,       //表数据均衡化
   MOVEMODE_DATABASE     //集群数据均衡化
}DistMoveMode;

///用户输入的迁移任务基本信息
typedef struct _MoveTaskInfo
{
	Name           taskname;      //分布式在线迁移任务名字
	Oid            rep_mode;      //迁移模式，是阻塞写方式还是逻辑复制方式
	Oid            drop_method;   //迁移成功后，旧分片的处理策略
	Oid            lock_mode;     //锁库模式，共六个模式
	Oid            check_pattern; //数据校验模式
	float          threshold;     //介于0.0和1.0之间的浮点数
	DistMoveMode   move_mode;     //迁移模式
	union
	{
		int shardid;
		Name table_name;
		Name database_name;
	};
}MoveTaskInfo;

//具体的任务信息
typedef struct _DistMoveTasker
{
    int             taskid;         //任务id（调度任务该值为-1）
    TimestampTz     start_time;     //任务开始时间
    bool	        in_use;         //当前任务slot是否正在被使用
    bool            is_ready;       //当前任务是否已经前期工作做好，等待调度进程处理
    uint16          generation;     //每次重复使用该槽时自增1，帮忙stop时误处理
    PGPROC	        *proc;          //指向proc array的指针，进程没有运行时为NULL

    TaskerType   tasker_type;       //当前任务类型
	MoveTaskInfo taskinfo;          //当前任务基本信息	 
}DistMoveTasker;

//迁移任务上下文信息，保存在共享内存中
typedef struct _DistMoveCtxStruct
{
    slock_t		   relmutex;        //锁   
    DistMoveTasker tasker_slot[9];  //任务槽，其中第0号槽是留给调度进程的
}DistMoveCtxStruct;

//共享内存
DistMoveCtxStruct DistMoveCtx;

//全局缓存
HTAB *g_MoveShardCacheHash = NULL;
HTAB *g_MoveScheduleCacheHash = NULL;
List *g_TaskMoveCacheList = NIL;

DistMoveTasker *MyMoveWorker = NULL;
static volatile sig_atomic_t got_SIGHUP = false;


/*
 * 构造三个自定义表
 */
typedef struct FormData_fdd_dist_move_task
{
	Name taskname;
	int taskid;
	Oid rep_mode;
	Oid drop_method;
	Oid lock_mode;
	Oid check_pattern;
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
	int taskid;
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


typedef enum RepStatus
{
	REP_INVALID = 0          , //不合法的阶段（不会记录到表中）
	REP_INITIAL              , //初始状态，已经给当前操作分配了若干个复制子任务，并且给每个涉及的分片表也分配了对应的复制子进程序号
	REP_CREATED_TABLE        , //当前迁移操作所涉及的所有分片表已经在目的端创建了对应的表（“迁移操作总状态”独有）
	REP_CREATED_REP_PROCESS  , //对于逻辑复制：创建了逻辑复制的发布端和订阅端；对于阻塞写：创建了复制处理子进程
	REP_STARTUP              , //对于逻辑复制：该条逻辑复制槽正在进行存量更新，没有表进入增量更新；对于阻塞写：当前复制子进程正在copy数据
	REP_PART_CATCHUP         , //该条逻辑复制槽的订阅端中有SYNC进程正在进行追增量操作,但是还有部分进程在追存量或等待追存量（逻辑复制独有）
	REP_CATCHUP              , //该条逻辑复制槽的订阅端中有SYNC进程正在进行追增量操作，剩下其他表都是由APP追增量（逻辑复制独有）
	REP_PART_STREAMING       ,
	REP_STREAMING            , //对于逻辑复制：该逻辑复制槽只存在订阅端APP进程追增量操作；对于阻塞写：当前复制子进程已经把分配的表copy完成
	REP_ROUTE_SWITCH         , //正在进行路由切换操作，需要锁库，当前仅有一个复制处理子进程能够进行该操作
	REP_MOVE_END              //当前批次的表已经复制完成，可以分配新的复制任务，或者进入结束清理操作
}RepStatus;


typedef enum ShardMoveStatus
{
	SHARD_INVALID = 0	,		//不合法的阶段（不会记录到表中）
	SHARD_INITIAL		,		//初始化，经为其分配了复制任务处理进程，但是还不能够进行分片投递
	SHARD_CREATED_TABLE ,		//已经在目的节点上创建了该表且成功
	SHARD_WAITE_POST	,		//前期准备都已经完成，可以进入投递流程
	SHARD_REP_INIT		,		//该表已经投递到复制任务处理进程，但是还没有被具体的复制流程进行处理
	SHARD_REP_DATASYNC	,		//该表正在进行存量同步过程中（COPY）阶段
	SHARD_REP_SYNCDONE	,		//该表正在进行追增量更新过程中（逻辑复制独有）
	SHARD_REP_READY 	,		//该表数据同步完成，可以择机进行锁库（对于逻辑复制表明追增量完成，由APP完成后续增量同步）
	SHARD_MOVE_END		,		//该表复制操作已经结束，且元数据已经切换成功
	SHARD_DELETE_OLD		   //该表已经在源端上删除，只存在于目的节点上 

}ShardMoveStatus;

#define NAME_LEN (128)
#define CONFLICT (200)
#define SRC_CONNECT_FAIL (400)
#define DST_CONNECT_FAIL (500)
#define MANUAL_INTERRUPT (600)
#define REP_APP_ERR (700)
#define REP_APP_SYNC_ERR (800)
#define REP_FETAL_ERR (900)
#define REP_SYNC_ERR (1000)

#define STATE_FAIL_THRESHOLD (5)
#define STATE_RECOVER_THRESHOLD (3)

// 新增结构体定义 : 订阅端的逻辑复制进程信息
typedef struct _sub_status
{
     Oid  sub_id;
     Oid  relid;
     int  slot_pid;
     char slot_pstatus;
     char subrel_satus;
     char sub_name[NAME_LEN];
     char slot_name[NAME_LEN];
     bool sub_enabled;
}sub_status;

// 新增结构体定义 : 复制任务子进程的监控计数
typedef struct _RepStatInfo
{
	int pub_fail_count;
	int sub_fail_count;
	int pub_normal_count;
	int sub_normal_count;
}RepStatInfo;

typedef struct _MoveWorker
{
	int repid;                 // 待迁移分片所分配的复制任务处理子进程序号
	int pid;                   // 复制任务处理子进程的进程号
	char sub_name[NAME_LEN];   // 订阅者名字，阻塞写模式此处为空
	char pub_name[NAME_LEN];   // 发布者名字，阻塞写模式此处为空
	int source_groupid;        // 源节点组id
	int target_groupid;        // 目的节点组id
	int rep_state;             // 复制任务子进程的整体迁移进度
	
    //以下结构不会存到表中，只存在内存中
	bool isValid;                     // 此条目是否收到缓存效通知，是否需要重构缓存
	char source_node_name[NAME_LEN];  // 待迁移分片所在的源节点的DNS名称
	int source_node_port;             // 数据库服务器正在侦听的源节点上的端口
	char target_node_name[NAME_LEN];  // 分片将迁移到目标节点的DNS名称
	int target_node_port;             // 数据库服务器正在侦听的目标节点上的端口
	HTAB *PostedShardsHash;           // key为relationId（订阅端分片表oid），entry为MoveShardCacheEntry
	bool enablePost;                  // 新增成员变量 : 是否允许分片投递
	RepStatInfo statInfo;             // 新增成员变量 : 复制任务子进程的监控计数
}MoveWorker;

// 新增结构体定义 : 逻辑复制进程的监控数据
typedef struct _SubscriptionStat
{
    int sub_pid;            // 订阅者逻辑复制进程pid：进程未启动或终止时此属性为默认值0
	int start_count;        // 逻辑复制进程启动次数
	bool abnormal_check;    // 是否启动异常检测
	int abnormal_count;     // 进程确认异常次数
    int normal_count;       // 进程正常次数
}SubscriptionStat;

//迁移任务进度
typedef struct _TaskMoveCacheEntry
{
	int taskid;
	int rep_mode;
	int drop_method;                 //分片迁移成功后，旧分片的处理策略
	int lock_mode;                   //锁库模式
	int check_pattern;               //数据校验模式
	float threshold;                 //节点利用率与平均利用率的最大差异比率    
	int shard_state;                 //该任务当前处于哪个迁移阶段

	//以下结构不会存到表中，只存在内存中
	bool isValid;                    //此条目是否收到缓存效通知，是否需要重构缓存
	List *MoveShardWaitingList;      //等待投递队列，根据投递算法决定分片顺序
	int MoveShardPostingHashLength;  //在MoveShardPostingHash中分片的个数
	HTAB *MoveShardPostingHash;      //正在投递队列(hash key为shardid，hash entry为ShardPostUnitEntry)
}TaskMoveCacheEntry;

//分片表分组投递单元
typedef struct _ShardPostUnitEntry
{
	int anchorShardId;                 //被指定的进行迁移的分片ID
	int anchorRepId;
	int anchorTaskId;
	Oid anchorRelid;                   //该anchorShardId表的主表的oid
	int anchorShard_state;
	int shardIndex;
	int SortedColShardArrayLength;     //SortedColShardArray 数组的长度
	MoveShardCacheEntry **SortedColShardArray ; //保存根据shardid排序后的anchorShardId以及所有其亲和分片，决定了投递顺序
}ShardPostUnitEntry;

typedef struct _MoveShardCacheEntry
{
	int shardid;            // 待移动的分片ID
	int groupid;            // 待迁移分片表所处的组id
	int repid;              // 待迁移分片所分配的复制任务处理子进程序号
	Oid tablename;          // 待迁移分片的表名（此行对应的分布式表。该值引用pg_class系统目录表中的relfilenode列）
	int taskid;             // 在线迁移任务id
	int assign_shardid;     // 是由哪个分片因为亲和关系，触发当前分片表进行迁移的，若当前值为shardid，则该行为分片迁移指定的分片
	int64 shard_size;       // 分片初始大小（以字节为单位）
	TimestampTz stage_time; // 当前状态开始的时间
	int shard_state;        // 该分片当前处于哪个迁移阶段
    
	// 以下结构不会存到表中，只存在内存中
	bool isValid;           // 此条目是否收到缓存效通知，是否需要重构缓存
	Oid relationId;         // 此分片表在订阅端上的oid
	SubscriptionStat subInfo;  // 新增成员变量 : 逻辑复制进程的监控数据
}MoveShardCacheEntry;

#define NUM_PER_REPLICATION (9)

//复制任务处理槽信息
typedef struct MoveScheduleCacheEntry
{
	int anchorRepId;
	int source_groupid;
	int dst_groupid;
	int rep_state;            //MoveWorkerArray中最小的rep_state
	bool claimedExclusively;
	int idl_num;              //当前复制槽上正在处理的进程空闲的个数，若不为0.则表明可以往其上投入新的分片   
	int WorkerNum;            //当前复制槽上处理进程的个数
	MoveWorker *MoveWorkerArray[NUM_PER_REPLICATION];
}MoveScheduleCacheEntry;



/*
 * 三个自定义表的查询函数
 */
HeapTuple
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


HeapTuple
FDD_LookupMoveShardTuple(int shardId, int taskId, bool missOk)
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
	ScanKeyInit(&scanKey[1], Anum_fdd_dist_move_shard_taskid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(taskId));
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


HeapTuple
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
 * 三个自定义表的 Tuple 转换为结构体变量函数
 */
TaskMoveCacheEntry *
FDD_TupleToMoveTask(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Datum datumArray[Natts_fdd_dist_move_task];
	bool isNullArray[Natts_fdd_dist_move_task];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);

	TaskMoveCacheEntry *taskEntry = (TaskMoveCacheEntry*)palloc0(sizeof(TaskMoveCacheEntry));
	taskEntry->check_pattern = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_checkmode - 1]);
	taskEntry->rep_mode = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_repmode - 1]);
	taskEntry->drop_method = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_dropmode - 1]);
	taskEntry->lock_mode = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_lockmode - 1]);
	taskEntry->shard_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_shardstate - 1]);
	taskEntry->taskid = DatumGetInt32(datumArray[Anum_fdd_dist_move_task_taskid - 1]);
	taskEntry->threshold = 0;
	taskEntry->isValid = true;
	taskEntry->MoveShardPostingHashLength = 0;
	taskEntry->MoveShardPostingHash = FDD_CreateHash("Posting Shard Cache", 32, sizeof(ShardPostUnitEntry));
	taskEntry->MoveShardWaitingList = NIL;

	return taskEntry;
}


MoveShardCacheEntry *
FDD_TupleToMoveShard(HeapTuple heapTuple, TupleDesc tupleDescriptor)
{
	Datum datumArray[Natts_fdd_dist_move_shard];
	bool isNullArray[Natts_fdd_dist_move_shard];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);
	
	MoveShardCacheEntry *shardEntry = (MoveShardCacheEntry*)palloc0(sizeof(MoveShardCacheEntry));
	shardEntry->assign_shardid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_assign_shardid - 1]);
	shardEntry->groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_groupid - 1]);
	shardEntry->repid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_repid - 1]);
	shardEntry->shardid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_shardid - 1]);
	shardEntry->shard_size = DatumGetInt64(datumArray[Anum_fdd_dist_move_shard_shard_size - 1]);
	shardEntry->stage_time = DatumGetTimestampTz(datumArray[Anum_fdd_dist_move_shard_start_time - 1]);
	shardEntry->shard_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_shard_state - 1]);
	shardEntry->tablename = DatumGetObjectId(datumArray[Anum_fdd_dist_move_shard_tablename - 1]);
	shardEntry->taskid = DatumGetInt32(datumArray[Anum_fdd_dist_move_shard_taskid - 1]);
	
	//后续与订阅端通信获取分片表id
	shardEntry->relationId = InvalidOid;

	return shardEntry;
}


MoveWorker *
FDD_TupleToMoveWorker(HeapTuple heapTuple, TupleDesc tupleDescriptor, bool fullInit)
{
	Datum datumArray[Natts_fdd_dist_move_worker];
	bool isNullArray[Natts_fdd_dist_move_worker];
	
	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);
	
	MoveWorker *worker = (MoveWorker *)palloc0(sizeof(MoveWorker));
	worker->repid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_repid - 1]);
	worker->pid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_pid - 1]);

	Name subName = DatumGetName(datumArray[Anum_fdd_dist_move_worker_subname - 1]);
	char *subNameString = NameStr(*subName);
	Name pubName = DatumGetName(datumArray[Anum_fdd_dist_move_worker_pubname - 1]);
	char *pubNameString = NameStr(*pubName);

	strlcpy(worker->sub_name, subNameString, NAME_LEN);
	strlcpy(worker->pub_name, pubNameString, NAME_LEN);
	worker->source_groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_source_groupid - 1]);
	worker->target_groupid = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_target_groupid - 1]);
	worker->rep_state = DatumGetInt32(datumArray[Anum_fdd_dist_move_worker_rep_state - 1]);

	worker->isValid = true;
	if (fullInit)
	{

	}
	else
	{
		memset(worker->source_node_name, 0, NAME_LEN);
		worker->source_node_port = 0;
		memset(worker->target_node_name, 0, NAME_LEN);
		worker->target_node_port = 0;
	}
	worker->PostedShardsHash = FDD_CreateHash("Posted Shard Cache", 32, sizeof(MoveShardCacheEntry));
	worker->enablePost = true;
	memset(worker->statInfo, 0, sizeof(RepStatInfo));
	
	return worker;
}


/* 三个自定义表的INSERT函数 */



/*
 * 三个自定义表的DML函数
 */
void
FDD_UpdateTaskShardState(char* taskName, int shardState)
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
FDD_UpdateShardState(int shardId, int repId, int shardState)
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
FDD_UpdateWorkerRepState(int repId, int repState)
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


/* 搜索全局缓存 */
TaskMoveCacheEntry *
FDD_FindMoveTaskEntry(int taskId, bool *found)
{

}

MoveShardCacheEntry *
FDD_FindMoveShardEntry(int shardId, HASHACTION action, bool *found)
{

}

MoveScheduleCacheEntry *
FDD_FindMoveSchedulaEntry(int repId, HASHACTION action, bool *found)
{

}

ShardPostUnitEntry * 
FDD_GetIndexInWaitingList(List *list, int shardId, int *index)
{
	//在等待链表中查找入参分片信息并返回
}

MoveShardCacheEntry **
FDD_GetAllShardsArray(int shardId, int *len)
{
	//获取入参分片的所有亲和分片id

	//构造数据生成数组(入参shardId作为数组第一个元素)并返回数组长度
}


/* 共享内存初始化 */
void
FDD_MoveShardWorker_ShmemInit(void)
{
	bool found = false;
	Size workerSize = sizeof(DistMoveCtxStruct);

	DistMoveCtx = (DistMoveCtxStruct *)
		ShmemInitStruct("MoveShard Context Data", workerSize, &found);

	if (!found)
	{
		MemSet(DistMoveCtxStruct, 0, workerSize);
	}
}


/* 调度进程注册 */
BackgroundWorkerHandle *
FDD_MoveShardWorker_Launch(int slot_index)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	MemoryContextCallback *workerCleanup = NULL;

	ereport(INFO, (errmsg("starting move shard worker")));

	/* Register the new dynamic worker. */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |	BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = Int32GetDatum(slot_index);
	
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "citus");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "FDD_MoveShardWorker_Main");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "move shard worker");
	
	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background worker slots"),
				 errhint("You might need to increase max_worker_processes.")));
	}
	
	workerCleanup = palloc0(sizeof(MemoryContextCallback));
	workerCleanup->func = FDD_MoveShardWorker_CleanUp;
	workerCleanup->arg = bgw_handle;

	MemoryContextRegisterResetCallback(CurrentMemoryContext, workerCleanup);

	return bgw_handle;
}


/* 调度进程主函数 */
void 
FDD_MoveShardWorker_Main(Datum main_arg)
{
	int worker_slot = DatumGetInt32(main_arg);
	MemoryContext oldctx;
	char		originname[NAMEDATALEN];
	char	   *myslotname;
	WalRcvStreamOptions options;
	
	/* Attach to slot */
	FDD_MoveShardWorker_Attach(worker_slot);
	FDD_MoveShardWorker_CacheInit();

	/* Setup signal handling */
	pqsignal(SIGHUP, FDD_MoveShardWorker_WakeUp);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Connect to our database. */
	BackgroundWorkerInitializeConnectionByOid(MyDatabaseId, InvalidOid, 0);

	FDD_MoveShardWorker_Loop();
}


/* 调度进程缓存初始化 */
void 
FDD_MoveShardWorker_CacheInit()
{
	if (g_MoveScheduleCacheHash == NULL)
	{
		FDD_CreateMoveScheduleCache();
	}

	if (g_MoveShardCacheHash == NULL)
	{
		FDD_CreateMoveShardCache();
	}
}


void 
FDD_CreateMoveShardCache()
{
	HASHCTL info;
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(int);
	info.entrysize = sizeof(MoveShardCacheEntry);
	info.hash = tag_hash;
	info.hcxt = CurrentMemoryContext;
	g_MoveShardCacheHash =
		hash_create("Move Shard Cache", 288, &info,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
}


void 
FDD_CreateMoveScheduleCache()
{
	HASHCTL info;
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(int);
	info.entrysize = sizeof(MoveScheduleCacheEntry);
	info.hash = tag_hash;
	info.hcxt = CurrentMemoryContext;
	g_MoveScheduleCacheHash =
		hash_create("Move Schedule Cache", 32, &info,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
}


void
FDD_ConstructMoveWorker(FormData_fdd_dist_move_worker *workerData, MoveWorker *worker)
{
	worker->repid = workerData->repid;
	worker->pid = workerData->pid;
	strncpy(worker->sub_name, );
	worker->pub_name
	worker->source_groupid = workerData->source_groupid;
	worker->target_groupid = workerData->target_groupid;
	worker->rep_state = workerData->rep_state;
	
	worker->isValid = true;

	worker->source_node_name
	worker->source_node_port
	worker->target_node_name
	worker->target_node_port

	worker->PostedShardsHash = FDD_CreateHash("Posted Shard Cache", 32, sizeof(MoveShardCacheEntry));
	worker->enablePost = true;
	memset(worker->statInfo, 0, sizeof(RepStatInfo));
}


bool 
FDD_IsNewRepId(MoveWorker **workerArray, int repid)
{

}


HTAB *
FDD_CreateHash(const char *hashName, uint32 hashSize, uint32 entrySize)
{
	HASHCTL info;
	int hashFlags = 0;
	HTAB *shardHash = NULL;

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = entrySize;
	info.hash = tag_hash;
	info.hcxt = CurrentMemoryContext;
	hashFlags = (HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	shardHash = hash_create(hashName, hashName, &info, hashFlags);
	if (shardHash == NULL)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("could not initialize posted shard hash")));
	}

	return shardHash;
}


/* 死循环处理事务：新增任务、状态监控 */
void FDD_MoveShardWorker_Loop()
{
	long wait_time = 0;
	int rc = 0;

	for (;;)
	{
	
		wait_time = 1000L;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_POSTMASTER_DEATH,
							   fd, wait_time,
							   WAIT_EVENT_LOGICAL_APPLY_MAIN);
		
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
		
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
		}
		
		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
		
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			
			//处理新增任务
			FDD_MoveShardWorker_DoWork();
		}
		
		if (rc & WL_TIMEOUT)
		{
			//执行监控流程
			FDD_MoveShardWorker_DoMonitor();
		}
	}
}


/* 调度进程初始化 */
void 
FDD_MoveShardWorker_Attach(int slot)
{
	if (slot < 0 || slot > 9)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("Invalid slot %d", slot)));
	}
	
	SpinLockAcquire(&DistMoveCtx.relmutex);
	
	MyMoveWorker = &DistMoveCtx->tasker_slot[slot];
	if (!MyMoveWorker->in_use)
	{
		SpinLockRelease(&DistMoveCtx.relmutex)
	}

	if (MyMoveWorker->proc != NULL)
	{
		SpinLockRelease(&DistMoveCtx.relmutex)
	}

	MyMoveWorker->proc = MyProc;
	MyMoveWorker->start_time = GetCurrentTimestamp();
	MyMoveWorker->tasker_type = TASKER_SCHEDULE;

	before_shmem_exit(FDD_MoveShardWorker_OnExit, (Datum) 0);
	SpinLockRelease(&DistMoveCtx.relmutex)
}


/* 调度进程退出处理 */
void 
FDD_MoveShardWorker_OnExit(int code, Datum arg)
{
	/* Disconnect gracefully from the remote side. */
	if (wrconn)
		walrcv_disconnect(wrconn);

	FDD_MoveShardWorker_Detach();

	ApplyLauncherWakeup();

}


/* 调度进程退出时的处理 */
void 
FDD_MoveShardWorker_Detach()
{
	SpinLockAcquire(&DistMoveCtx.relmutex);

	FDD_MoveShardWorker_CleanUp();
	
	SpinLockRelease(&DistMoveCtx.relmutex)
}


/* 重置调度进程参数 */
void
FDD_MoveShardWorker_CleanUp()
{
	MyMoveWorker->proc = NULL;
	MyMoveWorker->in_use = false;
	MyMoveWorker->is_ready = false;
}


/* 处理 SIGNUP 信号 */
void 
FDD_MoveShardWorker_WakeUp()
{
	int save_errno = errno;

	got_SIGHUP = true;

	/* Waken anything waiting on the process latch */
	SetLatch(MyLatch);

	errno = save_errno;
}


/* 处理新增任务 */
void 
FDD_MoveShardWorker_DoWork()
{
	bool found = false;
	
	SpinLockAcquire(&DistMoveCtx.relmutex);

	for (int i = 1; i <= 9; ++i)
	{
		DistMoveTasker *tasker = &DistMoveCtx.tasker_slot[i];
		if (!tasker->in_use)
		{
			continue;
		}

		if (tasker->tasker_type != TASKER_NEW || !tasker->is_ready)
		{
			continue;
		}
		
		//将表数据插入全局缓存 g_TaskMoveCacheList
		HeapTuple taskTuple = FDD_LookupMoveTaskTuple(tasker->taskinfo.taskname, false);
		FormData_fdd_dist_move_task moveTask = FDD_TupleToMoveTask(taskTuple);
		TaskMoveCacheEntry *taskEntry = FDD_FindMoveTaskEntry(moveTask.taskid, &found);
		if (found)
		{
			if (taskEntry->isValid)
			{
				//报错
			}
			//重置数据
		}
		taskEntry->check_pattern = moveTask.check_pattern;
		taskEntry->drop_method = moveTask.drop_method;
		taskEntry->lock_mode = moveTask.lock_mode;
		taskEntry->shard_state = moveTask.shard_state;
		taskEntry->taskid = moveTask.taskid;
		taskEntry->threshold = 0;
		taskEntry->isValid = true;
		taskEntry->MoveShardPostingHashLength = 0;
		taskEntry->MoveShardPostingHash = FDD_CreateHash("Posting Shard Cache", 32, sizeof(ShardPostUnitEntry));
		taskEntry->MoveShardWaitingList = NIL;
		
		//构造数据保存到 WaitingList
		ShardPostUnitEntry *postEntry = (ShardPostUnitEntry *)palloc(sizeof(ShardPostUnitEntry));
		postEntry->anchorShardId = tasker->taskinfo.shardid;
		postEntry->anchorTaskId = tasker->taskid;
		postEntry->anchorRelid = RelationIdForShard(tasker->taskinfo.shardid);
		postEntry->anchorShard_state = moveTask.shard_state;
		postEntry->shardIndex = 0;
		postEntry->SortedColShardArray = FDD_GetAllShardsArray(tasker->taskinfo.shardid, &postEntry.SortedColShardArrayLength);
		postEntry->anchorRepId = postEntry.SortedColShardArray[0].repid;
		
		taskEntry->MoveShardWaitingList = lappend(taskEntry->MoveShardWaitingList, postEntry);

		//遍历在表中查出 SortedColShardArray 所有数据, 插入全局缓存 g_MoveShardCacheHash
		MoveScheduleCacheEntry *scheduleEntry = NULL;
		MoveShardCacheEntry *assignShardEntry = NULL;
		
		for (int j = 0; j < postEntry.SortedColShardArrayLength; ++j)
		{
			HeapTuple shardTuple = FDD_LookupMoveShardTuple(postEntry.SortedColShardArray[j].shardid, tasker->taskid, false);
			FormData_fdd_dist_move_shard moveShard = FDD_TupleToMoveShard(shardTuple);
			MoveShardCacheEntry *shardEntry = FDD_FindMoveShardEntry(moveShard.shardid, HASH_ENTER, &found);
			if (found)
			{
				if (shardEntry->isValid)
				{
					//报错
				}
				//重置数据
			}
			
			shardEntry->assign_shardid = moveShard.assign_shardid;
			shardEntry->groupid = moveShard.groupid;
			shardEntry->repid = moveShard.repid;
			shardEntry->shardid = moveShard.shardid;
			shardEntry->shard_size = moveShard.shard_size;
			shardEntry->stage_time = moveShard.state_begin_time;
			shardEntry->shard_state = moveShard.shard_state;
			shardEntry->tablename = moveShard.tablename;
			shardEntry->taskid = moveShard.taskid;

			//与订阅端通信获取分片表id
			shardEntry->relationId = InvalidOid;

			//将表数据插入全局缓存 g_MoveScheduleCacheHash
			HeapTuple workerTuple = FDD_LookupMoveWorkerTuple(postEntry.SortedColShardArray[j].repid, tasker->taskid, false);
			TupleDesc tupleDescriptor = RelationGetDescr(fddDistMoveShard);
			FormData_fdd_dist_move_worker moveWorker = FDD_TupleToMoveWorker(workerTuple, tupleDescriptor);

			//找到主分片数据
			if (assignShardEntry == NULL && shardEntry->shardid == shardEntry->assign_shardid)
			{
				assignShardEntry = shardEntry;
				
				scheduleEntry = FDD_FindMoveSchedulaEntry(assignShardEntry->repid, HASH_ENTER, &found);
				if (found)
				{
					//报错
				}
				scheduleEntry->anchorRepId = moveWorker.repid;
				scheduleEntry->rep_state = moveWorker.rep_state;
				scheduleEntry->claimedExclusively = false;
				scheduleEntry->source_groupid = moveWorker.source_groupid;
				scheduleEntry->dst_groupid = moveWorker.target_groupid;
				scheduleEntry->idl_num = 0;
				scheduleEntry->WorkerNum = 0;
			}

			//若 repid 在 MoveWorkerArray 中不存在, 则构造一个 MoveWorker
			if (FDD_IsNewRepId(scheduleEntry->MoveWorkerArray, postEntry.SortedColShardArray[j].repid))
			{
				FDD_ConstructMoveWorker(&moveWorker, &scheduleEntry->MoveWorkerArray[scheduleEntry->WorkerNum]);
				++scheduleEntry->WorkerNum;
			}
		}
		scheduleEntry->idl_num = scheduleEntry->WorkerNum;
		
		//调用算法对 postEntry->SortedColShardArray 进行排序
		

		//进入状态机
		FDD_MoveShardWorker_StateMachine(assignShardEntry, false);

		tasker->tasker_type = TASKER_SCHEDULE;
	}

	SpinLockRelease(&DistMoveCtx.relmutex);
}


/* 监控在线迁移状态 */
void 
FDD_MoveShardWorker_DoMonitor()
{

}


/* 状态机 */
void 
FDD_MoveShardWorker_StateMachine(MoveShardCacheEntry *shardEntry, bool isRestart)
{
	ShardMoveStatus state = shardEntry->shard_state / 100 * 100;
	switch(state)
	{
		case SHARD_WAITE_POST:
		{
			bool found = false;
			TaskMoveCacheEntry *taskEntry = FDD_FindMoveTaskEntry(shardEntry->taskid, &found);
			if (!found)
			{
				if (!isRestart)
				{
					//报错
				}
				//初始化并填充所有全局缓存
			}
			
			MoveScheduleCacheEntry *scheduleEntry = FDD_FindMoveSchedulaEntry(shardEntry->repid, HASH_FIND, &found);
			if (!found)
			{
				if (!isRestart)
				{
					//报错
				}
				//初始化并填充所有全局缓存
			}

			//查看是否有空闲的复制槽
			if (scheduleEntry->idl_num == 0)
			{
				break;
			}
			
			//查看当前分片是否可以投递
			int index = 0;
			ShardPostUnitEntry *postEntry = FDD_GetIndexInWaitingList(taskEntry->MoveShardWaitingList, shardEntry->shardid, &index);
			if (index != 1)
			{
				break;
			}

			//开始投递, 从等待链表中移出, 添加到投递HASH表中
			--scheduleEntry->idl_num;
			list_delete_first(taskEntry->MoveShardWaitingList);

			//与发布端、订阅端通信, 刷新发布表, 开始逻辑复制

			//构造 MoveWorker 数据
			scheduleEntry->MoveWorkerArray[scheduleEntry->WorkerNum] = ;
			++scheduleEntry->WorkerNum;

			//状态跳转
			shardEntry->shard_state = SHARD_REP_INIT;
			
			//将数据落盘到自定义表中
			
			break;
		}
		case SHARD_REP_INIT:
		case SHARD_REP_DATASYNC:
		case SHARD_REP_SYNCDONE:
		{
			if (!isRestart)
			{
				break;
			}
			
			shardEntry->shard_state = SHARD_WAITE_POST;
			FDD_MoveShardWorker_StateMachine(shardEntry, true);
			break;
		}
		case SHARD_REP_READY:
		{
			if (isRestart)
			{

			}
			break;
		}
		case SHARD_MOVE_END:
		{
			break;
		}
		case SHARD_DELETE_OLD:
		{
			break;
		}
		default:
		{
			break;
		}
	}
}


void 
FDD_MoveShardWorker_DoPost()
{

}
