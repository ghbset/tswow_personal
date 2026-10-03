import { Dataset } from "./Dataset";

/**
 * What `package client` produces, before anything client-specific happens to it.
 *
 * Everything upstream of this point — datascripts, livescripts, SQL — is target
 * agnostic: datascripts emit DBC, which is an intermediate format rather than a
 * client format, and livescripts/SQL never leave the server. The client-facing
 * decision lives entirely in the publish step, which is why the change set is
 * modelled separately from whatever consumes it.
 */

/** Which part of the pipeline a change came from. */
export type ChangeKind = 'dbc' | 'luaxml' | 'asset';

export interface ChangeEntry {
    /** Absolute path of the built file on disk. */
    src: string;
    /**
     * Client-relative destination, in the backslash form the client expects
     * (e.g. `DBFilesClient\Spell.dbc`, `Interface\FrameXML\Foo.lua`).
     */
    dest: string;
    kind: ChangeKind;
    /**
     * Owning module's full name, or the literal `'dbc'` / `'luaxml'` for those
     * two pseudo-modules — matching the keys `Package.Mapping` accepts.
     */
    module: string;
    /**
     * The `Package.Mapping` group this entry resolved to (e.g. `A.MPQ`).
     * Meaningful to targets that bucket their output; ignorable by those that
     * don't.
     */
    bucket: string;
}

export interface ChangeSet {
    dataset: Dataset;
    /**
     * Only files that actually differ from the stock client, unless the caller
     * asked for everything via `--fullDBC` / `--fullInterface`. Ordering is
     * stable: dbc, then luaxml, then module assets in dataset module order.
     */
    entries: ChangeEntry[];
    /** True when DBCs were taken wholesale rather than diffed against source. */
    fullDBC: boolean;
    /** True when luaxml was taken wholesale rather than diffed against source. */
    fullInterface: boolean;
    /**
     * Bypass any incremental machinery the target implements. The 3.3.5a
     * target no longer has any — every package is full — so this is currently
     * a no-op, kept so `--full-package` stays accepted and for targets added
     * later that do work incrementally.
     */
    forceFull: boolean;
}

export interface PublishTarget {
    /** Value used in the `Client.Targets` dataset setting. */
    readonly id: string;
    publish(changes: ChangeSet): Promise<void>;
}

/** Registered publish targets, keyed by their `Client.Targets` id. */
const targets = new Map<string, PublishTarget>();

export function registerPublishTarget(target: PublishTarget) {
    targets.set(target.id, target);
}

export function getPublishTarget(id: string): PublishTarget | undefined {
    return targets.get(id);
}

export function publishTargetIds(): string[] {
    return Array.from(targets.keys());
}
