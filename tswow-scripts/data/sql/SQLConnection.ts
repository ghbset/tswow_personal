/*
 * This file is part of tswow (https://github.com/tswow)
 *
 * Copyright (C) 2020 tswow <https://github.com/tswow/>
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
import * as fs from 'fs';
import * as mysql from 'mysql2';
import { queryToSql } from '../query/Query';
import { BuildArgs, datasetName, NodeConfig } from '../Settings';
import { SqlRow } from './SQLRow';
import { SqlTable } from './SQLTable';
import { translate } from './SQLTranslate';
import deasync = require('deasync');

export class PreparedStatement {
    private asyncStatement: any;

    readonly query: string;
    private early: any[][] = []
    private normal: any[][] = []
    private late: any[][] = []

    constructor(statement: string) {
        this.query = statement;
    }

    writeEarly(values: any[]) {
        this.early.push(values);
    }

    writeNormal(values: any[]) {
        this.normal.push(values);
    }

    writeLate(values: any[]) {
        this.late.push(values);
    }

    static clear(stmnt: PreparedStatement) {
        stmnt.early = []
        stmnt.normal = []
        stmnt.late = []
    }

    static setStatement(stmnt: PreparedStatement, asyncStatement: any) {
        stmnt.asyncStatement = asyncStatement;
    }

    static getStatement(stmnt: PreparedStatement) {
        return stmnt.asyncStatement;
    }
}

export class Connection {
    static end(connection: Connection) {
        if(connection.sync !== undefined)  {
            connection.sync.end();
            connection.sync = undefined;
        }

        if(connection.async !== undefined) {
            connection.async.end();
            connection.async = undefined;
        }
    }

    static connect(connection: Connection) {
        this.end(connection);
        if(NodeConfig.UsePooling) {
            connection.async = mysql.createPool(Object.assign({}, connection.settings, { enableKeepAlive: true}));
            connection.sync = mysql.createPool(Object.assign({}, connection.settings, { enableKeepAlive: true}));
        } else {
            connection.async = mysql.createConnection(connection.settings);
            connection.sync = mysql.createConnection(connection.settings);
            connection.async.connect((err)=>{
                if(!err) return;
                console.error(`Failed to connect with settings`,connection.settings,err)
                process.exit(-1);
            });
            connection.sync.connect((err)=>{
                if(!err) return;
                console.error(`Failed to connect with settings`,connection.settings,err)
                process.exit(-1);
            });
        }

        connection.syncQuery = deasync(connection.sync.query
            .bind(connection.sync));
    }

    protected settings: any;
    protected async: mysql.Pool | mysql.Connection | undefined;
    protected sync: mysql.Pool | mysql.Connection | undefined;
    protected syncQuery: any;

    constructor(obj: any) {
        this.settings = obj;
        this.settings.dateStrings = true;
        this.settings.multipleStatements = true;
    }

    protected statements: PreparedStatement[] = []
    protected early: string[] = [];
    protected normal: string[] = [];
    protected late: string[] = [];

    databaseName() {
        return this.settings.database;
    }

    read(query: string) {
        if(this.sync===undefined) {
            throw new Error(
                  `Tried to read from a disconnected adapter.\n`
                + `This typically indicates that your node_modules folder is corrupt. Try deleting it, re-run 'npm i' and restart TSWoW.\n`
                + `\n`
                + `If the problem persists, please report this as a bug.`
            );
        }
        SqlConnection.log(this.settings.database,query);
        return this.syncQuery(query);
    }

    prepare(statement: string) {
        let prep = new PreparedStatement(statement);
        this.statements.push(prep);
        return prep;
    }

    write(query: string) {
        this.normal.push(query);
    }

    writeEarly(query: string) {
        this.early.push(query);
    }

    writeLate(query: string) {
        this.late.push(query);
    }

    /** Row cap per bulk statement. Byte budget usually binds first. */
    private static readonly BULK_MAX_ROWS = 1000;
    /**
     * Payload budget per bulk statement, well under the server's
     * max_allowed_packet (64MB here). Wide tables — item_template has 139
     * columns, quest_template 106, both with long text — would blow a
     * fixed row count, so size the chunk by estimated bytes instead.
     * Anything that still overflows is caught and split by writeChunk.
     */
    private static readonly BULK_MAX_BYTES = 4 * 1024 * 1024;

    private static jsonSafe(_: string, value: any) {
        return typeof(value) == 'bigint' ? value.toString() : value;
    }

    /**
     * Rewrite a single-row prepared statement into its multi-row form:
     *   REPLACE INTO t (a,b) VALUES (?,?)   ->   REPLACE INTO t (a,b) VALUES ?
     *
     * Returns undefined for statements that have no VALUES tuple — notably the
     * prepared DELETEs, which are `WHERE pk = ? AND ...` and cannot be batched
     * this way. Those keep the original one-execute-per-row path.
     */
    private static toBulk(query: string): string | undefined {
        const m = /^(.*\bVALUES\s*)\(\s*\?\s*(?:,\s*\?\s*)*\)\s*;?\s*$/is.exec(query);
        return m ? `${m[1]}?` : undefined;
    }

    /** Cheap upper-bound estimate of a row's serialized size. */
    private static rowBytes(row: any[]): number {
        let n = 0;
        for(const v of row) {
            n += typeof(v) === 'string' ? v.length + 3 : 12;
        }
        return n;
    }

    private static chunkRows(rows: any[][]): any[][][] {
        const out: any[][][] = [];
        let cur: any[][] = [];
        let bytes = 0;
        for(const row of rows) {
            const rb = Connection.rowBytes(row);
            if(cur.length > 0
                && (cur.length >= Connection.BULK_MAX_ROWS
                    || bytes + rb > Connection.BULK_MAX_BYTES)) {
                out.push(cur);
                cur = [];
                bytes = 0;
            }
            cur.push(row);
            bytes += rb;
        }
        if(cur.length > 0) out.push(cur);
        return out;
    }

    private queryAsync(sql: string, values?: any, executor?: any): Promise<void> {
        return new Promise<void>((res,rej)=>{
            const target = executor !== undefined ? executor : this.async;
            if(target===undefined) {
                return rej(`Tried to apply while async adapter was disconnected`);
            }
            const cb = (err: any) => err ? rej(err) : res();
            if(values === undefined) {
                target.query(sql,cb);
            } else {
                target.query(sql,values,cb);
            }
        })
    }

    /**
     * Write one bulk chunk, bisecting on failure.
     *
     * Batching costs per-row error attribution, and a chunk can also exceed
     * max_allowed_packet despite the byte budget. Both are handled the same
     * way: on any error, split and retry the halves. At size 1 the offending
     * row is isolated and reported with its values — the same fidelity the
     * old per-row path gave, in O(log n) statements and only on the error path.
     */
    private async writeChunk(bulkSql: string, rows: any[][], executor?: any): Promise<void> {
        try {
            SqlConnection.log(this.settings.database,bulkSql);
            await this.queryAsync(bulkSql,[rows],executor);
        } catch(err) {
            if(rows.length <= 1) {
                if(err.message == undefined) err.message = ''
                err.message = `(For SQL "${bulkSql}" with values `
                    + `(${JSON.stringify(rows[0],Connection.jsonSafe)}))\n${err.message}`
                throw err;
            }
            const mid = rows.length >> 1;
            await this.writeChunk(bulkSql,rows.slice(0,mid),executor);
            await this.writeChunk(bulkSql,rows.slice(mid),executor);
        }
    }

    /**
     * Whether an apply runs as a single transaction on a single connection.
     *
     * The win is durability round-trips, not concurrency: under autocommit
     * every statement commits (and flushes) on its own, so a large apply pays
     * thousands of fsyncs where one COMMIT pays one. It also leaves the
     * destination database untouched when an apply fails halfway instead of
     * half-written. The cost is that statements no longer spread across the
     * pool. DDL in the queues still commits implicitly - MySQL has no
     * transactional DDL - so atomicity is best-effort around those.
     */
    private static readonly APPLY_IN_TRANSACTION = true;

    /**
     * Reserve one connection for the whole apply. Pools hand out a member;
     * a plain connection is its own executor and needs no release.
     */
    private acquireApplyConnection(): Promise<{executor: any, release: ()=>void}> {
        const adapter: any = this.async;
        if(adapter === undefined) {
            return Promise.reject(`Tried to apply while async adapter was disconnected`);
        }

        if(typeof(adapter.getConnection) !== 'function') {
            return Promise.resolve({executor: adapter, release: ()=>{}});
        }

        return new Promise<{executor: any, release: ()=>void}>((res,rej)=>{
            adapter.getConnection((err: any, connection: any)=>{
                if(err) {
                    return rej(err);
                }
                res({executor: connection, release: ()=>connection.release()});
            })
        })
    }

    async apply() {
        const applyStart = Date.now();
        const lease = await this.acquireApplyConnection();
        const executor = lease.executor;

        const doPriority = async (name: string) => {
            let priority: string[] = this[name]

            let promises = priority.map((x)=>new Promise<void>((res,rej)=>{
                SqlConnection.log(this.settings.database,x);

                executor.query(x,(err)=>{
                        if(err){
                            err.message = `(For SQL "${x}")\n`+err.message;
                            return rej(err);
                    } else {
                        return res();
                }})
            }))

            this.statements.forEach(x=>{
                const values = x[name] as any[][];
                if(values.length === 0) {
                    return;
                }

                // Batch anything with a VALUES tuple. One statement per ~1000
                // rows instead of one per row: measured 6,425 -> 56,022 rows/s
                // against this dataset's REPLACE workload.
                const bulk = Connection.toBulk(x.query);
                if(bulk !== undefined) {
                    for(const chunk of Connection.chunkRows(values)) {
                        promises.push(this.writeChunk(bulk,chunk,executor));
                    }
                    return;
                }

                // No VALUES tuple (prepared DELETEs) — keep per-row execute.
                values.forEach(y=>{
                    promises.push(new Promise<void>((res,rej)=>{
                        try {
                            executor.execute(x.query,y, err => {
                                if(err) {
                                    if(err.message == undefined) {
                                        err.message = ''
                                    }
                                    err.message += ` (For SQL "${x.query}" with values (${JSON.stringify(y,Connection.jsonSafe)}))\n${err.message}`
                                    rej(err);
                                } else {
                                    res();
                                }
                            })
                        } catch(err) {
                            err.message += ` (For SQL "${x.query}" with values (${JSON.stringify(y,Connection.jsonSafe)}))\n${err.message}`
                            rej(err)
                        }
                    }))
                })
            })

            return Promise.all(promises);
        }

        try {
            if(Connection.APPLY_IN_TRANSACTION) {
                await this.queryAsync('START TRANSACTION',undefined,executor);
            }
            await doPriority('early');
            await doPriority('normal');
            await doPriority('late');
            if(Connection.APPLY_IN_TRANSACTION) {
                await this.queryAsync('COMMIT',undefined,executor);
            }
        } catch(err) {
            if(Connection.APPLY_IN_TRANSACTION) {
                try {
                    await this.queryAsync('ROLLBACK',undefined,executor);
                } catch(rollbackError) {
                    // keep the original apply error, it's the useful one
                }
            }
            throw err;
        } finally {
            lease.release();
        }

        this.statements.forEach(x=>PreparedStatement.clear(x))
        this.early = [];
        this.normal = [];
        this.late = [];

        if(BuildArgs.USE_TIMER) {
            console.log(
                `[timer] SQL apply ${this.settings.database}: `
                + `${((Date.now()-applyStart)/1000).toFixed(2)}s`
            );
        }
    }
}

