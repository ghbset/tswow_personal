import * as crypto from 'crypto';
import * as fs from 'fs';
import * as path from 'path';
import { commands } from "../util/Commands";
import { wfs } from "../util/FileSystem";
import { resfp } from '../util/FileTree';
import { ipaths } from "../util/Paths";
import { wsys } from "../util/System";
import { term } from '../util/Terminal';
import { util } from '../util/Util';
import { Addon } from "./Addon";
import { Datascripts } from "./Datascripts";
import { Dataset } from "./Dataset";
import { Identifier } from "./Identifiers";
import { NodeConfig } from "./NodeConfig";

export interface PackageMeta {
    size: number;
    md5s: string[];
    filename: string;
    chunkSize: number;
}

/**
 * One entry per file that landed in the base MPQ at the last full
 * package-client run. Used by the incremental packager to determine
 * which files have changed since that baseline.
 */
interface ManifestEntry {
    src: string;        // absolute source path on disk
    size: number;       // bytes at base-build time
    mtime: number;      // ms-since-epoch at base-build time
}

/** Per-MPQ snapshot persisted to disk after each full build. */
interface PackageBaseManifest {
    /** Bumped to 2 when MD5-chunk cache was added.
     *  Loader accepts 1 and 2; missing fields trigger fresh computation. */
    version: 1 | 2;
    base_mpq_filename: string;     // e.g. "default.dataset.B.MPQ.MPQ"
    base_build_timestamp: number;  // ms
    /** MD5-chunk cache for the base MPQ. Populated on full rebuild, reused
     *  by incremental runs (the base MPQ hasn't changed, so its chunks
     *  haven't either). Avoids re-hashing the ~5.7 GB base on every
     *  incremental — that step otherwise costs ~3-5 minutes per cycle.
     *  Cache is keyed by size + chunkSize; if either differs at read
     *  time, we recompute. */
    base_size?: number;
    base_chunkSize?: number;
    base_md5s?: string[];
    files: { [destPath: string]: ManifestEntry };
}

interface DiffResult {
    addedOrModified: { src: string; dest: string; size: number; mtime: number }[];
    removed: string[];
    addedBytes: number;
}

export class Package {
    // ===== Incremental-package (Phase 1) tunables ========================
    // If the would-be incremental MPQ exceeds this fraction of the base
    // MPQ's size, fall back to a full rebuild. Rationale: at that point
    // we're rewriting most of the archive anyway; full is cleaner and
    // resets the manifest baseline.
    private static INCREMENTAL_SIZE_FRACTION = 0.20;
    private static INCREMENTAL_SIZE_HARD_CAP = 1024 * 1024 * 1024; // 1 GB

    /** Where the per-MPQ manifest lives for a given dataset. */
    private static manifestDir(dataset: Dataset): string {
        return path.join(dataset.path.get(), '.tswow', 'package');
    }
    private static manifestPath(dataset: Dataset, mpq: string): string {
        return path.join(this.manifestDir(dataset), `${mpq}.manifest.json`);
    }
    private static loadManifest(dataset: Dataset, mpq: string): PackageBaseManifest | null {
        const p = this.manifestPath(dataset, mpq);
        if (!fs.existsSync(p)) return null;
        try {
            const obj = JSON.parse(fs.readFileSync(p, 'utf8'));
            if (obj.version !== 1 && obj.version !== 2) return null;
            return obj as PackageBaseManifest;
        } catch (_) { return null; }
    }
    private static saveManifest(dataset: Dataset, mpq: string, m: PackageBaseManifest): void {
        fs.mkdirSync(this.manifestDir(dataset), { recursive: true });
        fs.writeFileSync(this.manifestPath(dataset, mpq), JSON.stringify(m));
    }

