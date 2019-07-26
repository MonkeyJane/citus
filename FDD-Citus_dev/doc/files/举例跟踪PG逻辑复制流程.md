## 一 逻辑复制

自 PostgreSQL 9.4 ，开始支持逻辑流复制。 

### 1.1 背景

PostgreSQL 9.4 对逻辑流复制的支持具有相当的意义。我们可以用该技术完成很多企业级的需求。

1. PostgreSQL 的大版本升级，由于可以使用流复制做增量数据同步，所以停机服务时间会非常短。
2. PostgreSQL 自定义逻辑拆库。由于是逻辑数据，所以很容易自定义分发规则。例如按表拆，逻辑分表，白名单，黑名单等等。
3. PostgreSQL 增量同步到其他异构数据库，例如 PG 同步到 Mysql；PG 同步到 Oracle。
4. 逻辑主备同步，逻辑多节点同时可写集群。



### 1.2 名词

| 名词               | 解释                                                         |
| ------------------ | ------------------------------------------------------------ |
| Logical Decoding   | PostgreSQL 的逻辑日志来源于解析物理 WAL 日志。解析 WAL 成为逻辑数据的过程叫 Logical Decoding。 |
| Replication Slots  | 保存逻辑或物理流复制的基础信息。类似 Mysql 的位点信息。一个 逻辑 slot 创建后，它的相关信息可以通过 pg_replication_slots 系统视图获取。如果它在 active 状态，则可以通过系统视图 pg_stat_replication 看到一些 slot 的实时的状态信息。 |
| Output Plugins     | PostgreSQL 的逻辑流复制协议开放一组可编程接口，用于自定义输数据到客户端的逻辑数据的格式。这部分实现使用插件的方式被内核集成和使用，称作 Output Plugins。 |
| Exported Snapshots | 当一个逻辑流复制 slot 被创建时，系统会产生一个快照。客户端可以通过它订阅到数据库任意时间点的数据变化。 |

### 1.3 逻辑流复制输出的数据格式

Logical Decoding 是把 WAL 日志解析成逻辑日志的过程。这个过程输出的是数据格式可以描述为：
1、事务开始 任何的变化总是在一个事务中，所以订阅的数据变化的开始是是一个事务被启动的消息，他包括了事务 ID，LSN，开始时间等信息。
2、数据的变化 包括当前事务中修改的数据。即对某些表的 insert update delete 操作来带的数据变化。

- ​	一个事务内可以包含任意个表和任意行数据的变化。

	 ​	输出的数据的格式和对应表的定义和索引相关。

3、事务的提交 包括事务提交的 LSN，时间等相关信息。



摘自：https://m.aliyun.com/yunqi/articles/71916



## 二 逻辑复制设置

### 2.1 GUC设置

PG支持逻辑复制，wal级别需要设置大于等于'logical'：

```c
wal_level=logical
```



PG数据库在启动时，如果配置项'max_logical_replication_workers'大于0（默认为4），PG会注册一个"logical replication launcher"进程（逻辑复制管理进程），该进程执行函数为ApplyLauncherMain，然后启动该进程。

代码调用流程如下：

```c
PostmasterMain
	ApplyLauncherRegister //("ApplyLauncherMain")
		RegisterBackgroundWorker
	maybe_start_bgworkers
```



## 三 发布者订阅者创建

### 3.1 创建发布者

```sql
CREATE PUBLICATION name
    [ FOR TABLE [ ONLY ] table_name [ * ] [, ...]
      | FOR ALL TABLES ]
    [ WITH ( publication_parameter [= value] [, ... ] ) ]
```

FOR TABLE：发布的关系，可以是列表

FOR ALL TABLES ：该数据库中的所有关系，包括之后创建的

WITH ：publish (string)，关注的操作，默认为'insert, update, delete'



例如：

```sql
CREATE PUBLICATION pub_shard FOR TABLE test;
```



#### 3.1.2 创建流程

1、在表pg_publication中插入数据。

2、解析发布的关系，在pg_publication_rel插入数据。

pg_publication表中的数据：

