#include "postgres.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/snapmgr.h"
#include "access/htup_details.h"


#include "utils/fmgroids.h"
#include "catalog/pg_subscription.h"
#include "lib/stringinfo.h"
#include "distributed/connection_management.h"
#include "catalog/pg_subscription_rel.h"
#include "replication/worker_internal.h"
#include "distributed/metadata_cache.h"
#include "distributed/remote_commands.h"
#include "distributed/worker_protocol.h"

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
typedef struct sub_status
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
typedef struct RepStatInfo
{
	int pub_fail_count;
	int sub_fail_count;
	int pub_normal_count;
	int sub_normal_count;
}RepStatInfo;

typedef struct MoveWorker
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
typedef struct SubscriptionStat
{
    int         sub_pid;           // 订阅者逻辑复制进程pid：进程未启动或终止时此属性为默认值0
	int			start_count;       // 逻辑复制进程启动次数
	bool		abnormal_check;    // 是否启动异常检测
	int			abnormal_count;    // 进程确认异常次数
    int         normal_count;      // 进程正常次数
}SubscriptionStat;

typedef struct MoveShardCacheEntry
{
	int shardid;            // 待移动的分片ID
	int groupid;            // 待迁移分片表所处的组id
	int repid;              // 待迁移分片所分配的复制任务处理子进程序号
	Oid tablename;          // 待迁移分片的表名（此行对应的分布式表。该值引用pg_class系统目录表中的relfilenode列）
	int taskid;             // 在线迁移任务id
	int64 assign_shardid;   // 是由哪个分片因为亲和关系，触发当前分片表进行迁移的，若当前值为shardid，则该行为分片迁移指定的分片
	int64 shard_size;       // 分片初始大小（以字节为单位）
	int stage_time;         // 当前状态开始的时间
	int shard_state;        // 该分片当前处于哪个迁移阶段
    
	// 以下结构不会存到表中，只存在内存中
	bool isValid;           // 此条目是否收到缓存效通知，是否需要重构缓存
	Oid relationId;         // 此分片表在订阅端上的oid
	SubscriptionStat subInfo;  // 新增成员变量 : 逻辑复制进程的监控数据
}MoveShardCacheEntry;

void FDD_GetSlotStateCount(sub_status *subData, int *startupCount, int *catchupCount, int *doneCount);
void FDD_GetRelStateErrCount(sub_status *subData, int *errCount, bool *isAppErr);
void FDD_DisableShardStateMonitor(MoveWorker *worker);
MoveShardCacheEntry *FDD_GetShardHashEntry(MoveWorker *worker, Oid relationId, bool missOk);
void FDD_UpdateRelState(MoveShardCacheEntry *shardEntry, char relState);
ShardMoveStatus FDD_TransformRelState(char relState);
void FDD_ShardStateMonitor(sub_status *stat_data, MoveShardCacheEntry *shardRec, bool *is_abnormal);
void FDD_UpdateRepStatus(MoveWorker *worker, int startupCount, int catchupCount, int doneCount);
int FDD_GetRepErrCode(MoveWorker *worker, int totalShardCount, int errShardCount, bool isAppErr);
void FDD_RepStateMonitor(MoveWorker *worker, bool pubConnNormal, bool subConnNormal, bool isShardAbnormal, int repErrCode);
bool FDD_IsShardRepErr(int state);
bool FDD_IsRepStop(int state);
HTAB *CreatePostedShardHash(MemoryContext memoryContext);


/*
 * FDD_LogicalReplicationMonitor 逻辑复制状态监控
 * 与发布端通信, 并从订阅端采集数据
 * 完成 rep_state 与 shard_state 刷新, 相关告警与清告警
 */
