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
import { inMemory } from '../query/Query';
import { BuildArgs } from '../Settings';
import { Row } from '../table/Row';
import { Table } from '../table/Table';
import { SqlConnection } from './SQLConnection';
import { SqlRow } from './SQLRow';

export type SqlRowCreator<C, Q, R extends SqlRow<C, Q>> = (table: SqlTable<C, Q, R>, obj: {[key: string]: any}) => R;

/**
 * Tables loaded in full the first time anything queries them.
 *
 * A build hits these thousands of times, and one `SELECT * FROM x` beats
 * thousands of round-trips even when most of the rows are never read. Tables
 * this dataset's source database doesn't have are skipped permanently on the
 * first failure (see eagerPreload), so the list is safe to keep broad.
 */
const EAGER_PRELOAD_TABLES = new Set<string>([
      'trainer_spell'
    , 'item_set_names'
    , 'gameobject'
    , 'item_template'
    , 'spell_dbc'
]);

/**
 * Anything the allowlist misses preloads itself once it has proven expensive:
 * a table that has already cost this many source queries *and* this much
 * wall-clock will not get cheaper by continuing one row at a time.
 */
const AUTO_EAGER_PRELOAD_QUERY_THRESHOLD = 32;
const AUTO_EAGER_PRELOAD_TIME_THRESHOLD_MS = 250;

export class SqlTable<C, Q, R extends SqlRow<C, Q>> extends Table<C, Q, R> {
    private cachedRows: {[key: string]: R} = {};
    private cachedFirst: R | undefined;
    /** Lazy indexes over primary-key prefixes, keyed by field signature. */
    private partialPkIndexes: {
        [signature: string]: {fields: string[], rows: Map<string, R[]>}
    } = {};
    /** True once cachedRows is known to hold every row of the table. */
    private hasLoadedAllRows = false;
    /** Set when a preload failed, so it is never attempted again. */
    private preloadFailed = false;
    protected rowCreator: SqlRowCreator<C, Q, R>;
    private _pkFields: string[] | undefined;

    /**
     * Primary-key field names for this table.
     *
     * These come from the @PrimaryKey decorator, which writes them to the row
     * *prototype* — so they are per-class metadata and identical for every row
     * of the table. Reading them used to mean allocating a throwaway row via
     * rowCreator on every single query (see isPkLookup); memoise instead.
     */
    private get pkFields(): string[] {
        if(this._pkFields === undefined) {
            this._pkFields = Row.primaryKeyFields(this.rowCreator(this,{}));
        }
        return this._pkFields;
    }

    private get cachedValues() {
        return Object.values(this.cachedRows);
    }

    static cachedRowCount(table: SqlTable<any, any, any>) {
        return Object.values(table.cachedRows).length;
    }

    static cachedValues(table: SqlTable<any, any, any>)  {
        return table.cachedValues;
    }

    static createRow<C, Q, R extends SqlRow<C, Q>>(table: SqlTable<C, Q, R>, obj: {[key: string]: any}): R {
        return table.rowCreator(table, obj);
    }

    static addRow<C, Q, R extends SqlRow<C, Q>>(table: SqlTable<C, Q, R>, row: R) {
        table.addRow(row);
    }

    constructor(name: string, rowCreator: SqlRowCreator<C, Q, R>) {
        super(name);
        this.rowCreator = rowCreator;
    }

    first(): R {
        if (this.cachedFirst) {
            return this.cachedFirst;
        }
        this.cachedFirst = this.filterInt({} as any, true)[0];
        if (!this.cachedFirst) {
            this.cachedFirst = this.rowCreator(this, {});
        }
        return this.cachedFirst;
    }

    queryAll(where: Q): R[] {
        return this.filterInt(where, false);
    }

    private isPkLookup(where: Q): string {
        let fields: string[] = this.pkFields;
        if(fields.length != Object.entries(where).length) {
            return undefined;
        }
        for(let field of fields) {
            if(where[field] === undefined) {
                return undefined;
            }

            if(typeof(where[field]) == 'object') {
                return undefined;
            }
        }
        return fields.map(x=>where[x]).join('_')
    }

