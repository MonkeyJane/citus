## 1、远端事务

cn连接worker节点，维护一个连接相关的数据结构：

```c
typedef struct MultiConnection
{
	......
	/* information about the associated remote transaction */
	RemoteTransaction remoteTransaction;
	/* membership in list of in-progress transactions */
	dlist_node transactionNode;
	......
} MultiConnection;

```

成员remoteTransaction管理连接在worker节点开启的事务（后续用翻译：远端事务）相关信息：

```c
typedef struct RemoteTransaction
{
	/* worker端事务的状态 */
	RemoteTransactionState transactionState;
	......
} RemoteTransaction;

```

RemoteTransactionState标记了远端事务的状态：

```c
typedef enum
{
	/* no transaction active */
	REMOTE_TRANS_INVALID = 0,

	/* transaction start */
	REMOTE_TRANS_STARTING,
	REMOTE_TRANS_STARTED,

	/* 2pc prepare */
	REMOTE_TRANS_PREPARING,
	REMOTE_TRANS_PREPARED,

	/* transaction abort */
	REMOTE_TRANS_1PC_ABORTING,
	REMOTE_TRANS_2PC_ABORTING,
	REMOTE_TRANS_ABORTED,

	/* transaction commit */
	REMOTE_TRANS_1PC_COMMITTING,
	REMOTE_TRANS_2PC_COMMITTING,
	REMOTE_TRANS_COMMITTED
} RemoteTransactionState;
```

### 1.1 开启cn事务

citus使用DistributedTransactionId结构来保存分布式事务信息：

```c
typedef struct DistributedTransactionId
{
	int initiatorNodeIdentifier; //开启事务的节点group id，集群中唯一
	bool transactionOriginator; //如果是cn初始化的，为true，在MX模式中，如果是MX节点开启的事务，为false，这个成员用来区分两者
	uint64 transactionNumber; //分布式事务的唯一事务id（只能保证某一个cn开启的唯一性，后面有说明）
	TimestampTz timestamp; //事务开始时间
} DistributedTransactionId;
```



开启远端事务的前提是cn子进程已经开启了事务，cn通过调用BeginOrContinueCoordinatedTransaction函数开启：

```c
void BeginOrContinueCoordinatedTransaction(void)
{
	if (CurrentCoordinatedTransactionState == COORD_TRANS_STARTED)
	{
		return;
	}

	BeginCoordinatedTransaction();
}

void BeginCoordinatedTransaction(void)
{
	if (CurrentCoordinatedTransactionState != COORD_TRANS_NONE &&
		CurrentCoordinatedTransactionState != COORD_TRANS_IDLE)
	{
		ereport(ERROR, (errmsg("starting transaction in wrong state")));
	}
	CurrentCoordinatedTransactionState = COORD_TRANS_STARTED;
	AssignDistributedTransactionId();
}
```

AssignDistributedTransactionId函数初始化了cn节点的事务信息：

```c
void AssignDistributedTransactionId(void)
{
    //1、从共享内存中获取nextTransactionNumber
	pg_atomic_uint64 *transactionNumberSequence =
		&backendManagementShmemData->nextTransactionNumber;
	//2、nextTransactionNumber递增，生成本次的事务ID
	uint64 nextTransactionNumber = pg_atomic_fetch_add_u64(transactionNumberSequence, 1);
    //3、保存子进程正在运行的事务信息
	int localGroupId = GetLocalGroupId();
	TimestampTz currentTimestamp = GetCurrentTimestamp();
	Oid userId = GetUserId();

	SpinLockAcquire(&MyBackendData->mutex);

	MyBackendData->databaseId = MyDatabaseId;
	MyBackendData->userId = userId;

	MyBackendData->transactionId.initiatorNodeIdentifier = localGroupId;
	MyBackendData->transactionId.transactionOriginator = true;
	MyBackendData->transactionId.transactionNumber = nextTransactionNumber;
	MyBackendData->transactionId.timestamp = currentTimestamp;

	MyBackendData->citusBackend.initiatorNodeIdentifier = localGroupId;
	MyBackendData->citusBackend.transactionOriginator = true;

	SpinLockRelease(&MyBackendData->mutex);
}
```

1、从共享内存中获取nextTransactionNumber，nextTransactionNumber原子递增，生成本次的事务ID

2、保存子进程正在运行的事务信息：
         节点group id
    	事务开始的时间戳
    	用户OID
    	数据库ID
    	事务ID



**注：事务ID只能保证某一个cn上产生的唯一性，不能保证多个cn之间的事务ID的唯一性。**

### 1.2 发送远端事务开启命令

