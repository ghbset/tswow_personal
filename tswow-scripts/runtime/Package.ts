import { commands } from "../util/Commands";
import { wfs } from "../util/FileSystem";
import { term } from '../util/Terminal';
import { util } from '../util/Util';
import { Addon } from "./Addon";
import { Datascripts } from "./Datascripts";
import { Dataset } from "./Dataset";
import { Identifier } from "./Identifiers";
import { NodeConfig } from "./NodeConfig";
import { Mpq335Target } from "./publish/Mpq335Target";
import {
      ChangeEntry
    , ChangeSet
    , getPublishTarget
    , publishTargetIds
    , registerPublishTarget
} from "./PublishTarget";

export { PackageMeta } from "./publish/Mpq335Target";

/**
 * `package client` — turns everything the dataset has built into whatever the
 * client reads.
 *
 * Two stages:
 *
 *   1. Collect a target-agnostic *change set*: every built file that actually
 *      differs from the stock client. This is the only place that knows how to
 *      tell "ours" from "Blizzard's", and it is the same answer regardless of
 *      which client eventually consumes it.
 *   2. Hand that change set to each configured publish target.
 *
 * Everything else in tswow terminates server-side (livescripts, SQL) or emits an
 * intermediate format (datascripts emit DBC), so this is the single place where
 * the client target matters.
 */
export class Package {
    /**
     * Resolve each module — plus the `dbc` and `luaxml` pseudo-modules — to its
     * `Package.Mapping` bucket. Longest matching prefix wins; unmapped modules
     * are skipped with a warning, as before.
     */
    private static resolveMappings(dataset: Dataset): {[mod: string]: string} {
        let mapstr: [string,string[]][] = dataset.config.PackageMapping
            .map(x=>x.split(':'))
            .map(([mpq,modules])=>[mpq,modules.split(',')])
        let mappings: {[mod: string]: /*mpq*/ string } = {}
        let buildModules = dataset.modules().map(x=>x.fullName).concat('_build')
        buildModules.concat(['luaxml','dbc']).forEach(x=>{
            let bestMpq: string = "";
            let bestLen: number = 0;
            mapstr.forEach(([mpq,modules])=>{
                modules.forEach(mod=>{
                    if((mod == '*' || util.isModuleOrParent(x,mod)) && mod.length > bestLen) {
                        bestLen = mod.length;
                        bestMpq = mpq;
                    }
                });
            });
            if(bestLen != 0) {
                mappings[x] = bestMpq
            } else {
                term.log(
                      'dataset'
                    , `Module ${x} has no package mapping in dataset ${dataset.fullName}, will not build it`
                )
            }
        })
        return mappings;
    }

    /**
     * Walk the built dataset and collect everything that differs from the stock
     * client.
     *
     * A DBC or luaxml file byte-identical to its counterpart under
     * `dbc_source` / `luaxml_source` is stock and gets skipped — that filter is
     * what makes this a *change* set rather than a full dump, and it is exactly
     * the set a non-MPQ target would need to express as overrides.
     *
     * Emission order (dbc, then luaxml, then module assets in dataset module
     * order) is load-bearing for the MPQ target's incremental diffing, so it is
     * kept stable here rather than left to the consumer.
     */
    static collectChangeSet(
          dataset: Dataset
        , fullDBC: boolean
        , fullInterface: boolean
        , forceFull: boolean
    ): ChangeSet {
        const mappings = Package.resolveMappings(dataset);
        const entries: ChangeEntry[] = [];

        if(mappings['dbc']) {
            dataset.path.dbc.iterate('FLAT','FILES','FULL',node=>{
                if(!fullDBC) {
                    const rel = node.relativeTo(dataset.path.dbc);
                    const src = dataset.path.dbc_source.join(rel);
                    if(src.exists()) {
                        if(wfs.readBin(node).equals(wfs.readBin(src))) {
                            return;
                        }
                    }
                }
                entries.push({
                      src: node.abs().get()
                    , dest: `DBFilesClient\\${node.basename().get()}`
                    , kind: 'dbc'
                    , module: 'dbc'
                    , bucket: mappings['dbc']
                });
            });
        }

        if(mappings['luaxml']) {
            dataset.path.luaxml.iterate('RECURSE','FILES','FULL',node=>{
                const rel = node.relativeTo(dataset.path.luaxml);
                if(!fullInterface) {
                    const src = dataset.path.luaxml_source.join(rel);
                    if(src.exists()) {
                        if(wfs.readBin(node).equals(wfs.readBin(src))) {
                            return;
                        }
                    }
                }
                entries.push({
                      src: node.abs().get()
                    , dest: rel.split('/').join('\\')
                    , kind: 'luaxml'
                    , module: 'luaxml'
                    , bucket: mappings['luaxml']
                });
            });
        }

        dataset.modules()
            .filter(x=>mappings[x.fullName] && x.assets.exists())
            .forEach(x=>{
                x.path.assets.iterate('RECURSE','FILES','FULL',node=>{
                    let lower = node.toLowerCase();
                    if(
                           lower.endsWith('.png')
                        || lower.endsWith('.blend')
                        || lower.endsWith('.psd')
                        || lower.endsWith('.json')
                        || lower.endsWith('.dbc')
                    ) return;
                    entries.push({
                          src: `${node.abs()}`
                        , dest: node.relativeTo(x.assets.path).split('/').join('\\')
                        , kind: 'asset'
                        , module: x.fullName
                        , bucket: mappings[x.fullName]
                    });
                });
            })

        return { dataset, entries, fullDBC, fullInterface, forceFull };
    }

