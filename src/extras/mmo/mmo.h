#pragma once

// The shared world ("MMO") of Orthros' modified Vice City: the hooks the game's own code calls.
// Design and wire protocol: Orthros' docs/mmo.md. Every function is harmless when no shared world is being played
// (IsActive() false): the game then runs exactly as without them. Each one belongs to one module (the file named in
// its section); mmo_weak.cpp gives every one a weak default so that each module builds on its own.
// Rules for the game's files: a call is wrapped in `#ifdef ORTHROS_EXTRAS`, includes "mmo.h", and stays a one-line
// test or call (the logic lives here).

#ifdef ORTHROS_EXTRAS
#include "common.h"

class CEntity;
class CPhysical;
class CPed;
class CVehicle;
class CGarage;
class CStoredCar;
class CRunningScript;
class CPickup;

namespace OrthrosMmo
{
	// ---------------------------------------------------------------- core (mmo_wire.cpp)
	// A shared world is being played (its world loaded with the multiplayer's rules on), or being entered.
	bool IsActive(void);
	bool IsEntering(void);
	// The account may use cheats (the server's admin list).
	bool IsAdmin(void);
	// The main loop, every iteration, right after OrthrosFrameYield (sdl2.cpp): the page's queue drained and
	// dispatched. Also the pause menu's away state.
	void Pump(void);
	// CGame::Process: after CPad::UpdatePads; just before CWorld::Process; just after it; at its very end (the
	// frame's records sent).
	void BeginProcess(void);
	void PreWorld(void);
	void PostWorld(void);
	void EndProcess(void);
	// FrontendIdle, after FrontEndMenuManager.Process (the main menu runs no CGame::Process).
	void FrontendFrame(void);
	// CMenuManager::DrawBackground, after a page's standard menus (active: the page shown, not the one fading out):
	// what the lobby's pages draw beyond their rows (the appearance picker).
	void DrawFrontend(int32 screen, bool active);
	// Render2dStuff before the HUD (name tags); over everything after RenderCheatList (chat, lists, notices).
	void Render2d(void);
	void RenderOverlay(void);
	// Keyboard (events.cpp HandleKeyDown, rsKeyCodes): true = taken by a text field or a list, not the game's.
	bool KeyDown(int32 rsKey);
	// Typed text (sdl2.cpp SDL_TEXTINPUT, UTF-8).
	void TextInput(const char *utf8);
	// The mouse while an MMO list is open: absolute, free, its clicks not the game's.
	bool OverlayHasMouse(void);
	void OverlayMouse(float x, float y);
	bool OverlayClick(float x, float y);
	// The local player died (GameLogic.cpp, before KillPlayer) / was respawned (after the restore): told to the
	// session and combat modules.
	void LocalPlayerDied(void);
	void LocalPlayerRespawned(void);

	// ---------------------------------------------------------------- entities (mmo_entities.cpp)
	// A copy of another client's entity (ambient puppet or remote player): no AI, no physics, never deleted by the
	// game's own removers, its damage forwarded to its owner.
	bool IsPuppet(const CEntity *e);
	// The ped/vehicle ProcessControl of a puppet (CPed, CCivilianPed, CCopPed, CEmergencyPed; CAutomobile, CBike,
	// CBoat, CHeli, CPlane): true = done, return at once.
	bool PuppetPedControl(CPed *ped);
	bool PuppetVehicleControl(CVehicle *veh);
	// Kinds for MayGenerateAt.
	enum { GEN_PED, GEN_GANG, GEN_LAW, GEN_TRAFFIC, GEN_BOAT, GEN_PARKED, GEN_CARPARK, GEN_EMERGENCY };
	// May this client create an ambient entity there (the closest player populates)? Law spawns of the wanted local
	// player always may.
	bool MayGenerateAt(const CVector &pos, int32 kind);
	// The game is about to delete an owned ambient entity for distance: true = given to a nearer remote player
	// instead (do not delete it).
	bool MigrateInsteadOfRemove(CEntity *e);
	// CPed / CVehicle destructors.
	void EntityDestroyed(CEntity *e);
	// The local player starts entering this car (PlayerInfo.cpp): false = refuse (driven by a remote player).
	bool PlayerMayEnter(CVehicle *veh);
	// A car generator about to spawn (CarGen.cpp): its index in CTheCarGenerators::CarGeneratorArray.
	bool MayGenerateParked(int32 generator, const CVector &pos);
	// The game is about to delete an owned entity for a reason other than distance (CarCtrl.cpp: an off-screen car
	// recycled, a wreck tidied up): true = a remote player is near it, keep it.
	bool KeepForOthers(CEntity *e);

