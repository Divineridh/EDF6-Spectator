# EDF6 Spectator

Spectator mode while you're down in Earth Defense Force 6: instead of staring at your own body while
you wait for a revive, the camera follows your teammates. It jumps to the first living teammate
`delay` seconds after you go down (2 by default), left click / RB and right click / LB cycle through
them, your own body is the last stop of the cycle, and the camera comes back to you as soon as you're
revived.

Only the local camera changes. Nothing is sent over the network, so the other players don't need the
mod.

## Status

Built from static analysis of `EDF.dll` (May 2025 build). Tried offline with `debug=1`: the hooks
install, and F8 plus left click move the camera through the allied NPC soldiers and back to the
player. **Not tried online yet**: that needs a session with someone who can let you go down. What the
log should show on the first try is under [Trying it](#trying-it).

## How the camera follows someone

Every player soldier gets a `CharacterGhostCamera` when it spawns: `SoldierBase_IRecordable_Registration`
(vtable slot 1, today `EDF.dll+0x5A4F40`) reads `player_no`, creates the soldier (`+0x591410`), puts
it on team 0 and calls the camera factory (`+0x82B40`) with the soldier at `InitParam+0x80` and the
player number as the camera slot.

| offset | what it is |
|--------|------------|
| camera `+0x350` / `+0x358` | `weak_ptr` (pointer, control block) to the followed object, copied from the object's `enable_shared_from_this` |
| camera `+0x360` | the soldier itself, used for its weapons (scopes); the mod leaves it alone |
| object `+0x28` / `+0x30` | the object's own `weak_ptr` (`enable_shared_from_this`) |
| object `+0x60`..`+0x9F` | world matrix |
| object `+0x2F4` / `+0x2F8` | max HP / HP (floats; found the same way as EDF6-EnemyHp) |
| object `+0x314` | team: 0 players, 1 enemies, 2 allied NPCs |
| object `+0x338` | player number, -1 for anything a player doesn't control |

The camera's per-frame update (vtable slot 4, today `+0xF86A0`) locks that `weak_ptr` and rebuilds
the view from the followed object every frame: its world matrix, its camera block (`+0x170` or
`+0x230` depending on the byte at `+0x224`), the player options for `+0x338` and the extras at
`+0x320`-`+0x354`. So pointing the `weak_ptr` at a teammate's soldier makes the camera follow that
teammate with their own camera setup.

The mod hooks that update and the camera's destructor (slot 0, `+0xF6030`). Before each update:

- If the followed object is a player soldier (team 0, player number set, `SoldierBase` in its RTTI
  hierarchy) with HP ≤ 0 for `delay` seconds, it keeps a weak reference to it (the "own body") and
  starts spectating.
- While spectating, it writes the chosen teammate into `+0x350` / `+0x358` with the same reference
  counting the game uses (weak count at control block `+0xC`, `_Delete_this` at vtable slot 1).
- It goes back to the own body when its HP is above 0 again, and lets go without touching anything
  if the game points the camera elsewhere on its own (respawn, vehicle, cutscene) or the body is
  gone.

Everything runs on the game thread inside the camera update, under a `__try`: if anything faults, the
mod logs the exception and turns itself off instead of taking the game down.

## Finding teammates

Team membership lives in the team manager (pointer at `EDF.dll+0x20B2978` today). It holds a
`CRITICAL_SECTION` at `+0x8` and a table at `+0x38` of 0x38-byte entries, one per team; each entry
begins with the head of a `std::set<GameObject *>` and `+0x28` is the number of teams. It's what
`+0x5E11D0` walks to visit every object of the allied teams. The mod takes the same lock and walks
team 0 for player soldiers alive (HP > 0), sorted by player number.

## How it's located

Nothing is looked up by address:

| piece | how |
|-------|-----|
| `CharacterGhostCamera` vtable | RTTI: `.?AVCharacterGhostCamera@@` → type descriptor → complete object locator → vtable. The update must contain `lea rdx, [rdi+350h]` |
| `SoldierBase`, `HumanBase` | their RTTI type descriptors, compared against each object's class hierarchy |
| team manager | `cmp dword ptr [rbx+314h], -1 / je / mov rcx, [rip+…] / mov rdx, rbx / call` in the team change of a game object |
| HP offset | the `addss / minss / maxss / movss` block of the damage function |

Player classes, for reference: Ranger `AssultSoldier`, Wing Diver `PaleWing`, Fencer `HeavyArmor`,
Air Raider `Engineer`, all derived from `SoldierBase`.

Also seen in `EDF.dll` while looking for this, unused: a debug console with `next_camera`,
`ghost_camera`, `free_camera`, `create_remote_camera` (handlers around `+0xA1F90`), and a camera
manager at `EDF.dll+0x20B2958` whose slot `i` (`+0x4D8 + i * 0x188`) holds a `weak_ptr` to the
current camera of player `i`.

## Trying it

With the mod installed, `Spectator.log` (next to EDF6.exe) should say on startup:

```
config: delay 2.0 s, next 0x01, prev 0x02, self 0x00, gamepad on
HP at +0x2F8, team manager at EDF.dll+0x20B2978, camera vtable at EDF.dll+0x1768C10
hooked camera update EDF.dll+0xF86A0 and destructor EDF.dll+0xF6030
```

and in an online mission, when you go down and get revived:

```
player 1 is down
player 1: spectating
watching player 0
back to own body: revived
```

Offline, `debug=1` in `config.ini` lets F8 start and stop spectating at any time, even alive, with
allied NPC soldiers in the cycle. It's a way to see the camera swap without a second player; while
alive your soldier may aim oddly, since it's meant only for testing.

Things to look at on the first online try:

- whether "player N is down" shows up at all (it assumes a downed player sits at HP 0);
- whether the view on a remote teammate turns with where they look, or stays at a fixed angle behind
  them (that depends on whether their camera block is replicated);
- that the camera comes back to you on revive.

## Build

```bash
build.bat
```

VS2019 Build Tools (MSVC 14.29). Dependencies aren't in the repo; clone them into `deps/`:

```bash
git clone https://github.com/TsudaKageyu/minhook    deps/minhook
git clone https://github.com/Quarri6343/EDF6Plugins deps/EDF6Plugins
```

## Package

```bash
python tools/package.py
```

Writes `EDF6Spectator.zip` to `../builds/` with the DLL, the sample `config.ini` and `README.txt`. It
refuses to package if any file in `src/` is newer than the DLL.

## Install

```
EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6Spectator.dll
EARTH DEFENSE FORCE 6\Mods\Spectator\config.ini   (optional)
```