/**
 * Represents the global SQL connection.
 *
 * @motivation Since we already decided not to allow parallell patching, this might
 * just as well be static so we avoid having to pass around a script context to all
 * data structures. Can always change if we want to support paralellism later.
 */
export class SqlConnection {
    static additional: Connection[] = [];
    static logFile: number;

    /**
     * Per-table cost of reading the source database, so SqlTable can decide a
     * table is worth loading in full (see AUTO_EAGER_PRELOAD_* there) and so
     * --use-timer can say where a build's SQL time actually went.
     */
    private static sourceReadCount = 0;
    private static sourceReadMs = 0;
    private static sourceReadRows = 0;
    private static sourceReadByTable = new Map<string, {count: number, ms: number, rows: number}>();

    static getSourceReadStats(table: string) {
        return this.sourceReadByTable.get(table) || {count: 0, ms: 0, rows: 0};
    }

    private static recordSourceRead(table: string, ms: number, rows: number) {
        this.sourceReadCount++;
        this.sourceReadMs += ms;
        this.sourceReadRows += rows;

        const stats = this.sourceReadByTable.get(table);
        if(stats) {
            stats.count++;
            stats.ms += ms;
            stats.rows += rows;
        } else {
            this.sourceReadByTable.set(table,{count: 1, ms: ms, rows: rows});
        }
    }

