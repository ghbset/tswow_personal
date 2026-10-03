import * as crypto from 'crypto';
import * as fs from 'fs';
import { wfs } from "../../util/FileSystem";
import { resfp } from '../../util/FileTree';
import { ipaths } from "../../util/Paths";
import { wsys } from "../../util/System";
import { term } from '../../util/Terminal';
import { NodeConfig } from "../NodeConfig";
import { ChangeSet, PublishTarget } from "../PublishTarget";

/**
 * The 3.3.5a publish target: writes the change set into MPQ patch archives the
 * stock client reads from its `Data/` directory.
 *
 * Every run is a full package. Incremental packaging used to live here — it
 * wrote a small `<base>.inc.MPQ` containing only what changed since the last
 * full run, and relied on that file shadowing the base in the client's patch
 * load order. That is fine for a local dev client and wrong for distribution:
 * a fresh install has no base for it to shadow, the incremental grows until
 * the next full package resets it, and publishing the base on its own silently
 * ships content older than the last build.
 */

export interface PackageMeta {
    size: number;
    md5s: string[];
    filename: string;
    chunkSize: number;
}

export class Mpq335Target implements PublishTarget {
    readonly id = '3.3.5a';

    /**
     * Bucket the change set into one mpqbuilder listfile per target archive.
     *
     * The listfile format is one `<absolute source>\t<client-relative dest>`
     * line per file; `ChangeEntry.bucket` is the `Package.Mapping` group the
     * entry resolved to, which is exactly the archive it belongs in.
     */
    private static listfilesFor(changes: ChangeSet): { [mpq: string]: string } {
        const listfiles: { [mpq: string]: string } = {};
        for (const entry of changes.entries) {
            listfiles[entry.bucket] =
                (listfiles[entry.bucket] || '') + `${entry.src}\t${entry.dest}\n`;
        }
        return listfiles;
    }

    /**
     * Compute the meta entry (size + MD5 chunks + chunkSize) for a packed MPQ.
     *
     * The chunk hashes let a launcher verify or fetch an archive piecewise
     * rather than re-downloading it whole.
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

    async publish(changes: ChangeSet): Promise<void> {
        const dataset = changes.dataset;
        const listfiles = Mpq335Target.listfilesFor(changes);

        ipaths.package.mkdir();
        const chunkSize = NodeConfig.LauncherPatchChunkSize;
        let metas: PackageMeta[] = []
        term.debug('client', `Packaging ${Object.entries(listfiles).length}`)

        for (const [mpq, list] of Object.entries(listfiles)) {
            const baseFilename = `${dataset.fullName}.${mpq}`;
            const basePackageFile = ipaths.package.file(baseFilename);
            // Removed as well as the base: a leftover .inc from before
            // incremental packaging was dropped would still shadow the base in
            // the client's load order and serve stale files.
            const incPackageFile = ipaths.package.file(`${dataset.fullName}.${mpq}.inc`);

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
            metas.push(Mpq335Target.computeMpqMeta(basePackageFile, chunkSize));
        }

        if(metas.length > 0) {
            ipaths.package.join(`${dataset.fullName}.meta.json`).toFile().writeJson(metas)
        }
    }
}