citus通过调用StartRemoteTransactionBegin开启远端事务：

```c
/*
 * StartRemoteTransactionBeging initiates beginning the remote transaction in
 * a non-blocking manner. The function sends "BEGIN" followed by
 * assign_distributed_transaction_id() to assign the distributed transaction
 * id on the remote node.
 */
void StartRemoteTransactionBegin(struct MultiConnection *connection)
{
    //每个连接中都有一个RemoteTransaction成员
	RemoteTransaction *transaction = &connection->remoteTransaction;
	......
	Assert(transaction->transactionState == REMOTE_TRANS_INVALID);
	......
    //修改远端事务为starting状态
	transaction->transactionState = REMOTE_TRANS_STARTING;
	/*
	 * 发送BEGIN TRANSACTION到worker
	 * 事务隔离级别为READ COMMITTED
	 */
	appendStringInfoString(beginAndSetDistributedTransactionId,
						   "BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED;");

    /*通过GetCurrentDistributedTransactionId获取本进程（cn进程）事务信息，发送"SELECT 			
     *assign_distributed_transaction_id"命令给worker节点。
    */
	distributedTransactionId = GetCurrentDistributedTransactionId();
	timestamp = timestamptz_to_str(distributedTransactionId->timestamp);
	appendStringInfo(beginAndSetDistributedTransactionId,
					 "SELECT assign_distributed_transaction_id(%d, " UINT64_FORMAT
					 ", '%s');",
					 distributedTransactionId->initiatorNodeIdentifier,
					 distributedTransactionId->transactionNumber,
					 timestamp);

	/* append in-progress savepoints for this transaction */
	activeSubXacts = ActiveSubXacts();
	transaction->lastSuccessfulSubXact = TopSubTransactionId;
	transaction->lastQueuedSubXact = TopSubTransactionId;
	foreach(subIdCell, activeSubXacts)
	{
		SubTransactionId subId = lfirst_int(subIdCell);
		appendStringInfo(beginAndSetDistributedTransactionId,
						 "SAVEPOINT savepoint_%u;", subId);
		transaction->lastQueuedSubXact = subId;
	}

	if (!SendRemoteCommand(connection, beginAndSetDistributedTransactionId->data))
	{
		.....
	}
}

```



在这个函数中，cn发送相关事务开启的命令给worker节点（在一个包中顺序发送）：

1、"BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED;"，开启事务，事务隔离级别为”READ COMMITTED“。

2、从cn子进程信息（backendData）中获取cn事务信息，将cn节点group id（initiatorNodeIdentifier），事务id（transactionNumber），事务开始时间（timestamp）作为参数，发送调用UDF的命令到worker节点：

"SELECT assign_distributed_transaction_id(initiatorNodeIdentifier, transactionNumber, timestamp);”

3、如果cn包含子事务， worker还需要做保存点（以子事务ID命名）:

​	"SAVEPOINT savepoint_subid;"



### 1.3 worker开启事务

1、worker节点收到"BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED;"命令后， 在exec_simple_query，调用StartTransactionCommand

```c
void StartTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_DEFAULT:
			StartTransaction();
			s->blockState = TBLOCK_STARTED;
			break;
    }
    ......
}
```



在执行命令的时候，调用standard_ProcessUtility，进入BeginTransactionBlock

```c
void BeginTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;
	switch (s->blockState)
	{
			/*
			 * We are not inside a transaction block, so allow one to begin.
			 */
		case TBLOCK_STARTED:
			s->blockState = TBLOCK_BEGIN;
			break;
    }
    ......
}
```



2、worker收到"SELECT assign_distributed_transaction_id(initiatorNodeIdentifier, transactionNumber, timestamp);”命令后，调用citus的udf，assign_distributed_transaction_id