    /**
     * Compute the diff between the just-built listfile (a string of
     * `src\tdest\n` lines) and the on-disk baseline manifest.
     *
     * - added/modified: dest is in newListfile but absent in baseline,
     *   OR (size, mtime) on disk differs from baseline.
     * - removed: dest is in baseline but absent from newListfile.
     *
     * Phase 1 deliberately does NOT support removals — any removed file
     * forces the caller to fall back to a full rebuild.
     */
    private static diffAgainstBaseline(
        newListfile: string,
        baseline: PackageBaseManifest
    ): DiffResult {
        const result: DiffResult = { addedOrModified: [], removed: [], addedBytes: 0 };
        const seen = new Set<string>();

        for (const line of newListfile.split(/\r?\n/)) {
            if (!line) continue;
            const tab = line.indexOf('\t');
            if (tab < 0) continue;
            const src = line.substring(0, tab);
            const dest = line.substring(tab + 1);
            seen.add(dest);
            let size = 0, mtime = 0;
            try {
                const st = fs.statSync(src);
                size = st.size;
                mtime = st.mtimeMs;
            } catch (_) { continue; }
            const base = baseline.files[dest];
            if (!base || base.size !== size || base.mtime !== mtime) {
                result.addedOrModified.push({ src, dest, size, mtime });
                result.addedBytes += size;
            }
        }
        for (const dest of Object.keys(baseline.files)) {
            if (!seen.has(dest)) result.removed.push(dest);
        }
        return result;
    }

    /**
     * Build a manifest snapshot from the just-written listfile, capturing
     * each source file's current (size, mtime). Written to disk after a
     * full base-MPQ build so the next package call has a baseline to
     * diff against.
     */
    private static buildManifestFromListfile(baseMpqFilename: string, listfile: string): PackageBaseManifest {
        const m: PackageBaseManifest = {
            version: 1,
            base_mpq_filename: baseMpqFilename,
            base_build_timestamp: Date.now(),
            files: {},
        };
        for (const line of listfile.split(/\r?\n/)) {
            if (!line) continue;
            const tab = line.indexOf('\t');
            if (tab < 0) continue;
            const src = line.substring(0, tab);
            const dest = line.substring(tab + 1);
            try {
                const st = fs.statSync(src);
                m.files[dest] = { src, size: st.size, mtime: st.mtimeMs };
            } catch (_) { /* missing source — exclude from manifest */ }
        }
        return m;
    }

    /**
     * Given an MPQ name and the path to its base file, return the
     * filename (without dataset prefix) that the incremental should
     * write to. Today this is "<base>.inc.MPQ" — by sitting alongside the
     * base in load order, it shadows the base when the WoW client picks
     * the later patch letter. Phase 1 always replaces this single file
     * (it grows cumulatively until the next full rebuild resets it).
     */
    private static incrementalMpqName(baseMpq: string): string {
        // base is e.g. "B.MPQ" → incremental is "B.MPQ.inc"
        // (full filename ends up as default.dataset.B.MPQ.inc.MPQ etc.)
        return `${baseMpq}.inc`;
    }

    /**
     * Compute the meta entry (size + MD5 chunks + chunkSize) for a packed
     * MPQ file. Shared between the full and incremental code paths.
     */
    private static computeMpqMeta(packageFile: any, chunkSize: number): PackageMeta {
        const meta: PackageMeta = {
            md5s: [],
            size: wfs.stat(packageFile).size,
            filename: packageFile.basename().get(),
            chunkSize
        };
        const handle = fs.openSync(resfp(packageFile), 'r');
        try {
            const buf = Buffer.alloc(chunkSize);
            while (true) {
                const nread = fs.readSync(handle, buf, 0, chunkSize, null);
                if (nread === 0) break;
                const data = nread < chunkSize ? buf.slice(0, nread) : buf;
                meta.md5s.push(crypto.createHash('md5').update(data).digest('hex'));
            }
        } finally {
            fs.closeSync(handle);
        }
        return meta;
    }

    /**
     * Build a PackageMeta for an unchanged base MPQ using chunk hashes
     * persisted in the v2 manifest. Skips the ~3–5 min re-hashing of the
     * 5.7 GB base file on every incremental run. Caller is responsible for
     * verifying baseline.base_size === current file size and
     * baseline.base_chunkSize === chunkSize before calling.
     */
    private static mpqMetaFromCache(packageFile: any, cachedMd5s: string[], chunkSize: number): PackageMeta {
        return {
            md5s: cachedMd5s.slice(),
            size: wfs.stat(packageFile).size,
            filename: packageFile.basename().get(),
            chunkSize,
        };
    }