    /**
     * The query's fields, if they form a strict subset of the primary key.
     *
     * Queries like {SpellID: x} against a (SpellID, EffectIndex) key are the
     * common shape once a table is fully cached, and the exact shape isPkLookup
     * cannot answer - they used to scan every cached row. Returns undefined for
     * anything an equality index can't answer: qany/qall wrappers, relations
     * (>, <, IN, ...), and queries covering the whole key.
     */
    private partialPkLookupFields(where: Q): string[] | undefined {
        if(where === undefined || where === null || typeof(where) !== 'object') {
            return undefined;
        }
        if((where as any).isAnyQuery || (where as any).isAllQuery) {
            return undefined;
        }

        const whereFields = Object.keys(where as any);
        const pkFields = this.pkFields;
        if(whereFields.length === 0 || whereFields.length >= pkFields.length) {
            return undefined;
        }

        const fields = pkFields.filter(x => Object.prototype.hasOwnProperty.call(where, x));
        if(fields.length !== whereFields.length) {
            return undefined;
        }
        for(const field of fields) {
            const value = where[field];
            if(value === undefined || value === null || typeof(value) === 'object') {
                return undefined;
            }
        }
        return fields;
    }

    /**
     * Index key for a set of fields on a row or a query.
     *
     * Values are stringified the way isPkLookup and Row.fullKey already build
     * their keys: a query passing '5' where the row holds 5 has to land in the
     * same bucket, or the index would answer "no rows" for a query the database
     * would have matched.
     */
    private partialPkKey(fields: string[], obj: any): string {
        return fields.map(field => {
            let value = obj[field];
            if(value && typeof(value) === 'object' && value.isCell) {
                value = value.get();
            }
            return typeof(value) === 'boolean' ? (value ? '1' : '0') : String(value);
        }).join('\u0000');
    }

    /**
     * Rows matching a primary-key-prefix query, or undefined if the index
     * can't answer it. Only valid once every row is cached - a partial cache
     * would turn a miss into a wrong empty result instead of a database read.
     */
    private partialPkMatches(where: Q): R[] | undefined {
        if(!this.hasLoadedAllRows) {
            return undefined;
        }
        const fields = this.partialPkLookupFields(where);
        if(!fields) {
            return undefined;
        }

        const signature = fields.join('\u0000');
        let index = this.partialPkIndexes[signature];
        if(!index) {
            index = {fields, rows: new Map<string, R[]>()};
            for(const row of this.cachedValues) {
                const key = this.partialPkKey(fields, row);
                const rows = index.rows.get(key);
                if(rows) {
                    rows.push(row);
                } else {
                    index.rows.set(key, [row]);
                }
            }
            this.partialPkIndexes[signature] = index;
        }
        return index.rows.get(this.partialPkKey(fields, where)) || [];
    }

    /** True for a `queryAll({})`, whose result set is the entire table. */
    private isUnfilteredQuery(where: Q, firstOnly: boolean) {
        if(firstOnly || where === undefined || where === null) {
            return false;
        }
        if(typeof(where) !== 'object') {
            return false;
        }
        if((where as any).isAnyQuery || (where as any).isAllQuery) {
            return false;
        }
        return Object.keys(where as any).length === 0;
    }

    private eagerPreload(reason: string) {
        const start = Date.now();
        let rows: R[];
        try {
            rows = SqlConnection.getRows(this, {} as any, false);
        } catch(err) {
            // A table the allowlist names but this source database doesn't
            // have. Nothing else about the table changes - it just keeps using
            // the per-query path.
            this.preloadFailed = true;
            if(BuildArgs.USE_TIMER) {
                console.log(
                    `[timer] Eager SQL preload ${this.name}: skipped `
                    + `(${err && err.message ? err.message : err})`
                );
            }
            return;
        }

        rows.filter(x => !this.cachedRows[Row.fullKey(x)])
            .forEach(x => this.addRow(x));
        this.hasLoadedAllRows = true;

        if(BuildArgs.USE_TIMER) {
            console.log(
                `[timer] Eager SQL preload ${this.name}: `
                + `rows=${rows.length}, `
                + `reason=${reason}, `
                + `time=${((Date.now()-start)/1000).toFixed(2)}s`
            );
        }
    }

