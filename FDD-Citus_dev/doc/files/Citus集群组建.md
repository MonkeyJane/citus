## 1. 创建用户

```shell
[root@center3 home]# adduser cituscluster
[root@center3 home]# passwd cituscluster
Changing password for user cituscluster.
New password: 
BAD PASSWORD: The password is shorter than 8 characters
Retype new password: 
passwd: all authentication tokens updated successfully.
```



## 2. 下载PgSQL 与 Citus源码

为了避免网络影响，直接从官网下载源码，传到环境上并解压

```shell
[root@center3 home]# su - cituscluster
[cituscluster@center3 ~]$ ll
total 4
drwxrwxrwx 5 cituscluster cituscluster 4096 Mar 18 21:56 citus-master
drwxrwxrwx 6 cituscluster cituscluster  331 Mar 18 21:55 postgresql-10.1
```



## 3. 编译安装 PgSQL

```shell
[cituscluster@center3 ~]$ cd postgresql-10.1/
[cituscluster@center3 postgresql-10.1]$ ll configure
total 1
-rwxrwxr-x  1 cituscluster cituscluster 495593 Mar 18 21:55 configure

# debug模式编译，方便后续GDB跟踪代码
[cituscluster@center3 postgresql-10.1]$ ./configure --prefix=/opt/pgsql-10.1/ --enable-debug

# root权限创建文件夹，存储pg编译结果
[cituscluster@center3 postgresql-10.1]$ su -
Password: 
Last login: Mon Mar 18 21:43:56 PDT 2019 on pts/0
[root@center3 ~]# mkdir /opt/pgsql-10.1
[root@center3 ~]# mkdir /opt/pgsql-10.1/bin
[root@center3 ~]# mkdir /opt/pgsql-10.1/include
[root@center3 ~]# mkdir /opt/pgsql-10.1/lib
[root@center3 ~]# mkdir /opt/pgsql-10.1/share
[root@center3 ~]# chown cituscluster -R /opt/pgsql-10.1/*
[root@center3 ~]# cd /opt/
[root@center3 opt]# ll
total 0
drwxr-xr-x  6 root root 56 Mar 18 22:10 pgsql-10.1
drwxr-xr-x. 2 root root  6 Sep  6  2017 rh
[root@center3 opt]# chown cituscluster -R /opt/pgsql-10.1
[root@center3 opt]# ll
total 0
drwxr-xr-x  6 cituscluster root 56 Mar 18 22:10 pgsql-10.1
drwxr-xr-x. 2 root         root  6 Sep  6  2017 rh
[root@center3 opt]# cd pgsql-10.1/
[root@center3 pgsql-10.1]# ll
total 0
drwxr-xr-x 2 cituscluster root 6 Mar 18 22:10 bin
drwxr-xr-x 2 cituscluster root 6 Mar 18 22:10 include
drwxr-xr-x 2 cituscluster root 6 Mar 18 22:10 lib
drwxr-xr-x 2 cituscluster root 6 Mar 18 22:10 share
[root@center3 pgsql-10.1]# logout

# 并行编译
[cituscluster@center3 postgresql-10.1]$ make -j2 && make install -j2
```



## 4. 配置环境变量

```shell
[cituscluster@center3 ~]$ vi .bashrc 

# 这里只设置了 $PGHOME $PGDATA
export PGHOME=/opt/pgsql-10.1
export PGDATA=~/data

# 将pg相关bin与lib添加到相应路径
export LD_LIBRARY_PATH=/lib64:$PGHOME/lib:$LD_LIBRARY_PATH
export PATH=$HOME/.local/bin:$HOME/bin:$PGHOME/bin:$PATH

[cituscluster@center3 ~]$ source .bashrc 
```



## 5. 编译安装Citus

```shell
[cituscluster@center3 ~]$ cd citus-master/

# 若configure提示libcurl错误，添加 without-libcurl 指令
[cituscluster@center3 citus-master]$ ./configure
If you have libcurl already installed, see config.log for details on the
failure. It is possible the compiler isn't looking in the proper directory.
Use --without-libcurl to disable anonymous statistics collection.

[cituscluster@center3 citus-master]$ ./configure --without-libcurl 

[cituscluster@center3 citus-master]$ make & make install
```



## 6. 初始化数据库

```shell
[cituscluster@center3 ~]$ initdb -D $PGDATA -E UTF8 --locale=C -U cituscluster
The files belonging to this database system will be owned by user "cituscluster".
This user must also own the server process.

The database cluster will be initialized with locale "C".
The default text search configuration will be set to "english".

Data page checksums are disabled.

creating directory /home/cituscluster/data ... ok
creating subdirectories ... ok
selecting default max_connections ... 100
selecting default shared_buffers ... 128MB
selecting dynamic shared memory implementation ... posix
creating configuration files ... ok
running bootstrap script ... ok
performing post-bootstrap initialization ... ok
syncing data to disk ... ok

WARNING: enabling "trust" authentication for local connections
You can change this by editing pg_hba.conf or using the option -A, or
--auth-local and --auth-host, the next time you run initdb.

Success. You can now start the database server using:

    pg_ctl -D /home/cituscluster/data -l logfile start

[cituscluster@center3 ~]$
```