void FDD_LogicalReplicationMonitor(MoveWorker *worker)
{
	List *subStatusList = NIL;
	ListCell *subStatCell = NULL;
	
	int startupCount = 0;
	int catchupCount = 0;
	int doneCount = 0;
	
	int repErrCode = 0;
	int totalShardCount = 0;
	int errCount = 0;
	bool isAppErr = false;
	
	bool isShardAbnormal = false;
	
	bool pubConnNormal = true;  // 默认与源端通信正常
	bool subConnNormal = false;
	bool enableCheck = false;
	
	// 1. 获取订阅端非'r'表信息到 subStatusList
	if (subStatusList != NIL && list_length(subStatusList) != 0)
	{
		subConnNormal = true;
	}
	
	// 若获取不到订阅端信息, 则不触发分片复制状态监控, 只进行复制任务处理子进程监控
	foreach (subStatCell, subStatusList)
	{
		sub_status *subData = lfirst(subStatCell);
		MoveShardCacheEntry *shardEntry = NULL;
		
		// 统计分片总数(需包含app进程)、各复制阶段数(不包含app进程)、出现异常的分片数(需包含app进程)
		++totalShardCount;
		FDD_GetSlotStateCount(subData, &startupCount, &catchupCount, &doneCount);
		FDD_GetRelStateErrCount(subData, &errCount, &isAppErr);
		
		// 若逻辑复制被关闭, 则直接退出
		if (!enableCheck && !subData->sub_enabled)
		{
			if (!FDD_IsRepStop(worker->rep_state))
			{
				// 报警：逻辑复制关闭
				worker->rep_state = worker->rep_state%100 + MANUAL_INTERRUPT;
				FDD_DisableShardStateMonitor(worker);
			
				// 此时不支持分片投递
				worker->enablePost = false;
			}
			return;
		}
		
		enableCheck = true;
		
		shardEntry = FDD_GetShardHashEntry(worker, subData->relid, true);
		if (shardEntry == NULL)
		{
			// 不允许找不到，报异常
		}
		
		// 更新分片复制状态
		FDD_UpdateRelState(shardEntry, subData->subrel_satus);
		
		// 更新分片的逻辑复制错误码
		FDD_ShardStateMonitor(subData, shardEntry, &isShardAbnormal);
	}
	
	if (isShardAbnormal || !subConnNormal)
	{
		// 与发布端进行通信结果设置到 pubConnNormal
	}
	
	if (FDD_IsRepStop(worker->rep_state) && enableCheck)
	{
		// 清告警：手动关闭故障恢复
		worker->rep_state = worker->rep_state % 100;
	}
	
	// 更新复制任务处理子进程所处阶段
	FDD_UpdateRepStatus(worker, startupCount, catchupCount, doneCount);
	
	// 获取复制任务处理子进程的逻辑复制错误码
	repErrCode = FDD_GetRepErrCode(worker, totalShardCount, errCount, isAppErr);
	
	// 更新复制任务处理子进程的逻辑复制错误码
	FDD_RepStateMonitor(worker, pubConnNormal, subConnNormal, isShardAbnormal, repErrCode);
	
	// 若复制任务处理子进程状态异常 或 与订阅端通信异常 或 某分片复制数据异常, 都将影响分片投递
	if (worker->rep_state / 100 != 0 || !subConnNormal || isShardAbnormal)
	{
		worker->enablePost = false;
	}
	else
	{
		worker->enablePost = true;
	}
}


/*
 * FDD_GetSlotStateCount 根据订阅端获取的逻辑复制进程状态, 统计相关状态的进程数目
 */
void FDD_GetSlotStateCount(sub_status *subData, int *startupCount, int *catchupCount, int *doneCount)
{
	switch (subData->slot_pstatus)
	{
		case '\0':
		{
			if (subData->relid != 0)
			{
				++(*startupCount);
			}
			break;
		}
		case 'd':
		case 'w':
		{
			++(*startupCount);
			break;
		}
		case 'c': 
		{
			++(*catchupCount);
			break;
		}
		case 's': 
		{
			++(*doneCount);
			break;
		}
		default : 
		{
			break;
		}
	}	
}


/*
 * FDD_GetRelStateErrCount 根据订阅端获取的逻辑复制进程数据, 统计状态异常的进程数目
 */
void FDD_GetRelStateErrCount(sub_status *subData, int *errCount, bool *isAppErr)
{
	if (subData->slot_pid != 0)
	{
		return;
	}
	
	if (subData->relid == 0)
	{
		++(*errCount);
		*isAppErr = true;
	}
	else if (subData->subrel_satus != 's' && subData->subrel_satus != 'r')
	{
		++(*errCount);		
	}
}


/*
 * FDD_DisableShardStateMonitor 逻辑复制关闭时的相关处理
 * 若分片状态正常, 则应清空相关分片监控数据
 * 状态异常时, 需保留异常数据. 若逻辑复制重启时异常已解决, 也需满足恢复流程检测
 */