![publication](https://github.com/ZavierZX/picfiles/blob/master/publication.jpg?raw=true)

pg_publication_rel表中的数据：

![pg_publication_rel](https://github.com/ZavierZX/picfiles/blob/master/pg_publication_rel.jpg?raw=true)





### 3.2 创建订阅者

```sql
CREATE SUBSCRIPTION subscription_name
    CONNECTION 'conninfo'
    PUBLICATION publication_name [, ...]
    [ WITH ( subscription_parameter [= value] [, ... ] ) ]
```

subscription_name：订阅者名字

CONNECTION：创建连接需要的参数

PUBLICATION：发布者名字（可以多个）

WITH：

​	copy_data  存量数据是否更新，默认为true

​	create_slot 是否创建复制槽（slot），默认为true

​	enabled  是否立即启动，默认为true

​	slot_name 复制槽的名字，如果没有设置，与订阅者名字一样

​	synchronous_commit  同步提交，默认为false

​	connect  是否创建时连接发布者，默认为true。如果connect设置为false，enabled, create_slot,  copy_data使用默认，enabled, create_slot,  copy_data会被强制转换为false，如果connect设置为false，enabled, create_slot,  copy_data设置为true且不为默认值，出错。



例如：

```sql
CREATE SUBSCRIPTION test
CONNECTION 'host=192.168.244.129 port=15432'
PUBLICATION pub_shard;
```

#### 3.2.1 创建流程

1、在pg_subscription插入数据

2、如果connect为true，连接发布者，查询发布者publication对应的表，保存在pg_subscription_rel中，如果create_slot为true，发送"CREATE_REPLICATION_SLOT"到发布者。



## 四 工作流程

### 4.1 逻辑复制管理进程

该进程在PS中显示是名称为："postgres: bgworker: logical replication launcher"。

该进程的主体执行函数为ApplyLauncherMain，主要功能是管理订阅者：

> 1、从pg_subscription获取订阅者。
>
> 2、查找对应的订阅者是否已经在运行（相关信息保存在LogicalRepCtx->workers中）。
>
> 3、如果订阅者没有运行，注册并启动子进程，该子进程名称为"logical replication worker for subscription"，主体执行函数为ApplyWorkerMain。



### 4.2 逻辑复制工作进程

该进程在PS中显示名称为：'postgres: bgworker: logical replication worker for subscription' + 目标表OID

该进程（子进程A）主体执行函数为ApplyWorkerMain，主要功能是为目标表做存量和增量更新：

存量更新：

> 1、子进程A连接发布者
>
> 2、发送"IDENTIFY_SYSTEM"命令到发布者，获取发布者系统状态。
>
> 3、发送"START_REPLICATION"命令到发布者（如果没有执行CREATE_REPLICATION_SLOT，将会报错退出） 。
>
> 4、从pg_subscription_rel表中获取没有准备好的关系列表（srsubstate不是'r'）
>
> 5、对于没有准备好的关系，注册和启动子进程（子进程B），该子进程主体执行函数为ApplyWorkerMain，参数带了具体的关系OID，这个子进程进行存量更新。
>
> 6、子进程B发送"BEGIN READ ONLY"到发布者。
>
> 7、发送"CREATE_REPLICATION_SLOT"（TEMPORARY）命令到发布者，创建slot。
>
> 8、发送"COPY %s TO STDOUT"到发布者，拷贝对应的表数据。
>
> 9、发送"commit"命令到发布者。
>
> 9、子进程B退出。



增量更新：

> 子进程A循环：
>
> 1、子进程A在订阅者存在期间一直存在，向发布者发送查询wal location请求。
>
> 2、处理发布者发送过来的数据变更（如插入，删除，更新）。



### 4.3 发布者wal发送进程

在2.3中，订阅者产生的子进程连接发布者，在startup数据包中带了"replication"和"database"参数，发布者产生的子进程初始化为wal sender进程，收到订阅者发来的消息，进入exec_replication_command处理流程，如果exec_replication_command 处理失败，会进入常规的sql命令处理。

#### 4.3.1 处理CREATE_REPLICATION_SLOT

（1）在共享内存中申请内存：ReplicationSlotCtl->replication_slots

（2）创建初始化decode上下文，获取复制槽需要的初始信息。

	>1、获取restart_lsn，处于恢复期，GetRedoRecPtr，否则GetXLogInsertRecPtr，考虑wal日志被删除的场景。
	>
	>2、获取oldest transactionid
	>
	>​	排他锁锁住 ProcArrayLock  防止vaccum
	>
	>​	共享锁锁住 XidGenLock , 其他进程不能请求新的xid
	>
	>​	遍历allPgXact，获取oldestSafeXid
	>
	>3、创建上下文，调用StartupDecodingContext函数实现
	>
	>​	回调函数赋值：ctx->callbacks，默认使用"pgoutput"动态库，加载动态库中的函数。
	>
	>​	初始化wal日志读取对象（ReorderBuffer， XLogReaderState），给其回调函数赋值。
	>	启动上下文。

（3）释放上下文，将创建的slot信息回复订阅者。



涉及到的回调函数：

```c
callbacks->begin_cb        = pgoutput_begin_txn       //begin_cb_wrapper调用   

callbacks->change_cb  	   = pgoutput_change		//change_cb_wrapper调用

callbacks->commit_cb	   =  pgoutput_commit_txn  //commit_cb_wrapper调用

ctx->reorder->begin 	   = begin_cb_wrapper;		//读取到commit命令的时候调用，应答"B"命令

ctx->reorder->apply_change = change_cb_wrapper;		//读取到commit命令的时候调用，应答"I"等命令
	
ctx->reorder->commit 	   = commit_cb_wrapper;		//读取到commit命令的时候调用，应答"C"命令

ctx->reorder->message 	   = message_cb_wrapper;

ctx->prepare_write         = WalSndPrepareWrite

ctx->write                 = WalSndWriteData;

ctx->update_progress       = WalSndUpdateProgress;
```



#### 4.3.2 处理START_REPLICATION

（1）从共享内存中获取对应的复制槽：ReplicationSlotCtl->replication_slots

（2）创建decode上下文（logical_decoding_ctx），调用StartupDecodingContext函数实现，回调函数参考4.3.1中的信息。

（3）进入主循环（WalSndLoop）：处理增量数据更新。

### 4.4  处理数据

2.3中子进程B连接发布者产生的子进程（存量数据发送）：

> 1、处理"BEGIN READ ONLY "命令
>
> 2、处理"CREATE_REPLICATION_SLOT"命令（TEMPORARY）。
>
> 3、处理"COPY"命令
>
> 4、处理"COMMIT"命令
>
> 5、退出



2.3中子进程A连接发布者产生的子进程（增量数据发送），该过程主要在WalSndLoop函数中循环实现，调用XLogSendLogical读取并decode wal日志：

> 1、读取wal日志，针对不同类型的wal日志进行处理。
>
> 2、如果有”insert“、”delete“、”update“操作（举例），从wal日志中解析元组，放入待处理队列
>
> 3、读取到一条commit日志，处理队列中已经存在的buf（事务id对应的）
>
> 4、向订阅者发送begin （"B")
>
> 5、如果订阅者关心的不是test表（比如test1），略过；如果是订阅者关心的，把元组数据发给订阅者 ("I“)（在change_cb_wrapper处理过滤）
>
> 6、向订阅者发送commit ("C")



LogicalDecodingProcessRecord函数处理读取到wal日志，：

```c
void
LogicalDecodingProcessRecord(LogicalDecodingContext *ctx, XLogReaderState *record)
{
	XLogRecordBuffer buf;

	buf.origptr = ctx->reader->ReadRecPtr;
	buf.endptr = ctx->reader->EndRecPtr;
	buf.record = record;

	/* cast so we get a warning when new rmgrs are added */
	switch ((RmgrIds) XLogRecGetRmid(record))
	{
			/*
			 * Rmgrs we care about for logical decoding. Add new rmgrs in
			 * rmgrlist.h's order.
			 */
		case RM_XLOG_ID:
			DecodeXLogOp(ctx, &buf);
			break;

		case RM_XACT_ID:
			DecodeXactOp(ctx, &buf);
			break;

		case RM_STANDBY_ID:
			DecodeStandbyOp(ctx, &buf);
			break;

		case RM_HEAP2_ID:
			DecodeHeap2Op(ctx, &buf);
			break;

		case RM_HEAP_ID:
			DecodeHeapOp(ctx, &buf);
			break;

		case RM_LOGICALMSG_ID:
			DecodeLogicalMsgOp(ctx, &buf);
			break;

		case RM_SMGR_ID:
		case RM_CLOG_ID:
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
		case RM_MULTIXACT_ID:
		case RM_RELMAP_ID:
		case RM_BTREE_ID:
		case RM_HASH_ID:
		case RM_GIN_ID:
		case RM_GIST_ID:
		case RM_SEQ_ID:
		case RM_SPGIST_ID:
		case RM_BRIN_ID:
		case RM_COMMIT_TS_ID:
		case RM_REPLORIGIN_ID:
		case RM_GENERIC_ID:
			/* just deal with xid, and done */
			ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(record),
									buf.origptr);
			break;
		case RM_NEXT_ID:
			elog(ERROR, "unexpected RM_NEXT_ID rmgr_id: %u", (RmgrIds) XLogRecGetRmid(buf.record));
	}
}
```





