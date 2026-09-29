 export interface ClientPatch {
    address: number
    values: number[]
}

export interface ClientPatchCat {
    name: string
    patches: ClientPatch[]
}

export function patch(name: string, patches: [number,number[]][]): ClientPatchCat
{
    return {name,patches:patches.map(x=>({address:x[0],values:x[1]}))}
}

/**
 * How much to raise the camera target, in game units (~1 unit = 1 yard).
 * The player model is ~2 yards tall and the camera target sits around chest
 * height, so 0.5 lifts it roughly to the shoulders. Change this value and
 * rebuild - the patch bytes are recomputed from it.
 */
export const CAMERA_HEIGHT_OFFSET = 0.5

/**
 * Raise the camera target Z in Camera_Update, immediately after
 * Camera_GetTargetPosition returns. The target is at [EBP-0x34]=X,
 * [EBP-0x30]=Y, [EBP-0x2c]=Z; we add a constant to Z.
 *
 * We overwrite the 6-byte `FLD dword ptr [0x9f1670]` at VA 0x6070cb with a
 * jump to a code cave, do the add there, run the displaced FLD, and jump back.
 *
 * The cave is a run of alignment NOPs at file 0x37421a (VA 0x774e1a). It is
 * deliberately NOT the one a naive "scan .text backwards for padding" search
 * finds: that lands at file 0x5dd7b3 / VA 0x9de3b3, which is exactly
 * `.text`'s VirtualSize boundary (0x5dd3b3) - the first byte past the end of
 * the mapped section. Bytes there exist in the file but aren't reliably mapped
 * at runtime, so a jump into it can land on zeros. This cave sits well inside
 * the mapped range, and clear of the client-extensions cave at 0x3738b8.
 *
 * Offsets are file offsets into a clean 3.3.5a Wow.exe
 * (md5 45892bdedd0ad70aed4ccd22d9fb5984), matching the rest of this file.
 */
export function cameraHeightPatch(heightAdd: number): ClientPatchCat[] {
    const PATCH_SITE   = 0x2064cb   // VA 0x6070cb
    const CAVE_OFF     = 0x37421a   // VA 0x774e1a
    const CAVE_VA      = 0x774e1a
    const PATCH_VA     = 0x6070cb
    const RETURN_VA    = 0x6070d1   // instruction after the displaced FLD
    const CAVE_LEN     = 23         // code length before the float constant
    const HEIGHT_VA    = CAVE_VA + CAVE_LEN

    const u32 = (v: number) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF]
    const f32 = (v: number) => {
        const b = Buffer.alloc(4); b.writeFloatLE(v, 0); return Array.from(b)
    }

    const cave = [
        0xD9, 0x05, ...u32(HEIGHT_VA),          // FLD  dword [heightAdd]
        0xD8, 0x45, 0xD4,                       // FADD dword [EBP-0x2c]   (target Z)
        0xD9, 0x5D, 0xD4,                       // FSTP dword [EBP-0x2c]
        0xD9, 0x05, 0x70, 0x16, 0x9F, 0x00,     // FLD  dword [0x9f1670]   (displaced original)
        0xE9, ...u32((RETURN_VA - (CAVE_VA + CAVE_LEN)) >>> 0),  // JMP back
        ...f32(heightAdd),
    ]

    const jump = [
        0xE9, ...u32((CAVE_VA - (PATCH_VA + 5)) >>> 0),
        0x90,                                   // NOP the 6th displaced byte
    ]

    return [patch('camera-height', [
        [PATCH_SITE, jump],
        [CAVE_OFF,   cave],
    ])]
}

/**
 * Ground decals ("projected textures" - Consecration, Death and Decay, unit
 * blob shadows) draw a full circle on flat ground but only a horizontal band
 * on a slope, with the uphill and downhill parts of the circle missing. These
 * four values control the fix below; the patch bytes are recomputed from them.
 *
 * Every one of them was arrived at by testing in game. See decalSlopePatch().
 */
export const DECAL_EXT_DOWN  = 30    // yd the projector box reaches below the effect
export const DECAL_EXT_UP    = 3     // ...and above it. More than a few yd up paints roofs.
export const DECAL_CULL_DEG  = 60    // surfaces within this of HORIZONTAL keep the decal
export const DECAL_KEEP_VERT = 0.25  // ...as do surfaces within this of EXACTLY VERTICAL