void FDD_DisableShardStateMonitor(MoveWorker *worker)
{
	HASH_SEQ_STATUS status;
	MoveShardCacheEntry *shardEntry = NULL;
	
	hash_seq_init(&status, worker->PostedShardsHash);
	
	shardEntry = (MoveShardCacheEntry *)hash_seq_search(&status);
	while (shardEntry != NULL)
	{
		if (!FDD_IsShardRepErr(shardEntry->shard_state))
		{
			// 保留异常检查标记
			shardEntry->subInfo.sub_pid = 0;
			shardEntry->subInfo.start_count = 0;
			shardEntry->subInfo.abnormal_count = 0;
	        shardEntry->subInfo.normal_count = 0;
		}
	}
}


/* 
 * FDD_GetShardHashEntry 查找指定分片元素
 * 根据relationId, 从PostedShardsHash中找到对应的分片元素 
 */
MoveShardCacheEntry *FDD_GetShardHashEntry(MoveWorker *worker, Oid relationId, bool missOk)
{
	MoveShardCacheEntry *shardEntry = NULL;
	void *hashKey = (void *) &relationId;
	bool foundInCache = false;

	shardEntry = hash_search(worker->PostedShardsHash, hashKey, HASH_FIND, &foundInCache);
	if (!missOk && !foundInCache)
	{
		return NULL;
	}
	return shardEntry;
}


/*
 * FDD_UpdateRelState 更新分片复制状态
 */
void FDD_UpdateRelState(MoveShardCacheEntry *shardEntry, char relState)
{
	int errCode = shardEntry->shard_state / 100 * 100;
	
	shardEntry->shard_state = FDD_TransformRelState(relState) + errCode;
}


/*
 * FDD_TransformRelState 将分片复制状态转换为ShardMoveStatus枚举值
 */
ShardMoveStatus FDD_TransformRelState(char relState)
{
	ShardMoveStatus sub_state = 0;
	
	switch(relState)
	{
		case 'i': 
		{
			sub_state = SHARD_REP_INIT;
			break;
		}
		case 'd': 
		{
			sub_state = SHARD_REP_DATASYNC;
			break;
		}
		case 's': 
		{
			sub_state = SHARD_REP_SYNCDONE;
			break;
		}
		case 'r': 
		{
			sub_state = SHARD_REP_READY;
			break;
		}
		default : 
		{
			break;
		}
	}
	
	return sub_state;
}


/* 
 * FDD_ShardStateMonitor 监控分片的逻辑复制状态(app 与 sync进程状态)
 * 将从订阅端查询的分片逻辑复制数据, 与HASH缓存中的MoveShardCacheEntry数据相比较
 * 刷新 shard_state, 进行相关告警与清告警
 * 调用者自行保障入参合法性
 * 上层框架决定了 stat_data->slot_pid 为0 只有两种情况 : 
 * 1. 逻辑复制完毕, sync进程正常退出
 * 2. 逻辑复制进程异常
 */