```c
Datum assign_distributed_transaction_id(PG_FUNCTION_ARGS)
{
	Oid userId = GetUserId();
	//1、获取cn发送过来的cn groupid、事务id和事务开始时间戳
	int32 initiatorNodeIdentifier = PG_GETARG_INT32(0);
	uint64 transactionNumber = PG_GETARG_INT64(1);
	TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(2);

	CheckCitusVersion(ERROR);

	/* MyBackendData should always be avaliable, just out of paranoia */
	if (!MyBackendData)
	{
		ereport(ERROR, (errmsg("backend is not ready for distributed transactions")));
	}

	/*
	 * Note that we don't need to lock shared memory (i.e., LockBackendSharedMemory()) here
	 * since this function is executed after AssignDistributedTransactionId() issued on the
	 * initiator node, which already takes the required lock to enforce the consistency.
	 */

	SpinLockAcquire(&MyBackendData->mutex);

	/*2、事务已经开始， 报错*/
	if (MyBackendData->transactionId.transactionNumber != 0)
	{
		SpinLockRelease(&MyBackendData->mutex);

		ereport(ERROR, (errmsg("the backend has already been assigned a "
							   "transaction id")));
	}
	//3、cn发送过来的cn groupid、事务id和事务开始时间戳
	MyBackendData->databaseId = MyDatabaseId;
	MyBackendData->userId = userId;

	MyBackendData->transactionId.initiatorNodeIdentifier = initiatorNodeIdentifier;
	MyBackendData->transactionId.transactionNumber = transactionNumber;
	MyBackendData->transactionId.timestamp = timestamp;
	MyBackendData->transactionId.transactionOriginator = false;

	MyBackendData->citusBackend.initiatorNodeIdentifier =
		MyBackendData->transactionId.initiatorNodeIdentifier;
	MyBackendData->citusBackend.transactionOriginator = false;

	SpinLockRelease(&MyBackendData->mutex);

	PG_RETURN_VOID();
}
```



远端事务的事务id和cn开启事务分配的事务id是一致的。

3、如果worker收到创建保存点的命令（savepoint），在standard_ProcessUtility中处理：

```c
void standard_ProcessUtility(PlannedStmt *pstmt,
						const char *queryString,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						char *completionTag)
{
	Node	   *parsetree = pstmt->utilityStmt;
	......
	switch (nodeTag(parsetree))
	{
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;
				switch (stmt->kind)
				{
					......
					case TRANS_STMT_SAVEPOINT:
						{
							ListCell   *cell;
							char	   *name = NULL;
							RequireTransactionChain(isTopLevel, "SAVEPOINT");
							foreach(cell, stmt->options)
							{
								DefElem    *elem = lfirst(cell);
								if (strcmp(elem->defname, "savepoint_name") == 0)
									name = strVal(elem->arg);
							}
							Assert(PointerIsValid(name));
							DefineSavepoint(name);
						}
						break;
                }
                ......
            }
            ......
    }
    ......
}
```



## 2、只读查询开启远端事务的场景

从附表1（StartRemoteTransactionBegin函数调用情况）可以看出，只读查询可能开启远端事务的操作有：

- RealTimeExecScan

- broadcast_intermediate_result

- RouterSelectExecScan

- CitusExplainScan



### 2.1 RealTimeExecScan

```c
TupleTableSlot *
RealTimeExecScan(CustomScanState *node)
{
	if (!scanState->finishedRemoteScan)
	{
        ......
		ExecuteSubPlans(distributedPlan);
		MultiRealTimeExecute(workerJob);
        ......
	}
}
```



#### 2.1.1  分布式执行计划包含子查询

如果执行计划包含子查询（暂时没有模拟出来场景），在ExecuteSubPlans中，有可能开启远端事务：

```c
void
ExecuteSubPlans(DistributedPlan *distributedPlan)
{
	if (subPlanList == NIL)
	{
		/* no subplans to execute */
		return;
	}

    //1、这里开启了cn事务
	BeginOrContinueCoordinatedTransaction();
    //2、获取可读worker节点
	nodeList = ActiveReadableNodeList();
	foreach(subPlanCell, subPlanList)
	{
        //3、CreateRemoteFileDestReceiver会开启远端事务
		copyDest = (DestReceiver *) CreateRemoteFileDestReceiver(resultId, estate,
																 nodeList,
																 writeLocalFile);
        ......
	}
    ......
    ExecutePlanIntoDestReceiver(plannedStmt, params, copyDest);
    ......
}

DestReceiver *CreateRemoteFileDestReceiver(char *resultId, EState *executorState,
							 List *initialNodeList, bool writeLocalFile)
{
    ......
	resultDest->pub.rStartup = RemoteFileDestReceiverStartup;
    ......
	return (DestReceiver *) resultDest;
}

```



在ExecutePlanIntoDestReceiver中，执行子计划，会调用RemoteFileDestReceiverStartup：

```c
static void RemoteFileDestReceiverStartup(DestReceiver *dest, int operation,
							  TupleDesc inputTupleDescriptor)
{
    ......
	foreach(initialNodeCell, initialNodeList)
	{
        ......
		WorkerNode *workerNode = (WorkerNode *) lfirst(initialNodeCell);
		char *nodeName = workerNode->workerName;
		int nodePort = workerNode->workerPort;
		MultiConnection *connection = NULL;
		connection = StartNonDataAccessConnection(nodeName, nodePort);
		ClaimConnectionExclusively(connection);
		MarkRemoteTransactionCritical(connection);
		......
		connectionList = lappend(connectionList, connection);
	}
	......
	FinishConnectionListEstablishment(connectionList);
	......
	/* 开启远端事务 */
	RemoteTransactionsBeginIfNecessary(connectionList);
	resultDest->connectionList = connectionList;
    ......
}
```

