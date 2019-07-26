citus注册分布式事务管理的相关回调函数：

_PG_init-->InitializeTransactionManagement

```c
void
InitializeTransactionManagement(void)
{
	/* hook into transaction machinery */
	RegisterXactCallback(CoordinatedTransactionCallback, NULL);
	RegisterSubXactCallback(CoordinatedSubTransactionCallback, NULL);
	.....
}
```





测试sql：“insert into test select generate_series(1,20),12346; “

参考《citus分布式执行计划生成过程源码浅析》

生成的执行计划如下图：

![insert_select1_final](https://github.com/ZavierZX/picfiles/blob/master/insert_select1_final.png?raw=true)



DistributedPlan结构为：

![insert_select1](https://github.com/ZavierZX/picfiles/blob/master/insert_select1.png?raw=true)



## 一  PG执行PlannedStmt

分布式执行计划中的CoordinatorInsertSelectCustomScanMethods如下：

```c
CustomScanMethods CoordinatorInsertSelectCustomScanMethods = {
	"Citus INSERT ... SELECT via coordinator",
	CoordinatorInsertSelectCreateScan
};

```



### 1.1 初始化PlanState

代码调用流程：

PortalStart-->ExecutorStart-->standard_ExecutorStart-->InitPlan-->ExecInitNode-->ExecInitCustomScan



PlannedStmt的planTree是T_CustomScan类型，所以在ExecInitCustomScan中，PlanState的ExecProcNode成员初始化为：**ExecCustomScan**；

```c
CustomScanState *
ExecInitCustomScan(CustomScan *cscan, EState *estate, int eflags)
{
	CustomScanState *css;
	...
    css = castNode(CustomScanState,
				   cscan->methods->CreateCustomScanState(cscan));
	/* fill up fields of ScanState */
	css->ss.ps.plan = &cscan->scan.plan;
	css->ss.ps.state = estate;
	css->ss.ps.ExecProcNode = ExecCustomScan;

	css->methods->BeginCustomScan(css, estate, eflags);

	return css;
}
```



cscan->methods->CreateCustomScanState(cscan)实际上执行了CoordinatorInsertSelectCreateScan

```c
static Node *
CoordinatorInsertSelectCreateScan(CustomScan *scan)
{
	CitusScanState *scanState = palloc0(sizeof(CitusScanState));

	scanState->executorType = MULTI_EXECUTOR_COORDINATOR_INSERT_SELECT;
	scanState->customScanState.ss.ps.type = T_CustomScanState;
	scanState->distributedPlan = GetDistributedPlan(scan);

	scanState->customScanState.methods = &CoordinatorInsertSelectCustomExecMethods;

	return (Node *) scanState;
}
```

将CitusScanState的methods成员赋值为CoordinatorInsertSelectCustomExecMethods，其内容如下：

```c
static CustomExecMethods CoordinatorInsertSelectCustomExecMethods = {
	.CustomName = "CoordinatorInsertSelectScan",
	.BeginCustomScan = CitusSelectBeginScan,
	.ExecCustomScan = CoordinatorInsertSelectExecScan,
	.EndCustomScan = CitusEndScan,
	.ReScanCustomScan = CitusReScan,
	.ExplainCustomScan = CoordinatorInsertSelectExplainScan
};
```



### 1.2  运行

代码调用流程：

PortalRun-->PortalRunSelect-->ExecutorRun-->standard_ExecutorRun-->ExecutePlan-->ExecProcNode

```c
static inline TupleTableSlot *
ExecProcNode(PlanState *node)
{
	if (node->chgParam != NULL) /* something changed? */
		ExecReScan(node);		/* let ReScan handle this */

	return node->ExecProcNode(node);
}
```



进入**ExecCustomScan**函数：

```c
static TupleTableSlot *
ExecCustomScan(PlanState *pstate)
{
	CustomScanState *node = castNode(CustomScanState, pstate);

	CHECK_FOR_INTERRUPTS();

	Assert(node->methods->ExecCustomScan != NULL);
	return node->methods->ExecCustomScan(node);
}
```

node->methods->ExecCustomScan(node)实际上执行了CoordinatorInsertSelectExecScan：

```c
/*
 * CoordinatorInsertSelectExecScan executes an INSERT INTO distributed_table
 * SELECT .. query by setting up a DestReceiver that copies tuples into the
 * distributed table and then executing the SELECT query using that DestReceiver
 * as the tuple destination.
 */
TupleTableSlot *
CoordinatorInsertSelectExecScan(CustomScanState *node)
{
	CitusScanState *scanState = (CitusScanState *) node;
	TupleTableSlot *resultSlot = NULL;

	if (!scanState->finishedRemoteScan)
	{
		......
		/*
		 * If we are dealing with partitioned table, we also need to lock its
		 * partitions. Here we only lock targetRelation, we acquire necessary
		 * locks on selected tables during execution of those select queries.
		 */
		if (PartitionedTable(targetRelationId))
		{
             //对test表加锁
			LockPartitionRelations(targetRelationId, RowExclusiveLock);
		}

		if (distributedPlan->workerJob != NULL)
		{
			......
		}
		else
		{
             //这个例子中，分布式执行计划不含workerJob，进入ExecuteSelectIntoRelation
			ExecuteSelectIntoRelation(targetRelationId, insertTargetList, selectQuery,
									  executorState);
		}

		scanState->finishedRemoteScan = true;
	}

	resultSlot = ReturnTupleFromTuplestore(scanState);
	return resultSlot;
}
```

执行ExecuteSelectIntoRelation函数：

```c
static void
ExecuteSelectIntoRelation(Oid targetRelationId, List *insertTargetList,
						  Query *selectQuery, EState *executorState)
{
	......
	/* set up a DestReceiver that copies into the distributed table */
	copyDest = CreateCitusCopyDestReceiver(targetRelationId, columnNameList,
										   partitionColumnIndex, executorState,
										   stopOnFailure, NULL);
	/*
	 * Make a copy of the query, since ExecuteQueryIntoDestReceiver may scribble on it
	 * and we want it to be replanned every time if it is stored in a prepared
	 * statement.
	 */
	queryCopy = copyObject(selectQuery);

	ExecuteQueryIntoDestReceiver(queryCopy, paramListInfo, (DestReceiver *) copyDest);

	executorState->es_processed = copyDest->tuplesSent;

	XactModificationLevel = XACT_MODIFICATION_DATA;
}
```



通过CreateCitusCopyDestReceiver，创建CitusCopyDestReceiver对象，用于copy数据到worker节点：

```c
CitusCopyDestReceiver *
CreateCitusCopyDestReceiver(Oid tableId, List *columnNameList, int partitionColumnIndex,
							EState *executorState, bool stopOnFailure,
							char *intermediateResultIdPrefix)
{
	......
	/* set up the DestReceiver function pointers */
	copyDest->pub.receiveSlot = CitusCopyDestReceiverReceive;
	copyDest->pub.rStartup = CitusCopyDestReceiverStartup;
	copyDest->pub.rShutdown = CitusCopyDestReceiverShutdown;
	copyDest->pub.rDestroy = CitusCopyDestReceiverDestroy;
	copyDest->pub.mydest = DestCopyOut;

	/* set up output parameters */
	......
	return copyDest;
}
```

进入ExecuteQueryIntoDestReceiver，开始分析执行select 子查询（即DistributedPlan的insertSelectSubquery成员）

```c
void
ExecuteQueryIntoDestReceiver(Query *query, ParamListInfo params, DestReceiver *dest)
{
	PlannedStmt *queryPlan = NULL;
	int cursorOptions = 0;

	cursorOptions = CURSOR_OPT_PARALLEL_OK;

	/* plan the subquery, this may be another distributed query */
	queryPlan = pg_plan_query(query, cursorOptions, params);

	ExecutePlanIntoDestReceiver(queryPlan, params, dest);
}
```

ExecutePlanIntoDestReceiver过程为：

```c
void
ExecutePlanIntoDestReceiver(PlannedStmt *queryPlan, ParamListInfo params,
							DestReceiver *dest)
{
	Portal portal = NULL;
	int eflags = 0;
	long count = FETCH_ALL;

	/* create a new portal for executing the query */
	portal = CreateNewPortal();

	/* don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	PortalDefineQuery(portal,
					  NULL,
					  "",
					  "SELECT",
					  list_make1(queryPlan),
					  NULL);

	PortalStart(portal, params, eflags, GetActiveSnapshot());
	PortalRun(portal, count, false, true, dest, dest, NULL);
	PortalDrop(portal, false);
}
```



## 二 子计划执行

### 2.1  初始化PlanState

代码流程：PortalStart-->ExecutorStart-->standard_ExecutorStart-->InitPlan-->ExecInitNode

子计划是一个T_ProjectSet类型，所以执行的是ExecInitProjectSet

```c
ProjectSetState *
ExecInitProjectSet(ProjectSet *node, EState *estate, int eflags)
{
	......
	state->ps.ExecProcNode = ExecProjectSet;

	/*
	 * initialize child nodes，初始化ProjectSet lefttree
	 */
	outerPlanState(state) = ExecInitNode(outerPlan(node), estate, eflags);

	......
	/*
	 * Build expressions to evaluate targetlist.  We can't use
	 * ExecBuildProjectionInfo here, since that doesn't deal with SRFs.
	 * Instead compile each expression separately, using
	 * ExecInitFunctionResultSet where applicable.
	 */
	off = 0;
	foreach(lc, node->plan.targetlist)
	{
        .......
		if ((IsA(expr, FuncExpr) &&((FuncExpr *) expr)->funcretset) ||
			(IsA(expr, OpExpr) &&((OpExpr *) expr)->opretset))
		{
            //使用generate_series生成数据，进入这个分支
			state->elems[off] = (Node *)
				ExecInitFunctionResultSet(expr, state->ps.ps_ExprContext,
										  &state->ps);
		}
		.......
	}

	return state;
}

```

在ExecInitFunctionResultSet中，根据target生成一个SetExprState结构，包含函数信息，以及参数信息等：

```c
typedef struct SetExprState
{
	NodeTag		type;
	Expr	   *expr;			/* expression plan node */
	List	   *args;			/* ExprStates for argument expressions */
    .......
	/*
	 * Function manager's lookup info for the target function.  If func.fn_oid
	 * is InvalidOid, we haven't initialized it yet (nor any of the following
	 * fields, except funcReturnsSet).
	 */
	FmgrInfo	func;
	.......
	FunctionCallInfoData fcinfo_data;
} SetExprState;
```



### 2.2 运行

代码流程：PortalRun-->PortalRunSelect-->ExecutorRun-->standard_ExecutorRun

#### 2.2.1 cn事务开启

在standard_ExecutorRun中，因为1.2中创建了CitusCopyDestReceiver对象，所以会执行(*dest->rStartup) (dest, operation, queryDesc->tupDesc);进行一系列初始化，实际上执行了CitusCopyDestReceiverStartup函数：

```c
static void
CitusCopyDestReceiverStartup(DestReceiver *dest, int operation,
							 TupleDesc inputTupleDescriptor)
{
	......
	/* prevent concurrent placement changes and non-commutative DML statements */
	LockShardListMetadata(shardIntervalList, ShareLock);
	......
	BeginOrContinueCoordinatedTransaction();

	if (cacheEntry->replicationModel == REPLICATION_MODEL_2PC ||
		MultiShardCommitProtocol == COMMIT_PROTOCOL_2PC)
	{
		CoordinatedTransactionUse2PC();
	}

	.......

	copyDest->shardConnectionHash = CreateShardConnectionHash(TopTransactionContext);
}

```

这个函数中，citus通过BeginOrContinueCoordinatedTransaction，在cn开始一个事务：

```c
void
BeginOrContinueCoordinatedTransaction(void)
{
	if (CurrentCoordinatedTransactionState == COORD_TRANS_STARTED)
	{
		return;
	}

	BeginCoordinatedTransaction();
}

void
BeginCoordinatedTransaction(void)
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

因为citus中，配置项MultiShardCommitProtocol默认配置为COMMIT_PROTOCOL_2PC，所以会通过CoordinatedTransactionUse2PC开启2PC事务提交：

```c
void
CoordinatedTransactionUse2PC(void)
{
	Assert(InCoordinatedTransaction());

	CoordinatedTransactionUses2PC = true;
}

```

#### 2.2.2 执行计划

代码流程：standard_ExecutorRun-->ExecutePlan-->ExecProcNode

在ExecProcNode中，实际会调用ExecProjectSet

```c
static TupleTableSlot *
ExecProjectSet(PlanState *pstate)
{
	ProjectSetState *node = castNode(ProjectSetState, pstate);
	......
	/*
	 * Check to see if we're still projecting out tuples from a previous scan
	 * tuple (because there is a function-returning-set in the projection
	 * expressions).  If so, try to project another one.
	 */
	if (node->pending_srf_tuples)
	{
		resultSlot = ExecProjectSRF(node, true);

		if (resultSlot != NULL)
			return resultSlot;
	}
	......
	/*
	 * Get another input tuple and project SRFs from it.
	 */
	for (;;)
	{
		/*
		 * Retrieve tuples from the outer plan until there are no more.
		 */
		outerPlan = outerPlanState(node);
		outerTupleSlot = ExecProcNode(outerPlan);

		.......
		/* Evaluate the expressions */
		resultSlot = ExecProjectSRF(node, false);
		......
	}

	return NULL;
}

```

首先会执行ProjectSet lefttree的东西，在for循环中，进入ExecProjectSRF函数：

```c
static TupleTableSlot *
ExecProjectSRF(ProjectSetState *node, bool continuing)
{
	......
	hassrf = hasresult = false;
	for (argno = 0; argno < node->nelems; argno++)
	{
		......
		else if (IsA(elem, SetExprState))
		{
			/*
			 * Evaluate SRF - possibly continuing previously started output.
			 */
			*result = ExecMakeFunctionResultSet((SetExprState *) elem,
												econtext, isnull, isdone);

			if (*isdone != ExprEndResult)
				hasresult = true;
			if (*isdone == ExprMultipleResult)
				node->pending_srf_tuples = true;
			hassrf = true;
		}
		......
	}
	......
	if (hasresult)
	{
		ExecStoreVirtualTuple(resultSlot);
		return resultSlot;
	}

	return NULL;
}

```

在2.1中，根据target生成一个SetExprState结构，所以进入ExecMakeFunctionResultSet函数，这个函数执行过程中会调用generate_series。

#### 2.2.3 worker事务开启

代码流程：ExecutePlan-->(*dest->receiveSlot) (slot, dest)

在2.2.2中，生成一条数据之后，cn会通过copy的方式，将数据发送到worker节点，实际上调用CitusCopyDestReceiverReceive，该函数会根据生成的分布键键值，找到对应的分片位置信息，建立连接（可能会有链接复用）：

```c
static bool
CitusCopyDestReceiverReceive(TupleTableSlot *slot, DestReceiver *dest)
{
	........
	/*
	 * Find the shard interval and id for the partition column value for
	 * non-reference tables.
	 *
	 * For reference table, this function blindly returns the tables single
	 * shard.
	 */
	shardInterval = FindShardInterval(partitionColumnValue, copyDest->tableMetadata);
	if (shardInterval == NULL)
	{
		.....
	}

	shardId = shardInterval->shardId;

	......

	/* get existing connections to the shard placements, if any */
	shardConnections = GetShardHashConnections(shardConnectionHash, shardId,
											   &shardConnectionsFound);
	if (!shardConnectionsFound)
	{
		......

		/* open connections and initiate COPY on shard placements */
		OpenCopyConnections(copyStatement, shardConnections, stopOnFailure,
							copyOutState->binary);

		......
	}

	/* replicate row to shard placements */
	resetStringInfo(copyOutState->fe_msgbuf);
	AppendCopyRowData(columnValues, columnNulls, tupleDescriptor,
					  copyOutState, columnOutputFunctions, columnCoercionPaths);
	SendCopyDataToAll(copyOutState->fe_msgbuf, shardId, shardConnections->connectionList);

	.......
	return true;
}

```

OpenCopyConnections创建一个新的连接，并且在worker节点开启一个事务：

```c
static void OpenCopyConnections(CopyStmt *copyStatement, ShardConnections *shardConnections,
					bool stopOnFailure, bool useBinaryCopyFormat)
{
	.......

	foreach(placementCell, finalizedPlacementList)
	{
		......
		/*
		 * Make sure we use a separate connection per placement for hash-distributed
		 * tables in order to allow multi-shard modifications in the same transaction.
		 */
		if (placement->partitionMethod == DISTRIBUTE_BY_HASH)
		{
			connectionFlags |= CONNECTION_PER_PLACEMENT;
		}

		connection = GetPlacementConnection(connectionFlags, placement, nodeUser);

		if (PQstatus(connection->pgConn) != CONNECTION_OK)
		{
			......
		}

		.......
		RemoteTransactionBeginIfNecessary(connection);

		copyCommand = ConstructCopyStatement(copyStatement, shardConnections->shardId,
											 useBinaryCopyFormat);

		if (!SendRemoteCommand(connection, copyCommand->data))
		{
			ReportConnectionError(connection, ERROR);
		}
		.......
	}

	/* if all placements failed, error out */
	if (failedPlacementCount == list_length(finalizedPlacementList))
	{
		ereport(ERROR, (errmsg("could not connect to any active placements")));
	}

	.......
}

```

调用RemoteTransactionBeginIfNecessary在worker开启一个事务：

```c
void
RemoteTransactionBeginIfNecessary(MultiConnection *connection)
{
	/* just delegate */
	if (InCoordinatedTransaction())
	{
		List *connectionList = list_make1(connection);

		RemoteTransactionsBeginIfNecessary(connectionList);
		list_free(connectionList);
	}
}

void
RemoteTransactionsBeginIfNecessary(List *connectionList)
{
	......

	/* issue BEGIN to all connections needing it */
	foreach(connectionCell, connectionList)
	{
		......
		if (transaction->transactionState != REMOTE_TRANS_INVALID)
		{
			continue;
		}

		StartRemoteTransactionBegin(connection);
	}

	......
}

void
StartRemoteTransactionBegin(struct MultiConnection *connection)
{
	......
	transaction->transactionState = REMOTE_TRANS_STARTING;
	/*
	 * Explicitly specify READ COMMITTED, the default on the remote
	 * side might have been changed, and that would cause problematic
	 * behaviour.
	 */
	appendStringInfoString(beginAndSetDistributedTransactionId,
						   "BEGIN TRANSACTION ISOLATION LEVEL READ COMMITTED;");
	/*
	 * Append BEGIN and assign_distributed_transaction_id() statements into a single command
	 * and send both in one step. The reason is purely performance, we don't want
	 * seperate roundtrips for these two statements.
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
            ......
	}
}

```

在worker开启事务之后，通过ConstructCopyStatement发送一条copy命令给worker， 本例发送‘GCOPY public.test_102009 (id, name) FROM STDIN WITH (FORMAT BINARY)’：

```c
static StringInfo
ConstructCopyStatement(CopyStmt *copyStatement, int64 shardId, bool useBinaryCopyFormat)
{
	StringInfo command = makeStringInfo();

	char *schemaName = copyStatement->relation->schemaname;
	char *relationName = copyStatement->relation->relname;

	char *shardName = pstrdup(relationName);
	char *shardQualifiedName = NULL;

	AppendShardIdToName(&shardName, shardId);

	shardQualifiedName = quote_qualified_identifier(schemaName, shardName);

	appendStringInfo(command, "COPY %s ", shardQualifiedName);

	if (copyStatement->attlist != NIL)
	{
		ListCell *columnNameCell = NULL;
		bool appendedFirstName = false;

		foreach(columnNameCell, copyStatement->attlist)
		{
			char *columnName = (char *) lfirst(columnNameCell);

			if (!appendedFirstName)
			{
				appendStringInfo(command, "(%s", columnName);
				appendedFirstName = true;
			}
			else
			{
				appendStringInfo(command, ", %s", columnName);
			}
		}

		appendStringInfoString(command, ") ");
	}

	appendStringInfo(command, "FROM STDIN WITH ");

	if (IsCopyResultStmt(copyStatement))
	{
		appendStringInfoString(command, "(FORMAT RESULT)");
	}
	else if (useBinaryCopyFormat)
	{
		appendStringInfoString(command, "(FORMAT BINARY)");
	}
	else
	{
		appendStringInfoString(command, "(FORMAT TEXT)");
	}

	return command;
}
```

#### 2.2.4 copy数据

代码流程：SendCopyDataToAll-->SendCopyDataToPlacement--->PutRemoteCopyData-->PQputCopyData

在PQputCopyData函数，将数据放到缓冲区，如果缓冲区满了，会调用pqFlush实际发送数据。



## 三 事务提交

代码流程：exec_simple_query-->finish_xact_command-->CommitTransactionCommand-->CommitTransaction

```c
static void
CommitTransaction(void)
{
	......
	is_parallel_worker = (s->blockState == TBLOCK_PARALLEL_INPROGRESS);

	......

	CallXactCallbacks(is_parallel_worker ? XACT_EVENT_PARALLEL_PRE_COMMIT
					  : XACT_EVENT_PRE_COMMIT);

	/*
	 * The remaining actions cannot call any user-defined code, so it's safe
	 * to start shutting down within-transaction services.  But note that most
	 * of this stuff could still throw an error, which would switch us into
	 * the transaction-abort path.
	 */

	......
	PreCommit_on_commit_actions();

	.......

	CallXactCallbacks(is_parallel_worker ? XACT_EVENT_PARALLEL_COMMIT
					  : XACT_EVENT_COMMIT);
	......
	RESUME_INTERRUPTS();
}

```

CallXactCallbacks会调用注册的CoordinatedTransactionCallback，本例不是并行任务，第一次调用会走到下面的流程：

```c
static void
CoordinatedTransactionCallback(XactEvent event, void *arg)
{
	switch (event)
	{
         ......
		case XACT_EVENT_PRE_COMMIT:
		{
			......

			if (CoordinatedTransactionUses2PC)
			{
				CoordinatedRemoteTransactionsPrepare();
				CurrentCoordinatedTransactionState = COORD_TRANS_PREPARED;

				/*
				 * Make sure we did not have any failures on connections marked as
				 * critical before committing.
				 */
				CheckRemoteTransactionsHealth();
			}
			.......

			/*
			 * Check again whether shards/placement successfully
			 * committed. This handles failure at COMMIT/PREPARE time.
			 */
			PostCommitMarkFailedShardPlacements(CoordinatedTransactionUses2PC);
			break;
		}
		.......
	}
}
```

### 3.1 PREPARE TRANSACTION

CoordinatedRemoteTransactionsPrepare向worker节点发送‘PREPARE TRANSACTION’命令：

```c
void CoordinatedRemoteTransactionsPrepare(void)
{
	......
	/* asynchronously send PREPARE */
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		Assert(transaction->transactionState != REMOTE_TRANS_INVALID);

		/* can't PREPARE a transaction that failed */
		if (transaction->transactionFailed)
		{
			continue;
		}

		StartRemoteTransactionPrepare(connection);
		connectionList = lappend(connectionList, connection);
	}

	raiseInterrupts = true;
	WaitForAllConnections(connectionList, raiseInterrupts);

	/* Wait for result */
	......

	CurrentCoordinatedTransactionState = COORD_TRANS_PREPARED;
}

```



```c
void StartRemoteTransactionPrepare(struct MultiConnection *connection)
{
	RemoteTransaction *transaction = &connection->remoteTransaction;
	......
	Assign2PCIdentifier(connection);

	/* log transactions to workers in pg_dist_transaction */
	workerNode = FindWorkerNode(connection->hostname, connection->port);
	if (workerNode != NULL)
	{
		LogTransactionRecord(workerNode->groupId, transaction->preparedName);
	}

	initStringInfo(&command);
	appendStringInfo(&command, "PREPARE TRANSACTION '%s'",
					 transaction->preparedName);

	if (!SendRemoteCommand(connection, command.data))
	{
		HandleRemoteTransactionConnectionError(connection, raiseErrors);
	}
	......
}
```



在Assign2PCIdentifier中，给每个事务分配了一个标识符：

```c
static void
Assign2PCIdentifier(MultiConnection *connection)
{
	/* local sequence number used to distinguish different connections */
	static uint32 connectionNumber = 0;

	/* transaction identifier that is unique across processes */
	uint64 transactionNumber = CurrentDistributedTransactionNumber();

	/* print all numbers as unsigned to guarantee no minus symbols appear in the name */
	snprintf(connection->remoteTransaction.preparedName, NAMEDATALEN,
			 PREPARED_TRANSACTION_NAME_FORMAT, GetLocalGroupId(), MyProcPid,
			 transactionNumber, connectionNumber++);
}

```



具体的格式：("citus\_%u\_%u\_%u_%u", GetLocalGroupId(), MyProcPid, transactionNumber, connectionNumber++)

如发送给某个worker的信息为：PREPARE TRANSACTION 'citus_0_87551_10_15'

​	由cn发出，cn的localgroupid为0，执行sql的子进程id为87551，事务ID为10，这是该子进程第15个事务（连接）



PG官方文档对这个标识的解释是：

​	一个任意的事务标识符， `COMMIT PREPARED`或者`ROLLBACK PREPARED` 以后将用这个标识符来标识这个事务。该标识符必须写成一个字符串， 并且长度必须小于 200 字节。它也不能与任何当前已经准备好的事务的标识符相同。 

### 3.2 COMMIT PREPARED

第二次执行CallXactCallbacks会走到下面的流程：

```
static void
CoordinatedTransactionCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		{
			......

			if (CurrentCoordinatedTransactionState == COORD_TRANS_PREPARED)
			{
				/* handles both already prepared and open transactions */
				CoordinatedRemoteTransactionsCommit();
			}

			......

			CurrentCoordinatedTransactionState = COORD_TRANS_NONE;
			XactModificationLevel = XACT_MODIFICATION_NONE;
			dlist_init(&InProgressTransactions);
			CoordinatedTransactionUses2PC = false;

			UnSetDistributedTransactionId();

			......
			break;
		}
		......
	}
}

