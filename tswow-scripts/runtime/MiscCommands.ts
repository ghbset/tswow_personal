import { commands } from "../util/Commands";
import { ipaths } from "../util/Paths";
import { term } from "../util/Terminal";
import { BuildCommand } from "./CommandActions";
import { Identifier } from "./Identifiers";
import { NodeConfig } from "./NodeConfig";

export class MiscCommands {
    static initialize() {
        term.debug('misc', `Initializing misc commands`)
        BuildCommand.addCommand(
              'all'
            , '(see arguments to build datscripts/addon/livescripts)'
            , 'Builds datascripts/addons/livescripts'
        , async args=>{
            let datasets = Identifier.getDatasets(args,'MATCH_ANY',NodeConfig.DefaultDataset)
            for(const dataset of datasets) {
                let runningClients = [dataset.client]
                let runningWorldservers = dataset.realms()

                // args.join(' ') — NOT `${args}`. Interpolating a string[]
                // joins with commas, so two or more flags arrive at the
                // subcommand as one unsplittable token ("--rebuild,--use-timer")
                // and every one of them is silently ignored. A single flag
                // happened to work, which is why this went unnoticed.
                const fwd = args.join(' ')

                await commands.sendCommand(`build data ${dataset.name} ${fwd} --no-restart`);
                await commands.sendCommand(`build addon ${dataset.name} ${fwd}`)
                // we've already built inlinescripts, skip them
                await commands.sendCommand(`build scripts ${dataset.name} ${fwd} --no-inline`)
                await commands.sendCommand(`build lua ${dataset.name} ${fwd} --no-inline`)

                await Promise.all(runningClients.map(x=>x.startup(NodeConfig.AutoStartClient)))
                let autorealms = NodeConfig.AutoStartRealms
                .map(x=>Identifier.getRealm(x))
                await Promise.all(runningWorldservers
                    .filter(x=>autorealms.find(y=>y.fullName===x.fullName))
                    .map(x=>x.start(x.lastBuildType)))
            }
        })

        commands.addCommand('check','','',(args)=>{
            return commands.sendCommand(`build data ${args} --readonly`)
        });

        commands.addCommand('revision','','',()=>{
            console.log(
                  `TSWoW Revision: ${ipaths.bin.revisions.tswow.readString().slice(0,7)}\n`
                + `TrinityCore Revision: ${ipaths.bin.revisions.trinitycore.readString().slice(0,7)}`
            )
        }).addAlias('version')
    }
}