/**
 * Make spell ground decals drape over the whole surface they are projected
 * onto instead of showing as a band. Ported verbatim (byte-for-byte verified,
 * see below) from clientedits/wow335a-decal-slope-fix.sh, which is the record
 * of how each address and constant was found.
 *
 * Three independent things are wrong, so this emits three separately
 * toggleable patches. THEY ARE A SET, and 'decal-height-fade' is the
 * prerequisite: with the fade in place the other two change nothing visible,
 * because the fade cuts the decal off before they ever apply.
 *
 *  1. 'decal-height-fade'   Texture 1 of the decal is a 1-D ramp indexed by
 *     world height at a fixed 0.25/yard, so `u` walks off the usable part of
 *     the ramp within a couple of yards of the caster's own height and the
 *     decal fades out. That narrow vertical window IS the band. The 0.25 is
 *     global - byte-identical for a 23-yard spell decal and a 3.5-yard blob
 *     shadow - so it cannot scale with the effect. At the decal draw call we
 *     zero c8.xyz and set c8.w to 0.5, collapsing texcoord1 to the fixed texel
 *     (0.5, 0.5) for every vertex of every batch. c8 is vertex shader constant
 *     8 in the CGx shadow buffer at 0x00C5EFE8 + N*16.
 *
 *     The pin MUST be a literal, not a computed value. An earlier version
 *     derived it from c8.xyz . t, t being the .w column of c31..c33 - but r0
 *     in the shader is VIEW space, not world space, so t is the view-space
 *     image of the batch's local origin and moves with the camera. That pinned
 *     a different texel per chunk and read as scattered fragments in game. Do
 *     not "improve" this back into a computation.
 *
 *  2. 'decal-aabb-extent'   The projector's world AABB is built around the
 *     effect and every Z gate in the collection path reads it, so ground
 *     outside that height band is rejected before triangles are generated.
 *     Opened ASYMMETRICALLY: far down, barely up. Slopes fall away below the
 *     caster; anything well above is a roof. An early version opened Z to
 *     +-20000 and painted an entire graveyard crypt, roof and underside.
 *
 *  3. 'decal-angle-cull'    Removing the fade exposes that the height ramp was
 *     also doing duty as a cheap surface-angle fade: the projection is planar
 *     and the vertex stream carries no normals, so nothing downstream can tell
 *     a floor from a wall. Both triangle builders already compute Nz and
 *     compare it against ZERO, which only rejects downward-facing triangles -
 *     a vertical wall (Nz == 0) sails through. Replaced with a scale-invariant
 *     angle test against the full normal length.
 *
 *     It is a BAND, not a threshold: accept near-horizontal (floors, slopes)
 *     AND near-exactly-vertical (flat walls), reject in between. Flat walls
 *     taking the decal looks deliberate; the wedge between is where curved and
 *     tapered pillars live. DECAL_KEEP_VERT is narrow for a measured reason -
 *     the crypt's walls are exactly axis-aligned while its decorative pillars
 *     carry about half a degree of modelled taper. At 1.5 deg both keep the
 *     decal and the pillars smear; at 0.25 only the walls do. Widening it
 *     undoes that. (A threshold rejecting ALL vertical faces would be 0.)
 *
 *     This cannot affect terrain: ground goes through the builders'
 *     heightfield branch, which copies vertices with no winding test at all.
 *     The test only ever runs on WMO/M2 geometry.
 *
 * Addresses are VAs in a clean 3.3.5a Wow.exe (md5
 * 45892bdedd0ad70aed4ccd22d9fb5984), converted to file offsets here.
 *
 * Two code caves are used, both verified all-zero in the clean exe AND in the
 * fully tswow-patched exe, and verified non-overlapping with every other patch
 * in this file (enabled or not):
 *
 *  - .zdata (VA 0x00DD1000 / file 0x0072CE00, 4096 bytes, characteristics
 *    0xE0000040 = RWX). Nothing in the image references above 0x00DD1C75.
 *    Holds the UV cave, the angle cave and all four float constants.
 *  - .text tail slack (VA 0x009DE3B3 / file 0x005DD7B3, 77 zero bytes), which
 *    holds the two 31-byte AABB caves. NOTE: cameraHeightPatch() above
 *    deliberately avoids this run because it starts exactly at .text's
 *    VirtualSize boundary (0x5DD3B3). It is nonetheless mapped and executable:
 *    the loader copies min(SizeOfRawData, align(VirtualSize)) = min(0x5DD400,
 *    0x5DE000) = 0x5DD400 bytes, so file-backed data reaches VA 0x009DE400 and
 *    both caves (ending at 0x009DE3F1) fall inside it. Only 15 bytes remain
 *    spare there - a third cave needs somewhere else, e.g. .zdata 0x00DD1F00.
 *
 * Everything here also stays below the 0x758C00 truncation that
 * Client.applyExePatches() performs when 'client-extensions' is enabled (the
 * highest byte written is 0x0072DDE8).
 */