    private static resetSourceReadProfile() {
        this.sourceReadCount = 0;
        this.sourceReadMs = 0;
        this.sourceReadRows = 0;
        this.sourceReadByTable.clear();
    }

    static printSourceReadProfile(maxTables: number = 15) {
        if(!BuildArgs.USE_TIMER) {
            return;
        }

        console.log(
            `[timer] SQL source reads: `
            + `queries=${this.sourceReadCount}, `
            + `rows=${this.sourceReadRows}, `
            + `total=${(this.sourceReadMs/1000).toFixed(2)}s`
        );

        Array.from(this.sourceReadByTable.entries())
            .sort(([,a],[,b])=>b.ms-a.ms)
            .slice(0,maxTables)
            .forEach(([table,stats])=>{
                console.log(
                    `[timer] SQL source table ${table}: `
                    + `queries=${stats.count}, `
                    + `rows=${stats.rows}, `
                    + `time=${(stats.ms/1000).toFixed(2)}s`
                );
            });
    }
    static log(db: string, sql: string) {
        if(BuildArgs.LOG_SQL) {
            fs.writeSync(this.logFile,`[${db}]: ${sql}\n`);
        }
    }

    static auth = new Connection(NodeConfig.DatabaseSettings('auth'));
    //static characters = new Connection(getDefaultSettings('characters'));
    static world_dst = new Connection(NodeConfig.DatabaseSettings('world',datasetName));
    static world_src = new Connection(NodeConfig.DatabaseSettings('world_source',datasetName))