调用了RemoteTransactionsBeginIfNecessary来开始远端事务：

```c
void RemoteTransactionsBeginIfNecessary(List *connectionList)
{
	//1、在ExecuteSubPlans中，cn开启了事务
	if (!InCoordinatedTransaction())
	{
		return;
	}
	......
	/* 开启远端事务 */
	foreach(connectionCell, connectionList)
	{
		if (transaction->transactionState != REMOTE_TRANS_INVALID)
		{
			continue;
		}
		......
		StartRemoteTransactionBegin(connection);
	}
}
```



#### 2.2.2  MultiRealTimeExecute有可能开启远端事务

```c
void MultiRealTimeExecute(Job *job)
{
	List *taskList = job->taskList;
	workerNodeList = ActiveReadableNodeList();
	workerHash = WorkerHash(workerHashName, workerNodeList);
	......
	if (IsMultiStatementTransaction() && SelectOpensTransactionBlock)
	{
		BeginOrContinueCoordinatedTransaction();
	}
	......
	/* initialize task execution structures for remote execution */
	foreach(taskCell, taskList)
	{
		TaskExecution *taskExecution = NULL;
		taskExecution = InitTaskExecution(task, EXEC_TASK_CONNECT_START);
		taskExecutionList = lappend(taskExecutionList, taskExecution);
	}
	......
	/* loop around until all tasks complete, one task fails, or user cancels */
	while (!(allTasksCompleted || taskFailed || QueryCancelPending ||
			 sizeLimitIsExceeded))
	{
		......
		forboth(taskCell, taskList, taskExecutionCell, taskExecutionList)
		{
			workerNodeState = LookupWorkerForTask(workerHash, task, taskExecution);

			/* in case the task is about to start, throttle if necessary */
			if (TaskExecutionReadyToStart(taskExecution) &&
				(WorkerConnectionsExhausted(workerNodeState) ||
				 MasterConnectionsExhausted(workerHash)))
			{
				continue;
			}
			......
			/* call the function that performs the core task execution logic */
			connectAction = ManageTaskExecution(task, taskExecution, &executionStatus,
												&executionStats);
        }
        ......
}
```

这个函数在一定条件会开启cn事务：

```c
if (IsMultiStatementTransaction() && SelectOpensTransactionBlock)
{
	BeginOrContinueCoordinatedTransaction();
}
```

```c
bool IsMultiStatementTransaction(void)
{
	if (IsTransactionBlock())
	{
		/* in a BEGIN...END block */
		return true;
	}
	else if (StoredProcedureLevel > 0)
	{
		/* in (a transaction within) a stored procedure */
		return true;
	}
	else if (FunctionCallLevel > 0 && FunctionOpensTransactionBlock)
	{
		/* in a language-handler function call, open a transaction if configured to do so */
		return true;
	}
	else
	{
		return false;
	}
}
```

SelectOpensTransactionBlock是一个GUC参数，决定于postgresql.conf文件中的citus.select_opens_transaction_block配置项，值为'true'或者false，默认为‘true’。



在ManageTaskExecution（multi_real_time_executor.c文件中，该函数有两个，还有一个属于task traker执行器），可能会开启远端事务：

```c
static ConnectAction ManageTaskExecution(Task *task, TaskExecution *taskExecution,
					TaskExecutionStatus *executionStatus,
					DistributedExecutionStats *executionStats)
{

	TaskExecStatus currentStatus = taskStatusArray[currentIndex];
	switch (currentStatus)
	{	
         ......
         case EXEC_TASK_CONNECT_POLL:
		{
			int32 connectionId = connectionIdArray[currentIndex];
			ConnectStatus pollStatus = MultiClientConnectPoll(connectionId);

			/*
			 * If the connection is established, we change our state based on
			 * whether a coordinated transaction has been started.
			 */
			if (pollStatus == CLIENT_CONNECTION_READY)
			{
				if (InCoordinatedTransaction())
				{
					taskStatusArray[currentIndex] = EXEC_BEGIN_START;
				}
           	 }
            break;
        }
        ......
		case EXEC_BEGIN_START:
		{
			int32 connectionId = connectionIdArray[currentIndex];
			MultiConnection *connection = MultiClientGetConnection(connectionId);
			RemoteTransaction *transaction = &connection->remoteTransaction;

			if (transaction->transactionState == REMOTE_TRANS_INVALID)
			{
				StartRemoteTransactionBegin(connection);
				taskStatusArray[currentIndex] = EXEC_BEGIN_RUNNING;
				break;
			}
			else
			{
				taskStatusArray[currentIndex] = EXEC_COMPUTE_TASK_START;
				break;
			}
		}
         .......
	}
	return connectAction;
}

```

