import { makeMask, MaskCon } from "../../../data/cell/cells/MaskCell";
import { CellSystem } from "../../../data/cell/systems/CellSystem";
import { SQL } from "../../SQLFiles";
import { RaceMask } from "../Race/RaceType";
import { Class } from "./Class";

export class ClassStartInventory extends CellSystem<Class> {
    add(items: number, amount: number, races?: MaskCon<keyof typeof RaceMask>) {
        // playercreateinfo_item.race is a RACE ID, not a racemask. Iterate the supplied
        // racemask and write one row per race (racemask bit N -> race id N+1). The old code
        // wrote the raw mask as the race column, which only matched Human (mask 1 == race 1)
        // and silently broke every other race (e.g. Undead mask 16 != race 5).
        const mask = makeMask(RaceMask, races);
        for (let bit = 0; bit < 11; bit++) {
            if (mask & (1 << bit)) {
                SQL.playercreateinfo_item.add(bit + 1, this.owner.ID, items)
                    .amount.set(amount)
            }
        }
        return this.owner;
    }
}