    /**
     * Dedup guard for source-DB reads: remembers which exact SELECTs have
     * already been issued so getRows can skip repeating them.
     *
     * Flat Map keyed by `<table>\0<where>`. A Map (not a nested object) so
     * that eviction can be bounded and ordered — JS Maps iterate in insertion
     * order, which gives FIFO eviction for free.
     *
     * NOTE: this cache is purely an optimisation. Dropping an entry only costs
     * a redundant SELECT — SQLTable.filterInt re-filters whatever getRows
     * returns against its own row cache by primary key, so a re-read can never
     * duplicate or clobber an already-cached row.
     */
    private static query_cache = new Map<string, true>();
    private static readonly MAX_CACHE_SIZE = 1_000_000;
    /** Entries dropped per overflow. Partial eviction, never a full wipe. */
    private static readonly EVICT_BATCH = 10_000;

    protected static endConnection() {
        Connection.end(this.auth);
        //Connection.end(this.characters);
        Connection.end(this.world_src);
        Connection.end(this.world_dst);
        this.additional.forEach(x=>Connection.end(x));
        this.additional = [];
    }

    static connect() {
        this.endConnection();
        [this.auth,this.world_dst,this.world_src]
            .forEach((x)=>Connection.connect(x));
        // Clear query cache on reconnect
        this.clearQueryCache();
        this.resetSourceReadProfile();
    }

    static clearQueryCache() {
        this.query_cache.clear();
    }

    static getRows<C, Q, T extends SqlRow<C, Q>>(table: SqlTable<C, Q, T>, where: Q, first: boolean) {
        const whereSql = queryToSql(where, false);
        const whereLookup = whereSql + first;

        // Check cache for the query, don't repeat
        const cacheKey = `${table.name}\0${whereLookup}`;
        if(this.query_cache.has(cacheKey)) {
            return [];
        }

        // Bound the cache without discarding it wholesale. The previous
        // implementation cleared every entry on overflow, which put the cache
        // into a sawtooth (fill to the cap, wipe, refill) and collapsed the
        // steady-state hit rate on any build large enough to reach the cap.
        // Evicting the oldest batch keeps the other ~99% of entries live.
        if(this.query_cache.size >= this.MAX_CACHE_SIZE) {
            let evicted = 0;
            for(const key of this.query_cache.keys()) {
                this.query_cache.delete(key);
                if(++evicted >= this.EVICT_BATCH) break;
            }
        }

        this.query_cache.set(cacheKey, true);

        const sqlStr = `SELECT * FROM ${table.name} ${whereSql.length > 1 ? ` WHERE ${whereSql}` : ''} ${first ? 'LIMIT 1' : ''};`;
        const readStart = Date.now();
        const res = SqlConnection.querySource(sqlStr);
        this.recordSourceRead(table.name, Date.now()-readStart, Array.isArray(res) ? res.length : 0);
        const rowsOut: T[] = [];
        for (const row of res) {
            translate(table.name,row,'IN');
            const jsrow = SqlTable.createRow(table, row);
            rowsOut.push(jsrow);
        }
        return rowsOut;
    }

    static querySource(sql: string): any {
        return this.world_src.read(sql);
    }

    static allDbs() {
        return this.additional.concat([this.world_src,this.world_dst,this.auth]);
    }
}