注：在realtime执行器执行计划的时候，TaskExecution针对每个连接维护一个状态机，如果状态变为EXEC_TASK_DONE或者出错，停止一个任务执行，全部任务都不执行，才停止MultiRealTimeExecute中的while循环。



### 2.2 broadcast_intermediate_result

broadcast_intermediate_result是一个udf函数，其调用方式为：

select * from broadcast_intermediate_result(result_id text, query text);



```c
Datum broadcast_intermediate_result(PG_FUNCTION_ARGS)
{
    .......
	//1、开启cn事务
	BeginOrContinueCoordinatedTransaction();
	//获取worker节点
	nodeList = ActivePrimaryNodeList();
	estate = CreateExecutorState();
    //创建远端文件接收器
	resultDest = (RemoteFileDestReceiver *) CreateRemoteFileDestReceiver(resultIdString,
																		 estate, nodeList,
																		 writeLocalFile);
	.......
	ExecuteQueryStringIntoDestReceiver(queryString, paramListInfo,
									   (DestReceiver *) resultDest);
    .......
}

```

在CreateRemoteFileDestReceiver函数，注册了相关回调函数：

```c
DestReceiver *
CreateRemoteFileDestReceiver(char *resultId, EState *executorState,
							 List *initialNodeList, bool writeLocalFile)
{
	RemoteFileDestReceiver *resultDest = NULL;
	resultDest = (RemoteFileDestReceiver *) palloc0(sizeof(RemoteFileDestReceiver));
    .......
	resultDest->pub.rStartup = RemoteFileDestReceiverStartup;
    .......
	return (DestReceiver *) resultDest;
}
```

ExecuteQueryStringIntoDestReceiver函数开始执行计划，会调用RemoteFileDestReceiverStartup：

```c
static void RemoteFileDestReceiverStartup(DestReceiver *dest, int operation,
							  TupleDesc inputTupleDescriptor)
{
	.......
	List *initialNodeList = resultDest->initialNodeList;

	.......
	foreach(initialNodeCell, initialNodeList)
	{
		WorkerNode *workerNode = (WorkerNode *) lfirst(initialNodeCell);
		connection = StartNonDataAccessConnection(nodeName, nodePort);
	}
	.......
	FinishConnectionListEstablishment(connectionList);
	.......
	/* must open transaction blocks to use intermediate results */
	RemoteTransactionsBeginIfNecessary(connectionList);
	.......
	resultDest->connectionList = connectionList;
}
```



RemoteFileDestReceiverStartup函数调用RemoteTransactionsBeginIfNecessary开启远端事务。



### 2.3 RouterSelectExecScan

当citus给select语句分配router执行器的时候（一般带分布键的时候），会调用RouterSelectExecScan。

```c
TupleTableSlot *RouterSelectExecScan(CustomScanState *node)
{
	CitusScanState *scanState = (CitusScanState *) node;
	TupleTableSlot *resultSlot = NULL;
	.......
	if (!scanState->finishedRemoteScan)
	{
		DistributedPlan *distributedPlan = scanState->distributedPlan;
		Job *workerJob = distributedPlan->workerJob;
		List *taskList = workerJob->taskList;

		/* we are taking locks on partitions of partitioned tables */
		LockPartitionsInRelationList(distributedPlan->relationIdList, AccessShareLock);
		.......
		ExecuteSubPlans(distributedPlan);

		if (list_length(taskList) > 0)
		{
            //1、如果执行计划的task有多个，会进入ExecuteSingleSelectTask
			Task *task = (Task *) linitial(taskList);

			ExecuteSingleSelectTask(scanState, task);
		}
		.......
		scanState->finishedRemoteScan = true;
	}
	.......
	resultSlot = ReturnTupleFromTuplestore(scanState);

	return resultSlot;
}
```

如果执行计划的task有多个，会进入ExecuteSingleSelectTask，这种情况一般在查询带的分布键键值分布在多个分片的时候会出现，如select *from test where id in (1, 201)：