    /**
     * Return the PackageMeta for an unchanged base MPQ, preferring the
     * cached chunks in `baseline` when its (size, chunkSize) still match
     * the on-disk file. On a miss (no cache, size drift, or chunk-size
     * change), recompute and persist back into the manifest so the next
     * run is fast. Only used on the incremental code path; the full
     * rebuild path computes + writes cache directly via the v2 manifest.
     */
    private static getBaseMeta(
          packageFile: any
        , chunkSize: number
        , baseline: PackageBaseManifest
        , dataset: Dataset
        , mpq: string
    ): PackageMeta {
        const currentSize = wfs.stat(packageFile).size;
        if (
               baseline.version === 2
            && baseline.base_md5s
            && baseline.base_size === currentSize
            && baseline.base_chunkSize === chunkSize
        ) {
            return Package.mpqMetaFromCache(packageFile, baseline.base_md5s, chunkSize);
        }
        const fresh = Package.computeMpqMeta(packageFile, chunkSize);
        baseline.version = 2;
        baseline.base_size = fresh.size;
        baseline.base_chunkSize = chunkSize;
        baseline.base_md5s = fresh.md5s.slice();
        Package.saveManifest(dataset, mpq, baseline);
        return fresh;
    }

    static async packageClient(dataset: Dataset, fullDBC: boolean, fullInterface: boolean, forceFullPackage = false) {
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
        if (Datascripts.isFresh(dataset)) {
            term.success('client', `Datascripts already fresh — skipping rebuild (saves ~30 min)`)
        } else {
            await Datascripts.build(dataset,['--no-shutdown']);
        }
        await Addon.build(dataset);

        // Step 1: Resolve mappings
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

        // Step 2: Build MPQ
        let listfiles: {[mpq: string]: string} = {}
        const appendListfile = (mod: string,value: string) => {
            let listfile = listfiles[mappings[mod]] || ''
            listfile+=value;
            listfiles[mappings[mod]] = listfile;
        }

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
                appendListfile('dbc',`${node.abs().get()}\tDBFilesClient\\${node.basename().get()}\n`)
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
                appendListfile('luaxml',`${node.abs().get()}\t${rel.split('/').join('\\')}\n`)
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
                    appendListfile(x.fullName, `${node.abs()}\t${node.relativeTo(x.assets.path).split('/').join('\\')}\n`)
                });
            })

        // Phase 1 incremental package: don't blindly delete every prior
        // dataset MPQ. We may want to keep the previous full B.MPQ around
        // while writing only a small B.MPQ.inc with the diff.
        ipaths.package.mkdir();
        const chunkSize = NodeConfig.LauncherPatchChunkSize;
        let metas: PackageMeta[] = []
        term.debug('client', `Packaging ${Object.entries(listfiles).length}`)

        for (const [mpq, list] of Object.entries(listfiles)) {
            const baseFilename = `${dataset.fullName}.${mpq}`;
            const incName = Package.incrementalMpqName(mpq);
            const incFilename = `${dataset.fullName}.${incName}`;
            const basePackageFile = ipaths.package.file(baseFilename);
            const incPackageFile  = ipaths.package.file(incFilename);

            const baseline = forceFullPackage
                ? null
                : Package.loadManifest(dataset, mpq);

            let didIncremental = false;

            // ---- Incremental path -----------------------------------------
            if (baseline && basePackageFile.exists()) {
                const baseSize = wfs.stat(basePackageFile).size;
                const diff = Package.diffAgainstBaseline(list, baseline);

                const sizeLimit = Math.min(
                    Math.floor(baseSize * Package.INCREMENTAL_SIZE_FRACTION),
                    Package.INCREMENTAL_SIZE_HARD_CAP
                );
                const trigger =
                    diff.removed.length > 0
                        ? `removal of ${diff.removed.length} file(s) (not supported in Phase 1)`
                        : diff.addedBytes > sizeLimit
                            ? `diff ${(diff.addedBytes / 1e6).toFixed(1)} MB exceeds threshold ${(sizeLimit / 1e6).toFixed(1)} MB`
                            : null;

                if (trigger !== null) {
                    term.log('client', `${mpq}: full rebuild — ${trigger}`);
                } else if (diff.addedOrModified.length === 0) {
                    term.success('client', `${mpq}: no changes since last build — keeping existing MPQs`);
                    // Push existing metas for both base + incremental (if either present).
                    if (basePackageFile.exists()) {
                        metas.push(Package.getBaseMeta(basePackageFile, chunkSize, baseline, dataset, mpq));
                    }
                    if (incPackageFile.exists()) {
                        metas.push(Package.computeMpqMeta(incPackageFile, chunkSize));
                    }
                    didIncremental = true;
                } else {
                    // Write only the changed files into the incremental MPQ.
                    // The previous inc MPQ (if any) is overwritten — its
                    // contents are cumulative since the last full build,
                    // so the new diff list already includes everything
                    // that's still drifted from base.
                    const incListfileText = diff.addedOrModified
                        .map(e => `${e.src}\t${e.dest}\n`)
                        .join('');
                    const listfileScratch = ipaths.bin.package.file(incPackageFile.get());
                    listfileScratch.write(incListfileText);
                    if (incPackageFile.exists()) incPackageFile.remove();
                    wsys.exec(
                          `"${ipaths.bin.mpqbuilder.mpqbuilder_exe.get()}"`
                        + ` ${listfileScratch.abs().get()}`
                        + ` ${incPackageFile.abs().get()}`
                        , 'inherit'
                    );
                    term.success('client',
                        `${mpq}: incremental ${diff.addedOrModified.length} files `
                        + `(${(diff.addedBytes / 1e6).toFixed(1)} MB) → ${incName}`);

                    // Base unchanged; meta carries both base + incremental.
                    metas.push(Package.getBaseMeta(basePackageFile, chunkSize, baseline, dataset, mpq));
                    metas.push(Package.computeMpqMeta(incPackageFile, chunkSize));
                    didIncremental = true;
                }
            }

            // ---- Full rebuild path ---------------------------------------
            if (!didIncremental) {
                // Remove prior outputs for this MPQ family.
                if (basePackageFile.exists()) basePackageFile.remove();
                if (incPackageFile.exists()) incPackageFile.remove();

                const listfileScratch = ipaths.bin.package.file(basePackageFile.get());
                listfileScratch.write(list);
                wsys.exec(
                      `"${ipaths.bin.mpqbuilder.mpqbuilder_exe.get()}"`
                    + ` ${listfileScratch.abs().get()}`
                    + ` ${basePackageFile.abs().get()}`
                    , 'inherit'
                );
                const baseMeta = Package.computeMpqMeta(basePackageFile, chunkSize);
                metas.push(baseMeta);

                // Snapshot the baseline manifest for future incremental diffs.
                // Persist base MD5 chunks so subsequent incremental runs can
                // skip the ~3–5 min re-hash of the unchanged 5.7 GB base MPQ.
                const fresh = Package.buildManifestFromListfile(baseFilename, list);
                fresh.version = 2;
                fresh.base_size = baseMeta.size;
                fresh.base_chunkSize = chunkSize;
                fresh.base_md5s = baseMeta.md5s.slice();
                Package.saveManifest(dataset, mpq, fresh);
            }
        }
        if(metas.length > 0) {
            ipaths.package.join(`${dataset.fullName}.meta.json`).toFile().writeJson(metas)
        }
    }

    static Command = commands.addCommand('package')

    static initialize() {
        term.debug('misc', `Initializing packages`)
        this.Command.addCommand(
              'client'
            , 'dataset --fullDBC --fullInterface --full-package'
            , 'Packages client data for the specified dataset.'
            + ' Pass --full-package to bypass the Phase-1 incremental'
            + ' packager and rewrite the full base MPQ + reset its manifest.'
            , async args => {
                const lower = args.map(x=>x.toLowerCase())
                const fullDBC = lower.includes('--fulldbc');
                const fullInterface = lower.includes('--fullinterface');
                const fullPackage = lower.includes('--full-package');
                await Promise.all(Identifier.getDatasets(
                      args
                    , 'MATCH_ANY'
                    , NodeConfig.DefaultDataset
                ).map(x=>this.packageClient(x,fullDBC,fullInterface,fullPackage)))
            }
        )
    }
}