    /**
     * Which publish targets this dataset wants. Defaults to `3.3.5a` so a
     * dataset config that predates the setting behaves exactly as before.
     */
    private static targetsFor(dataset: Dataset): string[] {
        const configured = (dataset.config as any).ClientTargets as string[] | undefined;
        return (configured && configured.length > 0) ? configured : ['3.3.5a'];
    }

    static async packageClient(dataset: Dataset, fullDBC: boolean, fullInterface: boolean, forceFullPackage = false, forceRebuildData = false) {
        term.log('client', `Packaging client for ${dataset.name}`)
        // Datascripts.build is normally called unconditionally here, but
        // its `wow/data/index.js` run is the ~30-minute step that
        // dominates `package client` time. When nothing has changed since
        // the last build, that work is pure duplication: the dest DBCs +
        // SQL are already what they'd be after the run. Datascripts.isFresh
        // returns true only when every tracked input (compiled datascripts
        // JS, modules' dbcs/, sql-data/, the wow data lib) has an mtime
        // older than the dataset's freshness stamp; otherwise we rebuild
        // as before. Pass `--force-rebuild-data` to bypass the check.
        if (Datascripts.isFresh(dataset, forceRebuildData ? ['--force-rebuild-data'] : [])) {
            term.success('client', `Datascripts already fresh — skipping rebuild (saves ~30 min)`)
        } else {
            await Datascripts.build(dataset,['--no-shutdown']);
        }
        await Addon.build(dataset);

        const changes = Package.collectChangeSet(dataset, fullDBC, fullInterface, forceFullPackage);

        for (const id of Package.targetsFor(dataset)) {
            const target = getPublishTarget(id);
            if (!target) {
                term.error('client'
                    , `Unknown client target "${id}" in dataset ${dataset.fullName}`
                    + ` (known: ${publishTargetIds().join(', ')}) — skipping`);
                continue;
            }
            await target.publish(changes);
        }
    }

    static Command = commands.addCommand('package')

    static initialize() {
        term.debug('misc', `Initializing packages`)
        registerPublishTarget(new Mpq335Target());
        this.Command.addCommand(
              'client'
            , 'dataset --fullDBC --fullInterface --full-package --force-rebuild-data'
            , 'Packages client data for the specified dataset.'
            + ' --full-package is accepted but no longer does anything:'
            + ' incremental packaging was removed, so every run is a full one.'
            + ' --force-rebuild-data (alias --force) skips the freshness check'
            + ' and always re-runs the datascripts build.'
            , async args => {
                const lower = args.map(x=>x.toLowerCase())
                const fullDBC = lower.includes('--fulldbc');
                const fullInterface = lower.includes('--fullinterface');
                const fullPackage = lower.includes('--full-package');
                const forceRebuildData = lower.includes('--force-rebuild-data')
                    || lower.includes('--force');
                await Promise.all(Identifier.getDatasets(
                      args
                    , 'MATCH_ANY'
                    , NodeConfig.DefaultDataset
                ).map(x=>this.packageClient(x,fullDBC,fullInterface,fullPackage,forceRebuildData)))
            }
        )
    }
}