```c
static void ExecuteSingleSelectTask(CitusScanState *scanState, Task *task)
{
	List *taskPlacementList = task->taskPlacementList;
    .......
	foreach(taskPlacementCell, taskPlacementList)
	{
		if (list_length(relationShardList) > 0)
		{
			placementAccessList = BuildPlacementSelectList(taskPlacement->groupId,
														   relationShardList);
			.......
			Assert(list_length(placementAccessList) == list_length(relationShardList));
		}
		else
		{
			/*
			 * When the SELECT prunes down to 0 shards, just use the dummy placement.
			 *
			 * FIXME: it would be preferable to evaluate the SELECT locally since no
			 * data from the workers is required.
			 */
			.......
			ShardPlacementAccess *placementAccess =
				CreatePlacementAccess(taskPlacement, PLACEMENT_ACCESS_SELECT);

			placementAccessList = list_make1(placementAccess);
		}
		.......
		if (placementAccessList == NIL)
		{
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							errmsg("a placement was moved after the SELECT was "
								   "planned")));
		}

		connection = GetPlacementListConnection(connectionFlags, placementAccessList,
												NULL);
		.......
		RemoteTransactionBeginIfNecessary(connection);	
	}
	.......
	ereport(ERROR, (errmsg("could not receive query results")));
}
```

这个过程调用了RemoteTransactionBeginIfNecessary函数，RouterSelectExecScan不会主动开启cn事务，如果cn事务没有开启，RouterSelectExecScan是不会开启远端事务的。有一种方法，现在'begin ... end'块中，通过其他语句触发cn事务开启，然后在执行触发RouterSelectExecScan函数的sql即可。

```c
void RemoteTransactionsBeginIfNecessary(List *connectionList)
{
	//1、在ExecuteSubPlans中，cn开启了事务
	if (!InCoordinatedTransaction())
	{
		return;
	}
	.......
	/* 开启远端事务 */
	foreach(connectionCell, connectionList)
	{
        .......
		if (transaction->transactionState != REMOTE_TRANS_INVALID)
		{
			continue;
		}
		.......
		StartRemoteTransactionBegin(connection);
	}
    .......
}
```



### 2.4 CitusExplainScan

在执行'explain sql'语句的时候，会触发调用CitusExplainScan 。

```c
void CitusExplainScan(CustomScanState *node, List *ancestors, struct ExplainState *es)
{
	CitusScanState *scanState = (CitusScanState *) node;
	DistributedPlan *distributedPlan = scanState->distributedPlan;
	.......
	if (!ExplainDistributedQueries)
	{
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str, "explain statements for distributed queries ");
		appendStringInfo(es->str, "are not enabled\n");
		return;
	}
    .......
	ExplainOpenGroup("Distributed Query", "Distributed Query", true, es);
	if (distributedPlan->subPlanList != NIL)
	{
		ExplainSubPlans(distributedPlan, es);
	}
    .......
	ExplainJob(distributedPlan->workerJob, es);
	ExplainCloseGroup("Distributed Query", "Distributed Query", true, es);
    .......
}
```

CitusExplainScan会调用ExplainJob：

```c
static void ExplainJob(Job *job, ExplainState *es)
{
	if (dependedJobCount > 0)
	{
		ExplainPropertyText("Tasks Shown", "None, not supported for re-partition "
										   "queries", es);
	}
	else if (ExplainAllTasks || taskCount <= 1)
	{
		ExplainPropertyText("Tasks Shown", "All", es);
	}
	else
	{
		StringInfo tasksShownText = makeStringInfo();
		appendStringInfo(tasksShownText, "One of %d", taskCount);
		ExplainPropertyText("Tasks Shown", tasksShownText->data, es);
	}

	if (dependedJobCount == 0)
	{
		ExplainOpenGroup("Tasks", "Tasks", false, es);
		ExplainTaskList(taskList, es);
		ExplainCloseGroup("Tasks", "Tasks", false, es);
	}
	ExplainCloseGroup("Job", "Job", true, es);
}
```



ExplainJob调用ExplainTaskList：

```c
static void ExplainTaskList(List *taskList, ExplainState *es)
{
	ListCell *taskCell = NULL;
	ListCell *remoteExplainCell = NULL;
	List *remoteExplainList = NIL;

	/* make sure that the output is consistent */
	taskList = SortList(taskList, CompareTasksByTaskId);

	foreach(taskCell, taskList)
	{
		Task *task = (Task *) lfirst(taskCell);
		RemoteExplainPlan *remoteExplain = NULL;

		remoteExplain = RemoteExplain(task, es);
		remoteExplainList = lappend(remoteExplainList, remoteExplain);
	}
}
```

在RemoteExplain中，会开启远端事务，这个过程，既开启了cn事务，也开启了远端事务：