```

CoordinatedRemoteTransactionsCommit会向worker发送‘COMMIT PREPARED’命令：

```c
void
CoordinatedRemoteTransactionsCommit(void)
{
	......
	dlist_foreach(iter, &InProgressTransactions)
	{
		MultiConnection *connection = dlist_container(MultiConnection, transactionNode,
													  iter.cur);
		RemoteTransaction *transaction = &connection->remoteTransaction;

		if (transaction->transactionState == REMOTE_TRANS_INVALID ||
			transaction->transactionState == REMOTE_TRANS_1PC_COMMITTING ||
			transaction->transactionState == REMOTE_TRANS_2PC_COMMITTING ||
			transaction->transactionState == REMOTE_TRANS_COMMITTED ||
			transaction->transactionState == REMOTE_TRANS_ABORTED)
		{
			continue;
		}

		StartRemoteTransactionCommit(connection);
		connectionList = lappend(connectionList, connection);
	}

	raiseInterrupts = false;
	WaitForAllConnections(connectionList, raiseInterrupts);

	......
}

void
StartRemoteTransactionCommit(MultiConnection *connection)
{
	......

	else if (transaction->transactionState == REMOTE_TRANS_PREPARED)
	{
		/* commit the prepared transaction */
		StringInfoData command;

		initStringInfo(&command);
		appendStringInfo(&command, "COMMIT PREPARED '%s'",
						 transaction->preparedName);

		transaction->transactionState = REMOTE_TRANS_2PC_COMMITTING;

		if (!SendRemoteCommand(connection, command.data))
		{
			HandleRemoteTransactionConnectionError(connection, raiseErrors);

			WarnAboutLeakedPreparedTransaction(connection, isCommit);
		}
	}
	else
	......
}
```

### 3.3  worker收到命令之后处理

#### 3.3.1  处理PREPARE TRANSACTION

进入standard_ProcessUtility函数处理：

```c
void standard_ProcessUtility(PlannedStmt *pstmt,const char *queryString,	ProcessUtilityContext context,ParamListInfo params,	QueryEnvironment *queryEnv,			DestReceiver *dest,	char *completionTag)
{
	......
	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;
				switch (stmt->kind)
				{
					......
					case TRANS_STMT_PREPARE:
						PreventCommandDuringRecovery("PREPARE TRANSACTION");
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in completionTag */
							if (completionTag)
								strcpy(completionTag, "ROLLBACK");
						}
						break;
					......
				}
			}
			break;
		......
	}
	free_parsestate(pstate);
}
```

调用PrepareTransactionBlock处理，其中stmt->gid就是事务的标识（remoteTransaction.preparedName） ，如： 'citus_0_87551_10_15'，该函数将标识保存在prepareGID中：

```c
bool PrepareTransactionBlock(char *gid)
{
	TransactionState s;
	bool		result;

	/* Set up to commit the current transaction */
	result = EndTransactionBlock();

	/* If successful, change outer tblock state to PREPARE */
	if (result)
	{
		s = CurrentTransactionState;

		while (s->parent != NULL)
			s = s->parent;

		if (s->blockState == TBLOCK_END)
		{
			/* Save GID where PrepareTransaction can find it again */
			prepareGID = MemoryContextStrdup(TopTransactionContext, gid);

			s->blockState = TBLOCK_PREPARE;
		}
		else
		{
			/*
			 * ignore case where we are not in a transaction;
			 * EndTransactionBlock already issued a warning.
			 */
			Assert(s->blockState == TBLOCK_STARTED);
			/* Don't send back a PREPARE result tag... */
			result = false;
		}
	}

	return result;
}
```



在执行完该命令，提交事务的时候，PG调用finish_xact_command，进入CommitTransactionCommand处理：

```c
void CommitTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		......
		case TBLOCK_PREPARE:
			PrepareTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * We were just issued a SAVEPOINT inside a transaction block.
			 * Start a subtransaction.  (DefineSavepoint already did
			 * PushTransaction, so as to have someplace to put the SUBBEGIN
			 * state.)
			 */
		......
	}
}
```

在PrepareTransaction中通过StartPrepare存入PG全局变量records（xlog）：

```c
static void PrepareTransaction(void)
{
    ......
    //1、获取PG当前的事务ID（非citus的分布式事务事务ID）
    TransactionId xid = GetCurrentTransactionId();
	......
	CallXactCallbacks(XACT_EVENT_PRE_PREPARE);

	......
	/*
	 * set the current transaction state information appropriately during
	 * prepare processing
	 */
	s->state = TRANS_PREPARE;

	prepared_at = GetCurrentTimestamp();

    //2、把标识和xid存到gxact和TwoPhaseState中，将twophaseExitRegistered置为TURE
	gxact = MarkAsPreparing(xid, prepareGID, prepared_at,
							GetUserId(), MyDatabaseId);
	prepareGID = NULL;

	......
    //3、把gxact保存到records
	StartPrepare(gxact);

	......
    //4、把records信息写入到xlog日志
	EndPrepare(gxact);

	......
	CallXactCallbacks(XACT_EVENT_PREPARE);

	.......
	RESUME_INTERRUPTS();
}
```

PrepareTransaction第一次调用CallXactCallbacks：

```c
static void CoordinatedTransactionCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		......
		case XACT_EVENT_PRE_PREPARE:
		{
            //CurrentCoordinatedTransactionState标记cn事务的状态，worker节点为COORD_TRANS_NONE
			if (CurrentCoordinatedTransactionState > COORD_TRANS_NONE)
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("cannot use 2PC in transactions involving "
									   "multiple servers")));
			}
			break;
		}
	}
}
```

PrepareTransaction第二次调用CallXactCallbacks：

```c
static void CoordinatedTransactionCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		......

		case XACT_EVENT_PREPARE:
		{
            //这个分支只和worker节点相关，
           	//我们应该在重置分布式事务ID之前删除中间结果。这是必要的，否则Citus将尝试删除不存在的文件夹并泄漏与worker节点上的分布式事务ID关联的一些现有文件夹。
			RemoveIntermediateResultsDirectory();
			//重置worker节点分布式事务信息
			UnSetDistributedTransactionId();
			break;
		}

		......
	}
}
//重置worker节点分布式事务信息（清空）
void UnSetDistributedTransactionId(void)
{
	/* backend does not exist if the extension is not created */
	if (MyBackendData)
	{
		SpinLockAcquire(&MyBackendData->mutex);
		MyBackendData->databaseId = 0;
		MyBackendData->userId = 0;
		MyBackendData->transactionId.initiatorNodeIdentifier = 0;
		MyBackendData->transactionId.transactionOriginator = false;
		MyBackendData->transactionId.transactionNumber = 0;
		MyBackendData->transactionId.timestamp = 0;
		MyBackendData->citusBackend.initiatorNodeIdentifier = -1;
		MyBackendData->citusBackend.transactionOriginator = false;
		SpinLockRelease(&MyBackendData->mutex);
	}
}
```

#### 3.3.2 处理COMMIT PREPARED

进入standard_ProcessUtility处理：

```c
void standard_ProcessUtility(PlannedStmt *pstmt,
						const char *queryString,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						char *completionTag)
{
	......
	switch (nodeTag(parsetree))
	{
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;
				switch (stmt->kind)
				{
					......
					case TRANS_STMT_COMMIT_PREPARED:
						PreventTransactionChain(isTopLevel, "COMMIT PREPARED");
						PreventCommandDuringRecovery("COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, true);
						break;

					......
				}
			}
			break;
	......
	}
	free_parsestate(pstate);
}
```

进入FinishPreparedTransaction函数处理：

```c
void FinishPreparedTransaction(const char *gid, bool isCommit)
{
	......
     
	/*
	 * Validate the GID, and lock the GXACT to ensure that two backends do not
	 * try to commit the same GID at once.
	 */
	gxact = LockGXact(gid, GetUserId());
	proc = &ProcGlobal->allProcs[gxact->pgprocno];
	pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];
	xid = pgxact->xid;

	......
	if (isCommit)
		RecordTransactionCommitPrepared(xid,
										hdr->nsubxacts, children,
										hdr->ncommitrels, commitrels,
										hdr->ninvalmsgs, invalmsgs,
										hdr->initfileinval);
	else
		RecordTransactionAbortPrepared(xid,
									   hdr->nsubxacts, children,
									   hdr->nabortrels, abortrels);
	......
	pfree(buf);
}
```

LockGXact校验传入的标识符：gid，获取正确的gxact信息：

```c
static GlobalTransaction LockGXact(const char *gid, Oid user)
{
	int			i;

	/* 1、处理prepare transaction时，在MarkAsPreparing置为了true */
	if (!twophaseExitRegistered)
	{
		before_shmem_exit(AtProcExit_Twophase, 0);
		twophaseExitRegistered = true;
	}

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	//2、从TwoPhaseState匹配正确的gxact
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];

		/* 是否有效 */
		if (!gxact->valid)
			continue;
        //匹配标识符
		if (strcmp(gxact->gid, gid) != 0)
			continue;

		/* Found it, but has someone else got it locked? */
		if (gxact->locking_backend != InvalidBackendId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("prepared transaction with identifier \"%s\" is busy",
							gid)));
		//匹配用户名
		if (user != gxact->owner && !superuser_arg(user))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to finish prepared transaction"),
					 errhint("Must be superuser or the user that prepared the transaction.")));

		/*
		 * Note: it probably would be possible to allow committing from
		 * another database; but at the moment NOTIFY is known not to work and
		 * there may be some other issues as well.  Hence disallow until
		 * someone gets motivated to make it work.
		 */
		if (MyDatabaseId != proc->databaseId)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prepared transaction belongs to another database"),
					 errhint("Connect to the database where the transaction was prepared to finish it.")));

		/* OK for me to lock it */
		gxact->locking_backend = MyBackendId;
		MyLockedGxact = gxact;

		LWLockRelease(TwoPhaseStateLock);

		return gxact;
	}

	LWLockRelease(TwoPhaseStateLock);

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("prepared transaction with identifier \"%s\" does not exist",
					gid)));

	/* NOTREACHED */
	return NULL;
}
```

通过获取到gxact，根据pgxact->xid，在RecordTransactionCommitPrepared写xlog。



#### 3.3.3  查看worker节点的xlog日志

使用pg_waldump命令查看worker节点的xlog：

![xlog_2PC](https://github.com/ZavierZX/picfiles/blob/master/xlog_2PC.png?raw=true)



#### 3.3.4  citus分布式事务ID用途

1、判断节点是否正在处理分布式事务。

2、用于死锁检测

3、citus两阶段提交（RecoverTwoPhaseCommits）

4、查询结果写入中间文件（pgsql_job_cache等中的文件）命名，如果正在处理分布式事务，其命名方式如下：

​	base/pgsql_job_cache/***(userId)***_***(initiatorNodeIdentifier)***\_***(transactionNumber)***

5、Assign2PCIdentifier，执行prepare transaction等命令时，在PG流程中，和节点ID、时间戳等组成PG两阶段提交唯一的标识符。