## 7. 配置PgSQL





## 8. 创建流复制从库

### 8.1 设置主库配置项

```shell
[cituscluster@center3 data]$ vi postgresql.conf 
wal_level = logical             # minimal, replica, or logical
max_wal_senders = 10            # max number of walsender processes
                                # (change requires restart)
wal_keep_segments = 128         # in logfile segments, 16MB each; 0 disables
wal_sender_timeout = 60s        # in milliseconds; 0 disables
```



### 8.2 创建从库

### 8.2.1 PG10

```shell
# 以192.168.221.131(center3)为主库，在主机dns1上创建备库. 将以主库为例本在$PGDATA创建一个文件夹
[cituscluster@dns1 ~]$ pg_basebackup -F p --progress -D $PGDATA -h 192.168.221.131 -p 5432 -U cituscluster --password
Password: 
48293/48293 kB (100%), 1/1 tablespace
```

### 8.2.2 PG12

```shell
# 以192.168.221.131(center3)为主库，在主机dns1上创建备库. 将以主库为例本在$PGDATA创建一个文件夹
[cituscluster@dns1 ~]$ pg_basebackup -h 192.168.221.131 -p 5432 -U cituscluster -W -Fp -Xs -Pv -R -D $PGDATA
```



### 8.3 设置从库配置项

```shell
# 在已有postgresql.conf文件基础上修改配置项
[cituscluster@dns1 data]$ vi postgresql.conf
hot_standby = on                        # "off" disallows queries during recovery
                                        # (change requires restart)

# 拷贝recovery.conf文件并进行相应配置
[cituscluster@dns1 data]$ cp /opt/pgsql-10.1/share/recovery.conf.sample recovery.conf
[cituscluster@dns1 data]$ vi recovery.conf
recovery_target_timeline = 'latest'
standby_mode = on
primary_conninfo = 'host=192.168.221.131 port=5432 user=cituscluster password=123456'           # e.g. 'host=localhost port=5432'
```



## 9. 使能citus插件

```shell
# 在pg配置文件中使能citus
[cituscluster@center3 ~]$ vi data/postgresql.conf
shared_preload_libraries = 'citus' 

# 启动数据库
[cituscluster@center3 ~]$ pg_ctl start
pg_ctl: another server might be running; trying to start server anyway
waiting for server to start....2019-03-18 23:50:32.322 PDT [15588] LOG:  number of prepared transactions has not been configured, overriding
2019-03-18 23:50:32.322 PDT [15588] DETAIL:  max_prepared_transactions is now set to 400
2019-03-18 23:50:32.323 PDT [15588] LOG:  listening on IPv4 address "0.0.0.0", port 5432
2019-03-18 23:50:32.323 PDT [15588] LOG:  listening on IPv6 address "::", port 5432
2019-03-18 23:50:32.326 PDT [15588] LOG:  listening on Unix socket "/tmp/.s.PGSQL.5432"
2019-03-18 23:50:32.444 PDT [15589] LOG:  database system was interrupted; last known up at 2019-03-18 23:42:22 PDT
2019-03-18 23:50:32.510 PDT [15589] LOG:  entering standby mode
2019-03-18 23:50:32.576 PDT [15589] LOG:  redo starts at 0/4000028
2019-03-18 23:50:32.578 PDT [15589] LOG:  consistent recovery state reached at 0/4000130
2019-03-18 23:50:32.579 PDT [15588] LOG:  database system is ready to accept read only connections
2019-03-18 23:50:32.607 PDT [15594] FATAL:  database system identifier differs between the primary and standby
2019-03-18 23:50:32.607 PDT [15594] DETAIL:  The primary's identifier is 6650267640443108455, the standby's identifier is 6650267942423385425.
2019-03-18 23:50:32.623 PDT [15595] FATAL:  database system identifier differs between the primary and standby
2019-03-18 23:50:32.623 PDT [15595] DETAIL:  The primary's identifier is 6650267640443108455, the standby's identifier is 6650267942423385425.
 done
server started

# 登录数据库，创建citus扩展件
[cituscluster@center3 ~]$ psql -d postgres
psql (10.1)
Type "help" for help.

postgres=# create extension citus;
```

## 10. 备升主

```shell
[cituscluster@dn1 ~]$pg_ctl promote
```



## 11. 主降备

```shell
[cituscluster@gtm2 ~]$ pg_rewind --target-pgdata $PGDATA --source-server='host=192.168.221.135 port=5432 user=cituscluster dbname=postgres'

[cituscluster@gtm2 ~]$ touch $PGDATA/standby.signal
[cituscluster@gtm2 ~]$ vi postgresql.conf
primary_conninfo = 'host=192.168.221.135 port=5432 user=cituscluster password=123456' 
[cituscluster@gtm2 ~]$ pg_ctl start
```