```c
static RemoteExplainPlan *RemoteExplain(Task *task, ExplainState *es)
{

	BeginOrContinueCoordinatedTransaction();

	for (placementIndex = 0; placementIndex < placementCount; placementIndex++)
	{
		ShardPlacement *taskPlacement = list_nth(taskPlacementList, placementIndex);
		

		connection = GetPlacementConnection(connectionFlags, taskPlacement, NULL);

		/* try other placements if we fail to connect this one */
		if (PQstatus(connection->pgConn) != CONNECTION_OK)
		{
			continue;
		}

		RemoteTransactionBeginIfNecessary(connection);

	}

	return remotePlan;
}
```





## 3、特别说明

### 3.1 TaskTrackerExecScan

TaskTrackerExecScan不会开启远端事务，这个函数虽然也调用了ManageTaskExecution，不过tasktraker执行器有自己同名函数，参数不一样。

### 3.2 create_intermediate_result

create_intermediate_result是一个udf函数，其调用方式为：

select *from create_intermediate_result(result_id text, query text);



参考broadcast_intermediate_result，create_intermediate_result虽然也会调用RemoteFileDestReceiverStartup函数，不过因为不涉及到任何节点，所以不会开启远端事务。

```c
Datum create_intermediate_result(PG_FUNCTION_ARGS)
{
	text *resultIdText = PG_GETARG_TEXT_P(0);
	char *resultIdString = text_to_cstring(resultIdText);
	text *queryText = PG_GETARG_TEXT_P(1);
	char *queryString = text_to_cstring(queryText);
	EState *estate = NULL;
	List *nodeList = NIL;
	bool writeLocalFile = true;
	RemoteFileDestReceiver *resultDest = NULL;
	ParamListInfo paramListInfo = NULL;

	CheckCitusVersion(ERROR);
	.......
	/*
	 * Make sure that this transaction has a distributed transaction ID.
	 *
	 * Intermediate results will be stored in a directory that is derived
	 * from the distributed transaction ID.
	 */
	BeginOrContinueCoordinatedTransaction();

	estate = CreateExecutorState();
	resultDest = (RemoteFileDestReceiver *) CreateRemoteFileDestReceiver(resultIdString,
																		 estate, nodeList,
																		 writeLocalFile);

	ExecuteQueryStringIntoDestReceiver(queryString, paramListInfo,
									   (DestReceiver *) resultDest);
	.......
	FreeExecutorState(estate);

	PG_RETURN_INT64(resultDest->tuplesSent);
}
```





## 附表1：StartRemoteTransactionBegin函数调用情况

ManageTaskExecution

|____ MultiRealTimeExecute

|	|____ RealTimeExecScan

|____ MultiTaskTrackerExecute

​	|____ TaskTrackerExecScan



RemoteTransactionBegin

|____ endCommandListToWorkerInSingleTransaction

​	|____ FDD_metadata_sync_to_node

​	|	|____ master_add_coordinator

​	|	|____ start_metadata_sync_to_node

​	|____ RepairShardPlacement

​	|	|____ master_copy_shard_placement

​	|____ ReplicateShardToNode

​	|	|____ ReplicateAllReferenceTablesToNode

​	|	|____ ReplicateShardToAllWorkers

​	|		|____ ReplicateSingleShardTableToAllWorkers

​	|			|____ upgrade_to_reference_table

​	|____ start_metadata_sync_to_node





RemoteTransactionListBegin

|____ citus_create_restore_point

​		

RemoteTransactionsBeginIfNecessary

|____ GetModifyConnections

|	|____ ExecuteSingleModifyTask

|		|____ ExecuteModifyTasksSequentiallyWithoutResults

|		|	|____ ExecuteDistributedDDLJob

|		|	|____ master_modify_multiple_shards

|		|____ RouterSequentialModifyExecScan

|			|____ RouterModifyExecScan

|____ OpenTransactionsForAllTasks

|	|____ ExecuteModifyTasks

|		|____ ExecuteMultipleTasks

|		|	|____CoordinatorInsertSelectExecScan

|		|	|____RouterMultiModifyExecScan

|		|____ ExecuteModifyTasksWithoutResults

|			|____ ExecuteDistributedDDLJob

|			|____ master_modify_multiple_shards

|			|____ ProcessVacuumStmt		

|____ RemoteFileDestReceiverStartup

|	|____ CreateRemoteFileDestReceiver

|		|____ broadcast_intermediate_result

|		|____ create_intermediate_result

|		|____ ExecuteSubPlans

|			|____ RealTimeExecScan

|			|____ RouterModifyExecScan

