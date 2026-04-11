/*
 * This file is part of tswow (https://github.com/tswow/).
 * Copyright (C) 2020 tswow <https://github.com/tswow/>
 *
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
#include "TSIncludes.h"
#include "TSDynObj.h"
#include "TSUnit.h"
#include "TSAura.h"
#include "TSSpellInfo.h"

#include "DynamicObject.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "SpellAuras.h"

TSDynObj::TSDynObj(DynamicObject* obj)
    : TSWorldObject(obj)
    , obj(obj)
{
}

void TSDynObj::Remove()
{
    obj->Remove();
}

void TSDynObj::SetDuration(int32 newDuration)
{
    obj->SetDuration(newDuration);
}

TSUnit TSDynObj::GetCaster()
{
    return TSUnit(obj->GetCaster());
}

TSNumber<uint32> TSDynObj::GetSpellId()
{
    return obj->GetSpellId();
}

TSSpellInfo TSDynObj::GetSpellInfo()
{
    return TSSpellInfo(sSpellMgr->GetSpellInfo(obj->GetSpellId()));
}

TSNumber<int32> TSDynObj::GetDuration()
{
    return obj->GetDuration();
}

void TSDynObj::SetAura(TSAura aura)
{
    obj->SetAura(aura.aura);
}

void TSDynObj::RemoveAura()
{
    obj->RemoveAura();
}