	// ---------------------------------------------------------------- combat (mmo_combat.cpp)
	// CPed::InflictDamage on entry: true = handled (a puppet's damage forwarded or dropped), return *result.
	bool PedDamage(CPed *victim, CEntity *damagedBy, int32 method, float damage, int32 pedPiece, uint8 direction, bool *result);
	// CPed::KillPedWithCar on entry: true = handled.
	bool PedHitByCar(CPed *victim, CVehicle *car, float impulse);
	// CVehicle::InflictDamage on entry: true = handled.
	bool VehicleDamage(CVehicle *veh, CEntity *damagedBy, int32 weaponType, float damage, const CVector &pos);
	// CAutomobile/CBike::BurstTyre, ::VehicleDamage(impulse, piece) on entry: true = handled.
	bool TyreBurst(CVehicle *veh, uint8 wheel);
	bool VehicleImpulse(CVehicle *veh, float impulse, uint16 piece);
	// BlowUpCar (CAutomobile, CBike, CBoat) on entry: true = not here (a puppet blows up when its owner says so).
	bool BlockBlowUp(CVehicle *veh);
	// CPed::SetDie / SetDead on entry: true = not here (a puppet dies when its owner says so).
	bool BlockPuppetDeath(CPed *ped);
	// CWeapon::Fire / FireFromCar / CPed::FightStrike on entry: true = the shooter is a puppet, no shot.
	bool BlockFire(CEntity *shooter);
	// A local shot's visible parts (CBulletTraces::AddTrace; CBulletInfo::AddBullet; CShotInfo::AddShot flames).
	void BulletTrace(const CVector &start, const CVector &end, int32 weaponType, CEntity *shooter);
	void BulletAdded(int32 weaponType, CEntity *source, const CVector &pos, const CVector &speed);
	void FlameShot(CEntity *source, const CVector &start, const CVector &end);
	// CProjectileInfo::AddProjectile after it succeeded (the projectile's slot); RemoveProjectile: true = no
	// explosion here (a remote projectile's comes with its owner's explosion).
	void ProjectileAdded(int32 slot);
	bool ProjectileSilent(int32 slot);
	// CExplosion::AddExplosion on entry (a local one is told to the others).
	void ExplosionAdded(CEntity *exploding, CEntity *culprit, int32 type, const CVector &pos, uint32 lifetime);
	// Area damage (explosions, fire, flames, gas) skips what is not ours.
	bool SkipAreaDamage(CEntity *e);
	// Pickups: a dynamic one created (CPickups::GenerateNewOne result), one collected by the local player.
	void PickupCreated(int32 handle);
	void PickupCollected(int32 slot);
	// CPickup::Update, the local player about to collect it: true = not paid here now (another client's pickup,
	// claimed from its maker: paid when granted).
	bool PickupHeld(CPickup *pickup);
	// CBike::KnockOffRider on entry: true = handled (a puppet rider knocked off by us: told to its owner).
	bool KnockOff(CVehicle *bike, CPed *rider, int32 weapon, uint8 direction);

	// ---------------------------------------------------------------- world (mmo_world.cpp)
	// CClock::Update on entry: true = the shared clock set it.
	bool ClockUpdate(void);
	// CWeather::Update's new-hour step: true = the shared weather set it (skip the local step).
	bool WeatherUpdate(void);
	// The traffic lights' milliseconds (CTimer's when no shared world).
	uint32 LightsTime(void);
	// Garages (hideouts): the door mirrored for another client's holder (true = skip the state machine); may this
	// client restore the stored cars and open (it holds the lock); the cars just stored; a vehicle the garage code
	// must leave alone.
	bool HideoutMirrored(CGarage *garage);
	bool HideoutMayOpen(CGarage *garage);
	void HideoutStored(CGarage *garage, CStoredCar *cars);
	bool GarageLeaveAlone(CVehicle *veh);
	// The open hideout's closing conditions (GS_OPENED): true = a remote player is in it or at its door, keep it open.
	bool HideoutKeepOpen(CGarage *garage);

	// ---------------------------------------------------------------- session (mmo_session.cpp)
	// After a saved game loaded (OrthrosExtras::GameLoaded): the shared world prepared and the character applied.
	void GameLoaded(void);
	// A new script thread named (COMMAND_SCRIPT_NAME): true = it was a single-player thread, now terminated.
	bool ScriptNamed(CRunningScript *script);
	// The player's model at a respawn (GameLogic SortOutStreamingAndMemory): the character's skin.
	const char *PlayerSkin(const char *original);
	// The debug menu, the var console, the coordinates teleport: only admins in a shared world.
	bool DebugAllowed(void);
	// The pause menu opening (its rows: no save, "Leave multiplayer").
	void PauseMenuOpening(void);
	// A game starts (OrthrosExtras::StartingGame: a new story game, a saved one, the free mode's, the shared world's):
	// any other than the shared world's leaves it (the character saved, LEAVE sent).
	void GameStarting(bool shared);

	// ---------------------------------------------------------------- social (mmo_social.cpp)
	// CPad::AddToPCCheatString after the character entered the string (reversed in buf): true = taken here (the
	// teleport list; a cheat refused or sent as a world cheat), the game's own matching skipped.
	bool CheatString(char *reversed);
	// CPad::DoCheats on entry: true = the gamepad's cheat combos are off (a shared world: cheats go through the
	// keyboard's gate only).
	bool PadCheatsBlocked(void);
	// The HELP list pauses the game except in a shared world.
	bool ListsPause(void);
}
#endif