void FDD_ShardStateMonitor(sub_status *stat_data, MoveShardCacheEntry *shardRec, bool *is_abnormal)
{
	// 外部已经保障入参合法性, 在此不做校验
	
	int caseMask = ((stat_data->slot_pid == shardRec->subInfo.sub_pid) << 2) | 
					((shardRec->subInfo.sub_pid == 0) << 1) | 
					(stat_data->slot_pid == 0);
	
	switch(caseMask)
	{
		case 4:
		{
			if (!shardRec->subInfo.abnormal_check && !FDD_IsShardRepErr(shardRec->shard_state))
			{
				return;
			}
			
			++shardRec->subInfo.normal_count;
			if (shardRec->subInfo.normal_count >= STATE_RECOVER_THRESHOLD)
			{
				if (FDD_IsShardRepErr(shardRec->shard_state))
				{
					// 清告警：逻辑复制故障恢复
					shardRec->shard_state %= 100;
				}
				
				// 重置正常计数
				shardRec->subInfo.normal_count = 0;
				
				// 清除异常计数
				shardRec->subInfo.abnormal_count = 0;
				shardRec->subInfo.abnormal_check = false;
			}
			break;
		}
		case 7:
		{
			*is_abnormal = true;
			
			if (FDD_IsShardRepErr(shardRec->shard_state))
			{
				return;
			}
					
			shardRec->subInfo.abnormal_check = true;
			++shardRec->subInfo.abnormal_count;
			
			if (shardRec->subInfo.abnormal_count >= STATE_FAIL_THRESHOLD)
			{
				// 报警：逻辑复制故障
				shardRec->shard_state = shardRec->shard_state % 100 + CONFLICT;
				shardRec->subInfo.abnormal_check = false;
			}
			break;
		}
		case 2:
		{
			shardRec->subInfo.sub_pid = stat_data->slot_pid;
			
			if (FDD_IsShardRepErr(shardRec->shard_state))
			{
				*is_abnormal = true;
				return;
			}
			
			++shardRec->subInfo.start_count;
			if (shardRec->subInfo.start_count - 1 >= STATE_FAIL_THRESHOLD)
			{
				// 报警：逻辑复制故障
				shardRec->shard_state = shardRec->shard_state % 100 + CONFLICT;
				shardRec->subInfo.abnormal_check = false;
			}
			else if (shardRec->subInfo.start_count > 1)
			{
				*is_abnormal = true;
			}
			break;
		}
		case 1:
		{
			shardRec->subInfo.sub_pid = stat_data->slot_pid;
			shardRec->subInfo.normal_count = 0;
			
			// app 进程 subrel_satus 为空
			if (stat_data->subrel_satus == 's' || stat_data->subrel_satus == 'r')
			{
				// 清除异常计数
				shardRec->subInfo.normal_count = 0;
				shardRec->subInfo.abnormal_count = 0;
				shardRec->subInfo.abnormal_check = false;	
			}
			else
			{
				*is_abnormal = true;
				++shardRec->subInfo.abnormal_count;
				
				if (shardRec->subInfo.abnormal_count >= STATE_FAIL_THRESHOLD)
				{
					// 报警：逻辑复制故障
					shardRec->shard_state = shardRec->shard_state % 100 + CONFLICT;
					shardRec->subInfo.abnormal_check = false;
				}
				else
				{
					shardRec->subInfo.abnormal_check = true;
				}
			}
			break;
		}
		case 0:
		{
			shardRec->subInfo.sub_pid = stat_data->slot_pid;
			shardRec->subInfo.normal_count = 0;
			*is_abnormal = true;
			
			if (FDD_IsShardRepErr(shardRec->shard_state))
			{
				return;
			}
			
			shardRec->subInfo.abnormal_check = true;
			++shardRec->subInfo.start_count;
			
			if (shardRec->subInfo.start_count - 1 >= STATE_FAIL_THRESHOLD)
			{
				// 报警：逻辑复制故障
				shardRec->shard_state = shardRec->shard_state % 100 + CONFLICT;
				shardRec->subInfo.abnormal_check = false;
			}
			break;
		}
		default: 
		{
			break;
		}
	}
}


/* 
 * FDD_GetRepState 判断复制任务子进程逻辑复制状态
 * 根据统计到的当前各状态进程数, 识别所处逻辑复制阶段
 * REP_STARTUP ：所有进程处于 'd' / 'w' / '\0'
 * REP_CATCHUP ：所有进程处于 'c'
 * REP_STREAMING ：所有进程处于 's'
 * REP_PART_CATCHUP ：所有进程处于 'c' 与 'd' / 'w' / '\0'
 * REP_PART_STREAMING ：部分进程处于 's'
 */
void FDD_UpdateRepStatus(MoveWorker *worker, int startupCount, int catchupCount, int doneCount)
{
	int errCode = worker->rep_state / 100 * 100;
	int repStatus = worker->rep_state % 100;
	int countMask = (startupCount > 0) | ((catchupCount > 0 ) << 1) | ((doneCount > 0) << 2);
	
	switch(countMask)
	{
		case 1:
		{
			repStatus = REP_STARTUP;
			break;
		}
		case 2:
		{
			repStatus = REP_CATCHUP;
			break;
		}
		case 3:
		{
			repStatus = REP_PART_CATCHUP;
			break;
		}
		case 4:
		{
			repStatus = REP_STREAMING;
			break;
		}
		case 5:
		case 6:
		case 7:
		{
			repStatus = REP_PART_STREAMING;
			break;
		}
		default:
		{
			break;
		}
	}
	
	worker->rep_state = repStatus + errCode;	
}


/*
 * FDD_GetRepErrCode 根据统计到的当前异常状态进程信息, 返回逻辑复制错误码
 * REP_APP_ERR ：app进程非正常退出
 * REP_APP_SYNC_ERR ：app和某些sync进程非正常退出
 * REP_FETAL_ERR ：app和所有sync进程全部非正常退出
 * REP_SYNC_ERR ：某些sync进程非正常退出
 */