export function decalSlopePatch(
      extDown: number, extUp: number, cullDeg: number, keepVert: number
): ClientPatchCat[] {
    // .text  VA 0x00401000 @ file 0x00000400  =>  file = VA - 0x400C00
    // .zdata VA 0x00DD1000 @ file 0x0072CE00
    const toff = (va: number) => va - 0x400C00
    const zoff = (va: number) => va - 0x00DD1000 + 0x0072CE00

    const u32 = (v: number) =>
        [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF]
    const f32 = (v: number) => {
        const b = Buffer.alloc(4); b.writeFloatLE(v, 0); return Array.from(b)
    }
    const nops = (n: number) => new Array(n).fill(0x90) as number[]

    // --- 1. height fade ----------------------------------------------------
    const VS_CONST     = 0x00C5EFE8   // CGx vertex shader constant shadow buffer
    const VS_DIRTY_MIN = 0x00C5FFEC
    const VS_DIRTY_MAX = 0x00C5FFE8
    const DRAW1        = 0x007E3CEA   // orig: E8 51 ED C9 FF
    const DRAW2        = 0x007E41FB   // orig: E8 40 E8 C9 FF
    const DRAW_TARGET  = 0x00482A40   // what those two CALLs originally reached
    const UVCAVE       = 0x00DD1D88

    const uvBody = [
        0x60, 0x31, 0xC0,                          // pushad; xor eax,eax
        0xA3, ...u32(VS_CONST + 128),              // mov [c8.x], 0
        0xA3, ...u32(VS_CONST + 132),              // mov [c8.y], 0
        0xA3, ...u32(VS_CONST + 136),              // mov [c8.z], 0
        0xB8, 0x00, 0x00, 0x00, 0x3F,              // mov eax, 0.5f
        0xA3, ...u32(VS_CONST + 140),              // mov [c8.w], eax
        // widen the backend's dirty range to include register 8, or it never pushes it
        0xA1, ...u32(VS_DIRTY_MIN),                // mov eax,[dirtyMin]
        0x83, 0xF8, 0x08,                          // cmp eax, 8
        0x76, 0x0A,                                // jbe +10
        0xC7, 0x05, ...u32(VS_DIRTY_MIN), 0x08, 0x00, 0x00, 0x00,
        0xA1, ...u32(VS_DIRTY_MAX),                // mov eax,[dirtyMax]
        0x83, 0xF8, 0x08,                          // cmp eax, 8
        0x73, 0x0A,                                // jae +10
        0xC7, 0x05, ...u32(VS_DIRTY_MAX), 0x08, 0x00, 0x00, 0x00,
        0x61,                                      // popad
    ]
    const uvCave = [
        ...uvBody,
        0xE9, ...u32((DRAW_TARGET - (UVCAVE + uvBody.length + 5)) >>> 0),
    ]

    // --- 2. vertical extent ------------------------------------------------
    const EXT1       = 0x007E3AB2   // orig: 8B 7D 14 8D 45 FC 50
    const EXT2       = 0x007E3E92   // orig: 8B 7D 14 8D 45 F8 50
    const EXTCAVE1   = 0x009DE3B3
    const EXTCAVE2   = 0x009DE3D2   // = EXTCAVE1 + 31
    const EXT_K_DOWN = 0x00DD1FE0
    const EXT_K_UP   = 0x00DD1FE4

    // esi = the AABB (set at 0x007E3AAE). Runs the displaced `mov edi,[ebp+14]`,
    // widens minZ/maxZ, then runs the displaced LEA and returns; the call site
    // keeps the original PUSH EAX.
    const extCave = (which: 1 | 2) => [
        0x8B, 0x7D, 0x14,                          // mov  edi,[ebp+0x14]   (displaced)
        0xD9, 0x46, 0x08,                          // fld  dword [esi+0x08] (minZ)
        0xD8, 0x25, ...u32(EXT_K_DOWN),            // fsub dword [extDown]
        0xD9, 0x5E, 0x08,                          // fstp dword [esi+0x08]
        0xD9, 0x46, 0x14,                          // fld  dword [esi+0x14] (maxZ)
        0xD8, 0x05, ...u32(EXT_K_UP),              // fadd dword [extUp]
        0xD9, 0x5E, 0x14,                          // fstp dword [esi+0x14]
        ...(which === 1
            ? [0x8D, 0x45, 0xFC]                   // lea eax,[ebp-4]  (displaced)
            : [0x8D, 0x45, 0xF8]),                 // lea eax,[ebp-8]  (displaced)
        0xC3,                                      // ret
    ]

    // --- 3. angle cull -----------------------------------------------------
    // Replaces the 30-byte Nz-vs-zero test in BOTH builders (byte-identical,
    // same registers: edi = v1, eax = v2, ebx = v3), leaving the following
    // `test ah,1 / jne` intact. Computes N = ex x ey, then accepts iff
    //   Nz > cos(cullDeg)*|N|   OR   |Nz| < sin(keepVert)*|N|
    // and sets AH bit 0 = reject, exactly as the fnstsw it replaces. The loop's
    // sentinel must still be on top of the x87 stack on return: this pushes at
    // most 2 and fcompp pops both.
    const ANG1    = 0x007E34BD
    const ANG2    = 0x007E31F1
    const ANGCAVE = 0x00DD1E00
    const ANG_NZ  = 0x00DD1EA0   // scratch: Nz
    const ANG_LEN = 0x00DD1EA4   // scratch: |N|
    const ANG_T   = 0x00DD1EA8   // const:  cos(cullDeg)
    const ANG_EPS = 0x00DD1EAC   // const:  sin(keepVert)

    const angCave = [
        // Nx^2
        0xD9,0x40,0x04, 0xD8,0x67,0x04, 0xD9,0x43,0x08, 0xD8,0x67,0x08, 0xDE,0xC9,
        0xD9,0x43,0x04, 0xD8,0x67,0x04, 0xD9,0x40,0x08, 0xD8,0x67,0x08, 0xDE,0xC9,
        0xDE,0xE9, 0xD8,0xC8,
        // + Ny^2
        0xD9,0x40,0x08, 0xD8,0x67,0x08, 0xD9,0x03, 0xD8,0x27, 0xDE,0xC9,
        0xD9,0x43,0x08, 0xD8,0x67,0x08, 0xD9,0x00, 0xD8,0x27, 0xDE,0xC9,
        0xDE,0xE9, 0xD8,0xC8, 0xDE,0xC1,
        // Nz
        0xD9,0x00, 0xD8,0x27, 0xD9,0x43,0x04, 0xD8,0x67,0x04, 0xDE,0xC9,
        0xD9,0x03, 0xD8,0x27, 0xD9,0x40,0x04, 0xD8,0x67,0x04, 0xDE,0xC9,
        0xDE,0xE9,
        0xD9,0x15, ...u32(ANG_NZ), 0xD8,0xC8, 0xDE,0xC1,   // save Nz, + Nz^2
        0xD9,0xFA, 0xD9,0x15, ...u32(ANG_LEN),             // |N|, save
        // floor test: Nz > cos(cullDeg)*|N| ?
        0xD8,0x0D, ...u32(ANG_T), 0xD9,0x05, ...u32(ANG_NZ), 0xDE,0xD9, 0xDF,0xE0,
        0xF6,0xC4,0x01, 0x74,0x1B,                         // accepted -> ret
        // wall test: |Nz| < sin(keepVert)*|N| ?
        0xD9,0x05, ...u32(ANG_LEN), 0xD8,0x0D, ...u32(ANG_EPS),
        0xD9,0x05, ...u32(ANG_NZ), 0xD9,0xE1, 0xDE,0xD9, 0xDF,0xE0,
        0x80,0xF4,0x01, 0xC3,                              // invert / ret
    ]

    return [
        patch('decal-height-fade', [
            [zoff(UVCAVE), uvCave],
            [toff(DRAW1),  [0xE8, ...u32((UVCAVE - (DRAW1 + 5)) >>> 0)]],
            [toff(DRAW2),  [0xE8, ...u32((UVCAVE - (DRAW2 + 5)) >>> 0)]],
        ]),
        patch('decal-aabb-extent', [
            [zoff(EXT_K_DOWN), [...f32(extDown), ...f32(extUp)]],
            [toff(EXTCAVE1),   extCave(1)],
            [toff(EXTCAVE2),   extCave(2)],
            [toff(EXT1), [0xE8, ...u32((EXTCAVE1 - (EXT1 + 5)) >>> 0), 0x50, 0x90]],
            [toff(EXT2), [0xE8, ...u32((EXTCAVE2 - (EXT2 + 5)) >>> 0), 0x50, 0x90]],
        ]),
        patch('decal-angle-cull', [
            [zoff(ANG_T),   f32(Math.cos(cullDeg  * Math.PI / 180))],
            [zoff(ANG_EPS), f32(Math.sin(keepVert * Math.PI / 180))],
            [zoff(ANGCAVE), angCave],
            [toff(ANG1), [0xE8, ...u32((ANGCAVE - (ANG1 + 5)) >>> 0), ...nops(25)]],
            [toff(ANG2), [0xE8, ...u32((ANGCAVE - (ANG2 + 5)) >>> 0), ...nops(25)]],
        ]),
    ]
}