|			|____ RouterSelectExecScan

|____ SendCommandToWorkersParams

|	|____ SendCommandToWorkers

|		|____  AddNodeMetadata

|		|____ CreateTableMetadataOnWorkers

|		|____ DeleteAllReferenceTablePlacementsFromNodeGroup

|		|____ MasterRemoveDistributedTableMetadataFromWorkers

|		|____ RemoveNodeFromCluster

|		|____ SetNodeState

|		|____ UpdateNodeLocation

|		|____ UpdateRelationColocationGroup	

|		|____ ProcessDropTableStmt

|		|____ ExecuteDistributedDDLJob

|		|____ FDD_SetHasMetadataToCN

|		|	|____ FDD_metadata_sync_to_node

|		|	|	|____ master_add_coordinator

|		|	|	|____ start_metadata_sync_to_node

|		|	|____ stop_metadata_sync_to_node

|		|____ FDD_SetNodeRole

|		|	|____ FDD_AlterToCN

|		|	|____ master_add_coordinator

|		|____ ReplicateShardToNode

|		|	|____ ReplicateAllReferenceTablesToNode

|		|	|	|____ ActivateNode

|		|	|		|____ master_activate_node

|		|	|		|____ master_add_coordinator

|		|	|		|____ master_add_node

|		|	|____ ReplicateShardToAllWorkers

|		|		|____ ReplicateSingleShardTableToAllWorkers

|		|			|____ upgrade_to_reference_table

|		|____ LockShardListMetadataOnWorkers

|		|	|____ BlockWritesToShardList

|		|	|	|____ DeleteAllReferenceTablePlacementsFromNodeGroup

|		|	|	|	|____ master_disable_node

|		|	|	|	|____ RemoveNodeFromCluster

|		|	|	|		|____ master_remove_node

|		|	|	|____ ReplicateAllReferenceTablesToNode

|		|	|		|____ ActivateNode

|		|	|____ LockReferencedReferenceShardDistributionMetadata

|		|		|____ BlockWritesToShardList

|		|		|____ RepairShardPlacement

|		|			|____ master_copy_shard_placement

|		|____ master_drop_sequences

|____ RemoteTransactionBeginIfNecessary

​	|____ CopyFromWorkerNode

​	|	|____ CitusCopyFrom

​	|____ CreateShardsOnWorkers

​	|	|____ CreateColocatedShards

​	|		|____ CreateHashDistributedTableShards

​	|	|____ CreateReferenceTableShard

​	|		|____ CreateDistributedTable

​	|	|____ CreateShardsWithRoundRobinPolicy

​	|		|____ CreateHashDistributedTableShards

​	|		|	|____ CreateDistributedTable

​	|		|____ master_create_worker_shards

​	|____ DropShards

​	|	|____ master_apply_delete_command

​	|	|____ master_drop_all_shards

​	|____ ExecuteSingleSelectTask

​	|	|____ RouterSelectExecScan					

​	|____ master_append_table_to_shard

​	|____ OpenCopyConnections

​	|	|____ CitusCopyDestReceiverReceive

​	|	|	|____ CreateCitusCopyDestReceiver

​	|	|____ StartCopyToNewShard

​	|		|____ CopyToNewShards

​	|			|____ CitusCopyFrom

​	|			|	|____ ProcessCopyStmt

​	|			|____ CopyFromWorkerNode

​	|				|____ CitusCopyFrom

​	|					|____ ProcessCopyStmt

​	|____ RemoteExplain

​	|	|____ ExplainTaskList

​	|		|____ ExplainJob

​	|			|____ CitusExplainScan

​	|____ SendCommandToWorker

​		|____ AcquireDistributedLockOnRelations

​		|	|____ LockTruncatedRelationMetadataInWorkers

​		|		|____ ProcessTruncateStatement

​		|____ SendCommandToFirstWorker

​			|____ LockShardListResourcesOnFirstWorker

​				|____ SerializeNonCommutativeWrites

​					|____ AcquireExecutorShardLock

​					|	|____ ExecuteSingleModifyTask

​					|____ CitusCopyDestReceiverStartup

​						|____ CreateCitusCopyDestReceiver

​							|____ CopyLocalDataIntoShards

​							|	|____ CreateDistributedTable

​							|____ CopyToExistingShards

​							|	|____ CitusCopyFrom

​							|		|____ ProcessCopyStmt

​							|____ ExecuteSelectIntoColocatedIntermediateResults

​							|		|____ CoordinatorInsertSelectExecScan

​							|____ ExecuteSelectIntoRelation

​								|____ CoordinatorInsertSelectExecScan