int FDD_GetRepErrCode(MoveWorker *worker, int totalShardCount, int errShardCount, bool isAppErr)
{
	int errCode = 0;
	
	if (0 == errShardCount)
	{
		errCode = worker->rep_state / 100 * 100;
	}
	else if (totalShardCount == errShardCount)
	{
		errCode = REP_FETAL_ERR;
	}
	else if (isAppErr)
	{
		errCode = (1 == errShardCount ? REP_APP_ERR : REP_APP_SYNC_ERR);
	}
	else
	{
		errCode = REP_SYNC_ERR;
	}
	return errCode;
}


/* 
 * FDD_RepStateMonitor 监控复制任务子进程状态, 刷新 rep_state, 进行相关告警与清告警
 * 异常优先级依次递减 :
 * 1. DST_CONNECT_FAIL
 * 2. SRC_CONNECT_FAIL 
 * 3. REP_APP_ERR  REP_APP_SYNC_ERR  REP_FETAL_ERR  REP_SYNC_ERR
 */
void FDD_RepStateMonitor(MoveWorker *worker, bool pubConnNormal, bool subConnNormal, bool isShardAbnormal, int repErrCode)
{
	int connStateMask = subConnNormal << 1 | pubConnNormal;
	int curErrCode = worker->rep_state / 100 * 100;
	int errStatMask = 0;
	
	switch (connStateMask)
	{
		case 0:
		{
			worker->statInfo.sub_normal_count = 0;
			worker->statInfo.pub_normal_count = 0;
			
			++worker->statInfo.sub_fail_count;			
			++worker->statInfo.pub_fail_count;
			break;
		}
		case 1:
		{
			worker->statInfo.sub_normal_count = 0;
			++worker->statInfo.pub_normal_count;
			
			++worker->statInfo.sub_fail_count;
			break;
		}
		case 2:
		{
			// 仅当出现复制异常时, 才去判断源端通信状态
			if (isShardAbnormal)
			{
				worker->statInfo.pub_normal_count = 0;
				++worker->statInfo.pub_fail_count;
			}
			
			++worker->statInfo.sub_normal_count;
			break;
		}
		case 3:
		{
			++worker->statInfo.pub_normal_count;
			++worker->statInfo.sub_normal_count;
			
			worker->statInfo.sub_fail_count = 0;
			worker->statInfo.pub_fail_count = 0;
			
			break;
		}
		default:
		{
			break;
		}		
	}
	
	errStatMask = (curErrCode != DST_CONNECT_FAIL && worker->statInfo.sub_fail_count >= STATE_FAIL_THRESHOLD) | 
	((curErrCode != SRC_CONNECT_FAIL && worker->statInfo.pub_fail_count >= STATE_FAIL_THRESHOLD) << 1) | 
	((curErrCode == DST_CONNECT_FAIL && worker->statInfo.sub_normal_count >= STATE_RECOVER_THRESHOLD) << 2) | 
	((curErrCode == SRC_CONNECT_FAIL && worker->statInfo.pub_normal_count >= STATE_RECOVER_THRESHOLD) << 3);
	
	switch (errStatMask)
	{
		case 1:
		{
			curErrCode = DST_CONNECT_FAIL;
			break;
		}
		case 2:
		{
			curErrCode = SRC_CONNECT_FAIL;
			break;
		}
		case 4:
		{
			// 清告警：目的端通信故障恢复
			curErrCode = repErrCode;
			break;
		}
		case 8:
		{
			// 清告警：源端通信故障恢复
			curErrCode = repErrCode;
			break;
		}
		default :
		{
			break;
		}
	}
	
	worker->rep_state = worker->rep_state % 100 + curErrCode;
}


/* 根据入参状态, 判断分片复制状态是否为异常态 */
bool FDD_IsShardRepErr(int state)
{
	if (state/100 == CONFLICT/100)
	{
		return true;
	}
	return false;
}

/* 根据入参状态, 判断分片复制状态是否为异常态 */
bool FDD_IsRepStop(int state)
{
	if (state/100 == MANUAL_INTERRUPT/100)
	{
		return true;
	}
	return false;
}


/* CreatePostedShardHash 初始化HASH表 */
HTAB *CreatePostedShardHash(MemoryContext memoryContext)
{
	HTAB *postedShardHash = NULL;
	int hashFlags = 0;
	HASHCTL info;

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(uint64);
	info.entrysize = sizeof(MoveShardCacheEntry);
	info.hcxt = memoryContext;
	hashFlags = (HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

	postedShardHash = hash_create("Posted Shard Data Hash", 128, &info, hashFlags);

	return postedShardHash;
}