export const EXTENSION_DLL_PATCH_NAME = 'client-extensions'
export const ITEM_DBC_DISABLER_PATCH_NAME = 'item-dbc-disabler'
export const FIX_COMBO_POINT_PATCH_NAME = 'fix-combo-points'

export function ClientPatches(
    gamebuild: number
    , roles: {class:number,tank:number,healer:number,damage:number,leader:number}[]
    ) {
        let rolemask: number[] = new Array(32).fill(0)
        roles.forEach(x=>{
            rolemask[x.class-1] =
              (x.leader ? 1 : 0)
            | (x.tank   ? 2 : 0)
            | (x.healer ? 4 : 0)
            | (x.damage ? 8 : 0)
        })
        return [
            patch('large-address-aware',[
                [0x000126,[0x23]]
            ]),
            patch('view-distacnce-unlock',[
                [0x014137,[0x10,0x27]],
                [0x4c99f0,[0x34]],
                [0x63cf0c,[
                    0x00,0x40,0x1c,0x46,0x00,0x40,0x1c,0x46
                ]],
            ]),
            patch(FIX_COMBO_POINT_PATCH_NAME,[
                [0x210b12,[
                    0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
                ]],
            ]),
            patch('allow-custom-gluexml',[
                [0x126,[0x23]],
                [0x1f41bf,[0xeb]],
                [0x415a25,[0xeb]],
                [0x415a3f,[0x3]],
                [0x415a95,[0x3]],
                [0x415b46,[0xeb]],
                [0x415b5f,[0xb8,0x03]],
                [0x415b61,[0x0,0x0,0x0,0xeb,0xed]],
            ]),
            patch('unlimited race/class pairs patch', [
                [0xe0355,[0x78]],
                [0xe038e,[0x88]],
                [0xe03a3,[0x88]],
                [0xe03c3,[0x88]],
            ]),
            patch('class roles',[
                // role mask cave
                [0x005E1A37,[0,...rolemask]],
                // xrefs
                [0x151d48,[0x37,0x32,0x9E,0x00]],
                [0x152f7d,[0x37,0x32,0x9E,0x00]],
                [0x152f94,[0x37,0x32,0x9E,0x00]],
                [0x1531e7,[0x37,0x32,0x9E,0x00]],
                [0x153d22,[0x37,0x32,0x9E,0x00]],
            ]),
            // from https://model-changing.net/index.php?app=downloads&module=downloads&controller=view&id=314
            // credits to kebabstorm, original tbc version by BenjaminLSR and rajkosto
            patch(ITEM_DBC_DISABLER_PATCH_NAME,[
                [0x168,[0x5a,0xc5,0x75]],
                [0x11646d,[0x56,0x89,0xe1,0xe8,0xdb,0x1d,0x24,0x0,0x83,0xc4,0x4,0x89,0xc6,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90]],
                [0x1164ac,[0x89,0xf1,0x90]],
                [0x1223f7,[0x56,0x89,0xe1,0xe8,0x51,0x5e,0x23,0x0,0x83,0xc4,0x4,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90]],
                [0x122419,[0x89,0xc7,0x90]],
                [0x1a54ef,[0x90,0x90,0x90,0x90,0x90,0x90,0x8d,0x4d,0xf4]],
                [0x1a54f9,[0x53,0x2d,0x1b]],
                [0x1a5528,[0x90,0x90,0x90]],
                [0x1a552c,[0x45,0xf4]],
                [0x1a572e,[0x90,0x90,0x90,0x90,0x90,0x90,0x89,0xd9]],
                [0x1a5737,[0x15,0x2b,0x1b]],
                [0x1a575c,[0x90,0x90,0x90]],
                [0x1a5760,[0x4d,0xf8]],
                [0x1a7cf5,[0x83,0xc4,0x4,0x56,0x89,0xe1,0xe8,0xd0,0x4,0x1b,0x0,0x83,0xc4,0x4,0xeb,0x17,0xcc,0x89,0xc3,0x89,0xe1,0xe8,0x41,0x5,0x1b]],
                [0x1a7d0f,[0x83,0xc4,0x4,0xe9,0x6b,0x33,0x0,0x0,0xcc,0xcc,0xcc,0xcc,0xcc,0x85,0xc0]],
                [0x1a8c8e,[0x89,0xe1,0xe8,0xbb,0xf5,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1a8c9c,[0x90,0x90,0x90]],
                [0x1aa6d4,[0x89,0xe1,0xe8,0x75,0xdb,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1aa6e2,[0x90,0x90,0x90]],
                [0x1aa821,[0x90,0x8d,0x4d,0x8,0xe8,0xa6,0xd9,0x1a]],
                [0x1aa82a,[0x8b,0xf8,0xe9,0x67,0xbe,0x15,0x0]],
                [0x1aa832,[0xc0]],
                [0x1aa86c,[0x90,0x89,0xf8]],
                [0x1aa8a2,[0x89,0xe1,0xe8,0xa7,0xd9,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1aa8b0,[0x90,0x90,0x90]],
                [0x1aa9d0,[0x89,0xe1,0xe8,0x79,0xd8,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1aa9de,[0x90,0x90,0x90]],
                [0x1aaafa,[0x89,0xe1,0xe8,0x4f,0xd7,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1aab08,[0x90,0x90,0x90]],
                [0x1ab076,[0x89,0xe1,0xe8,0x53,0xd1,0x1a,0x0,0xe9,0x84,0xcc,0xff,0xff]],
                [0x1ab083,[0xc0]],
                [0x1ab0a9,[0x90,0x90,0x85,0xdb]],
                [0x1ab316,[0x89,0xe1,0xe8,0x33,0xcf,0x1a,0x0,0x83,0xc4,0x4]],
                [0x1ab324,[0x90,0x90,0x90]],
                [0x306614,[0x8b,0x41,0x8,0x8d,0x48,0xc,0xe9,0x11,0x1b,0x5,0x0]],
                [0x306620,[0xeb,0xf2,0xcc,0xcc,0xcc,0xcc]],
                [0x306650,[0xeb,0x3a,0xcc,0xcc,0xcc,0xcc]],
                [0x306683,[0x8d,0x48]],
                [0x306686,[0xe9,0x45,0x1b,0x5,0x0,0xcc,0x8b,0x41,0x8,0x8d,0x48,0xc,0xe9,0xe9,0x1a,0x5,0x0,0xcc,0x8d,0x4d,0x8,0xe8,0xb0,0x1b,0x5]],
                [0x3066a0,[0xe9,0x8c,0x41,0xea,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc]],
                [0x306703,[0x8d,0x48]],
                [0x306706,[0xe9,0x45,0x1b,0x5,0x0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc]],
                [0x306733,[0x8d,0x48]],
                [0x306736,[0xe9,0x15,0x1c,0x5,0x0,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc]],
                [0x309e03,[0x8d,0x48]],
                [0x309e06,[0xe8,0x45,0xe4,0x4,0x0,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90]],
                [0x358136,[0x56,0x8b,0x31,0x89,0xf0]],
                [0x35813c,[0x1,0x99,0x6a,0x0,0x68,0x70,0xeb,0x5e,0x0,0x33,0xc2,0x8d,0x4d,0xf8,0x51,0x2b,0xc2,0x50,0xb9,0x28,0xd8,0xc5,0x0,0xc7,0x45,0xf8]],
                [0x358157,[0x0,0x0,0x0,0xc7,0x45,0xfc]],
                [0x35815e,[0x0,0x0,0x0,0xe8,0xca,0x3c,0xf2,0xff,0x85,0xc0,0x74,0x8]],
                [0x35816b,[0x40,0x4,0x5e,0x8b,0xe5,0x5d,0xc3,0x89,0xf0,0x5e,0x89,0xec,0x5d,0xe9,0xa9,0xe4,0xfa,0xff]],
                [0x358186,[0x56,0x8b,0x31,0x89,0xf0]],
                [0x35818c,[0x1,0x99,0x6a,0x0,0x68,0x70,0xeb,0x5e,0x0,0x33,0xc2,0x8d,0x4d,0xf8,0x51,0x2b,0xc2,0x50,0xb9,0x28,0xd8,0xc5,0x0,0xc7,0x45,0xf8]],
                [0x3581a7,[0x0,0x0,0x0,0xc7,0x45,0xfc]],
                [0x3581ae,[0x0,0x0,0x0,0xe8,0x7a,0x3c,0xf2,0xff,0x85,0xc0,0x74]],
                [0x3581bb,[0x40,0x8,0x5e,0x8b,0xe5,0x5d,0xc3,0x89,0xf0,0x5e,0x89,0xec,0x5d,0xe9,0x89,0xe4,0xfa,0xff]],
                [0x61be58,[0x7c,0x7c]]
            ]),
            patch(EXTENSION_DLL_PATCH_NAME,[
                // stage
                [0x28e19c,[
                    // Jump to hook (replaces "LoadLibraryA")
                    0xE9,0x19,0x57,0x0E,0x00,
                    // pad old instruction
                    0x90
                ]],

                // cave  (file 0x3738b8 -> .text VA 0x7744b8, delta 0x400c00)
                //
                // Loads ClientExtensions.dll and then wow_optimize.dll off the
                // back of the client's own d3d9.dll LoadLibraryA call. Both are
                // optional at runtime: the return value is discarded and the
                // whole block is bracketed by pushad/popad, so a missing DLL
                // just yields NULL and the client carries on.
                //
                // Budget: the `EB 26` at the top skips to 0x3738e0, so the cave
                // body may occupy 0x3738ba..0x3738df (38 bytes). The body below
                // ends at 0x3738dc, leaving 3 bytes of the original NOP padding
                // spare. Adding a third DLL would need a different cave.
                [0x3738b8,[
                    // short jump 38 bytes (past code cave)
                    0xEB, 0x26,
                    // call "LoadLibraryA" (for d3d9.dll) (this is what we jump to)
                    0xFF, 0x15, 0x48, 0xF2, 0x9D, 0x00,
                    // push all registers
                    0x60,
                    // push "ClientExtensions.dll" string (VA 0x009e4271, see below)
                    0x68, 0x71, 0x42, 0x9E, 0x00,
                    // call "LoadLibraryA" (for ClientExtensions.dll)
                    0xFF, 0x15, 0x48, 0xF2, 0x9D, 0x00,
                    // push "wow_optimize.dll" string (VA 0x009e4250, see below)
                    0x68, 0x50, 0x42, 0x9E, 0x00,
                    // call "LoadLibraryA" (for wow_optimize.dll)
                    0xFF, 0x15, 0x48, 0xF2, 0x9D, 0x00,
                    // pop all registers
                    0x61,
                    // jump back to 0x28e1a2 (rel32 = 0x28e1a2 - 0x3738dd = -0xe573b).
                    // NOTE: this displacement changed when the second DLL was
                    // added — the jmp moved 11 bytes later, so it is no longer
                    // the 0xfff1a8d0 of the single-DLL cave.
                    0xE9, 0xC5, 0xA8, 0xF1, 0xFF
                ]],
                // "ClientExtensions.dll" string (file 0x5e2a71 -> .rdata VA 0x009e4271)
                [0x5e2a71,[0x43,0x6C,0x69,0x65,0x6E,0x74,0x45,0x78,0x74,0x65,0x6E,0x73,0x69,0x6F,0x6E,0x73,0x2E,0x64,0x6C,0x6C]],
                // "wow_optimize.dll\0" string (file 0x5e2a50 -> .rdata VA 0x009e4250).
                // Placed in the zero run at 0x5e298b..0x5e2a70 that immediately
                // precedes the ClientExtensions string — the same padding tswow
                // already carved that one out of. 16 chars + NUL ends at
                // 0x5e2a60, leaving 16 spare bytes before 0x5e2a71.
                [0x5e2a50,[0x77,0x6F,0x77,0x5F,0x6F,0x70,0x74,0x69,0x6D,0x69,0x7A,0x65,0x2E,0x64,0x6C,0x6C,0x00]]
            ]),
            patch('gamebuild',[
                [0x4c99f0,[
                    gamebuild&0xff,
                    (gamebuild>>8)&0xff
                ]]
            ]),
            // @duskhaven-port-begin
            patch('no-ammo-check', [ // credits to Aleist3r - removes client-side ammo check
                [0x408940, [0xE9, 0xBA, 0x00, 0x00, 0x00, 0x90, 0x90]]
            ]),
            patch('windowed-mode-gamma-fix', [ // credits to Robinsch - fixes gamma in windowed mode
                [0xE94, [0xEB]]
            ]),
            patch('melee-swing-right-click', [ // credits to Robinsch - melee swing on right click
                [0x2E1C67, [0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90]]
            ]),
            patch('npc-turn-attack-anim', [ // credits to Robinsch - NPC turn-to-attack animation fix
                [0x33D7C9, [0xEB]]
            ]),
            patch('npc-evade-ghost-attack', [ // credits to Robinsch - prevents NPCs evading during ghost attack
                [0x355BF, [0xEB]]
            ]),
            patch('non-throttled-item-cache-wdb-requests', [ // credits to Robinsch - removes item WDB request throttling
                [0x2689FD, [0x00, 0x00]]
            ]),
            patch('patch-area-trigger-timer', [ // credits to Robinsch - area trigger timing fix
                [0x2DB241, [0x32]]
            ]),
            patch('patch-mail-request-timeout', [ // credits to Robinsch - mail request timeout fix
                [0x16D899, [0x05, 0x01, 0x00, 0x00, 0x00]]
            ]),
            patch('missing-pre-cast-animation', [ // credits to Robinsch - fixes missing pre-cast animation
                [0x33E0D6, [0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90]]
            ]),
            patch('naked-character-issue', [ // credits to Robinsch - fixes naked-character visual bug
                [0x1DDC5D, [0xEB]]
            ]),
            patch('allow-chat-commands-while-dead', [ // credits to Robinsch - allows /commands while dead
                [0x10CA41, [0xEB]]
            ]),
            patch('mouse-flickering-camera-snapping', [ // credits to bonbigz - mouse flicker + camera snap fix
                [0x469A2C, [0xE9, 0x71, 0xF0, 0x0B, 0x00, 0xF8, 0x13, 0xD4, 0x00, 0x8B, 0x1D, 0xFC]],
                [0x528AA2, [0x8D, 0x4D, 0xF0, 0x51, 0x57, 0xFF, 0x15, 0xDC, 0xF5, 0x9D, 0x00, 0x8B, 0x45, 0xF0, 0x8B, 0x15, 0xF8, 0x13, 0xD4, 0x00, 0xE9, 0x7A, 0x0F, 0xF4, 0xFF]],
                [0x4691B1, [0x89, 0xE5, 0x8B, 0x05, 0xFC, 0x13, 0xD4, 0x00, 0x8B, 0x0D, 0xF8, 0x13, 0xD4, 0x00, 0xEB, 0xC2, 0x7D, 0x03, 0x83, 0xC1, 0x01, 0x83, 0xC0, 0x32, 0x83, 0xC1, 0x32, 0x3B, 0x0D, 0xEC, 0xBC, 0xCA, 0x00, 0x7E, 0x03, 0x83, 0xE9, 0x01, 0x3B, 0x05, 0xF0, 0xBC, 0xCA, 0x00, 0x7E, 0x03, 0x83, 0xE8, 0x01, 0x83, 0xE9, 0x32, 0x83, 0xE8, 0x32, 0x89, 0x0D, 0xF8, 0x13, 0xD4, 0x00, 0x89, 0x05, 0xFC, 0x13, 0xD4, 0x00, 0x89, 0xEC, 0x5D, 0xE9, 0xB4, 0xF7, 0xFF, 0xFF, 0xEC, 0x5D, 0xC3, 0xC3]],
                [0x469183, [0x83, 0xF8, 0x32, 0x7D, 0x03, 0x83, 0xC0, 0x01, 0x83, 0xF9, 0x32, 0xEB, 0x31]]
            ]),
            patch('get-selected-background-model-dehardcoding', [ // Aleist3r - de-hardcodes char-select background model
                [0xE2A83, [0x25]],
                [0xE2A8B, [0x1D]],
                [0xE2A95, [
                    0x0F, 0xB6, 0x80, 0x78, 0x01, 0x00, 0x00, 0x50, 0xB9, 0x40, 0x34, 0xAD, 0x00, 0xE8, 0xE9, 0x8B,
                    0x17, 0x00, 0xEB, 0x23, 0xA1, 0x38, 0x34, 0xAD, 0x00, 0x83, 0xF8, 0x02, 0x7F, 0x33, 0x83, 0x3D,
                    0x34, 0x34, 0xAD, 0x00, 0x02, 0x7C, 0x2A, 0x8B, 0x15, 0x48, 0x34, 0xAD, 0x00, 0xB9, 0x02, 0x00,
                    0x00, 0x00, 0x2B, 0xC8, 0x8B, 0x04, 0x8A, 0x85, 0xC0, 0x74, 0x16, 0x8B, 0x40, 0x2C, 0x50, 0x57,
                    0xE8, 0x76, 0xAC, 0x36, 0x00, 0x83, 0xC4, 0x08, 0x5E, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5F, 0x5D,
                    0xC3, 0xB8, 0xFF, 0x14, 0x9E, 0x00, 0x50, 0x57, 0xE8, 0xE5, 0xAC, 0x36, 0x00, 0x83, 0xC4, 0x08,
                    0x5E, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5F, 0x5D, 0xC3
                ]],
                [0xE2AFE, [0x90, 0x90, 0x90, 0x90]]
            ]),
            patch('chain-spell-visual-crashfix', [ // Aleist3r - needs more testing, may break some spell visuals
                [0x32A36C, [0xEB]]
            ]),
            patch('checksum', [ // patches client checksum header values
                [0x168, [0x98, 0x96]],
                [0x1A9, [0x00, 0x00, 0x00, 0x00, 0x00]],
                [0x210, [0x00, 0xE0]],
                [0x238, [0x00, 0x70]],
                [0x260, [0x00, 0xB0]],
                [0x2B0, [0x00, 0x10]]
            ]),
            /**
             * Glue-screen (login/loading) art stops being stretched on
             * widescreen resolutions. The client scales these elements by a
             * hardcoded 4:3-era factor; the patches replace those float
             * constants with ones that keep the source aspect.
             *
             * Ported from a binary_pattern_replace .bat - the byte patterns
             * resolve to these file offsets in a clean 3.3.5a Wow.exe
             * (md5 45892bdedd0ad70aed4ccd22d9fb5984), each verified unique or
             * exactly two-occurrence as the .bat assumed.
             */
            patch('glue-background-no-stretch', [
                // 1.3333333, 1.6  ->  1.0, 1.5   (only match in the file)
                [0x6b43b4, [0x00,0x00,0x80,0x3F, 0x00,0x00,0xC0,0x3F]]
            ]),
            patch('loading-bar-border-no-stretch', [
                // 0.6 -> 0.2, both entries of the two-element descriptor table
                [0x5e1624, [0xCD,0xCC,0x4C,0x3E]],
                [0x5e1654, [0xCD,0xCC,0x4C,0x3E]]
            ]),
            patch('loading-bar-fill-no-stretch', [
                // 0.525 -> 0.175, matching the two border entries above
                [0x5e160c, [0x33,0x33,0x33,0x3E]],
                [0x5e163c, [0x33,0x33,0x33,0x3E]]
            ]),
            patch('sw-occluder', [ // Stormwind occluder fix for Open Azeroth
                [0x6EE040, [0x9f]],
                [0x6EE041, [0x86]],
                [0x6EE042, [0x01]],
                [0x6EE043, [0x00]]
            ]),
            /**
             * CM2Model::SetGeosetVisible (VA 0x82C7C0) reads each skin
             * section's id as a dword, but it is uint16 id + uint16 Level
             * (Level != 0 on models past 65535 vertices), so those sections
             * never matched and their geosets could not be shown or hidden.
             * Read the 16-bit id instead (Ascension's fix, detour 0x101a1440).
             * `mov eax,[ebp-4]; mov eax,[eax+edx]` -> `add edx,[ebp-4];
             * movzx eax,word [edx]`; edx is reloaded before its next use.
             */
            patch('geoset-level-fix', [
                [0x42BBEA, [0x03,0x55,0xFC, 0x0F,0xB7,0x02]]
            ]),
            ...cameraHeightPatch(CAMERA_HEIGHT_OFFSET),
            ...decalSlopePatch(
                  DECAL_EXT_DOWN, DECAL_EXT_UP, DECAL_CULL_DEG, DECAL_KEEP_VERT
            ),
            /**
             * awesome_wotlk's loader, ported from its standalone patcher
             * (Projects/awesome_wotlk/src/AwesomeWotlkPatch/Patch.h) so the
             * launcher can't wipe it: any exe the launcher ships is rebuilt
             * from Wow.exe.clean, and anything applied by hand afterwards is
             * overwritten on the next update.
             *
             * Requires AwesomeWotlkLib.dll next to Wow.exe - the blob at
             * 0x4E5CB0 LoadLibrary's it by name, and without the file the hook
             * runs but the library never loads.
             *
             * Verified against a clean 3.3.5a exe as not overlapping any other
             * patch in this file.
             */
            patch('awesome-wotlk', [
                // lua_ScanDllStart: mov eax,1; ret  (VA 0x4DCCF0)
                [0xDC0F0, [0xB8,0x00,0x00,0x00,0x00,0xC3]],
                // ScanDllStart: load AwesomeWotlkLib.dll, then resume  (VA 0x4E5CB0)
                [0xE50B0, [
                    0xB8,0x01,0x00,0x00,0x00, 0xA3,0x74,0xB4,0xB6,0x00,
                    0x68,0xE0,0x5C,0x4E,0x00, 0xE8,0x1C,0x68,0x38,0x00,
                    0x83,0xC4,0x04, 0x55, 0x8B,0xEC,
                    0xE8,0xA1,0x10,0xF2,0xFF, 0xE9,0x04,0x5B,0xF2,0xFF,
                    0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
                    // "AwesomeWotlkLib.dll\0"
                    0x41,0x77,0x65,0x73,0x6F,0x6D,0x65,0x57,0x6F,0x74,0x6C,0x6B,
                    0x4C,0x69,0x62,0x2E,0x64,0x6C,0x6C,0x00
                ]],
                // StartAddress: jmp into the loader above  (VA 0x40B7D0)
                [0xABD0, [0xE9,0xDB,0xA4,0x0D,0x00,0x90,0x90,0x90]],
            ])
            // @duskhaven-port-end
        ]
}