    private maybePreload() {
        if(this.hasLoadedAllRows || this.preloadFailed) {
            return;
        }

        if(EAGER_PRELOAD_TABLES.has(this.name)) {
            this.eagerPreload('allowlist');
            return;
        }

        const stats = SqlConnection.getSourceReadStats(this.name);
        if(stats.count >= AUTO_EAGER_PRELOAD_QUERY_THRESHOLD
            && stats.ms >= AUTO_EAGER_PRELOAD_TIME_THRESHOLD_MS) {
            this.eagerPreload(
                `${stats.count} queries/${(stats.ms/1000).toFixed(2)}s`
            );
        }
    }

    private filterInt(where: Q, firstOnly = false): R[] {
        this.maybePreload();

        // Try looking up using only primary key
        let pkLookup = this.isPkLookup(where);
        let cacheMatches: R[] = []
        if(pkLookup) {
            let row = this.cachedRows[pkLookup];
            if(row) {
                return [row];
            }
            if(this.hasLoadedAllRows) {
                return [];
            }
        } else {
            const indexed = this.partialPkMatches(where);
            if(indexed) {
                return firstOnly ? indexed.slice(0,1) : indexed.slice();
            }
            for(let key in this.cachedRows) {
                let value = this.cachedRows[key];
                if(inMemory(where, value)) {
                    cacheMatches.push(value);
                    if(firstOnly) {
                        return cacheMatches;
                    }
                }
            }
            if(this.hasLoadedAllRows) {
                return cacheMatches;
            }
        }

        const dbMatches = SqlConnection.getRows(this, where, firstOnly)
            .filter(x => !this.cachedRows[Row.fullKey(x)]);
        dbMatches.forEach(x => this.addRow(x));
        if(this.isUnfilteredQuery(where, firstOnly)) {
            this.hasLoadedAllRows = true;
        }
        const matches = cacheMatches.concat(dbMatches);
        return firstOnly ? matches.slice(0,1) : matches;
    }

    addRow(row: R) {
        const fullKey = Row.fullKey(row);
        const previous = this.cachedRows[fullKey];
        this.cachedRows[fullKey] = row;

        // Keep any index that has already been built in sync, so a row created
        // by a datascript is visible to the same queries that would find it in
        // the database.
        for(const signature in this.partialPkIndexes) {
            const index = this.partialPkIndexes[signature];
            const key = this.partialPkKey(index.fields, row);
            const rows = index.rows.get(key);
            if(!rows) {
                index.rows.set(key, [row]);
                continue;
            }
            if(previous) {
                const previousIndex = rows.indexOf(previous);
                if(previousIndex >= 0) {
                    rows.splice(previousIndex, 1);
                }
            }
            rows.push(row);
        }
    }

    // TODO/sqltable
    protected writeToFile(sqlfile: string): string {
        const dirtyRows = this.cachedValues.filter(SqlRow.isDirty);
        if (dirtyRows.length === 0) { return sqlfile; }
        return sqlfile + `--${this.name}\n` + dirtyRows.map(x => SqlRow.getSql(x)).join('\n') + '\n';
    }

    static writeSQL(table: SqlTable<any,any,any>) {
        // Very stupid
        let dummyRow: SqlRow<any,any>
        for(let row in table.cachedRows) {
            dummyRow = table.cachedRows[row];
            break;
        }
        if(!dummyRow) {
            return;
        }

        const normalQuery = SqlRow.generatePreparedStatement(dummyRow);
        const deleteQuery = SqlRow.generatePreparedDeleteStatement(dummyRow);

        let normalStatement = SqlConnection.world_dst.prepare(normalQuery)
        let deleteStatement = SqlConnection.world_dst.prepare(deleteQuery)

        SqlConnection.world_dst.prepare(table.rowCreator(table,{}))

        let values = table.cachedValues.filter(SqlRow.isDirty)

        values.forEach((x: SqlRow<any,any>)=>{
                if(x.isDeleted()) {
                    deleteStatement.writeNormal(SqlRow.getPreparedDeleteStatement(x))
                } else {
                    normalStatement.writeNormal(SqlRow.getPreparedStatement(x))
                }
                //SqlConnection.world_dst.write(SqlRow.getSql(x))
            });
        table.cachedRows = {};
        table.partialPkIndexes = {};
        table.cachedFirst = undefined;
        table.hasLoadedAllRows = false;
    }
}
