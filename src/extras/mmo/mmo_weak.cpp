// Weak defaults of every shared-world function (mmo.h, mmo_internal.h): each module's own definitions replace them at
// link time, so a module builds and runs without the others. With none of them, the game is the game.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"

#define WEAK __attribute__((weak))

namespace OrthrosMmo
{
bool bReplaying;

// ---------------------------------------------------------------- mmo.h: core
WEAK bool IsActive(void) { return false; }
WEAK bool IsEntering(void) { return false; }
WEAK bool IsAdmin(void) { return false; }
WEAK void Pump(void) {}
WEAK void BeginProcess(void) {}
WEAK void PreWorld(void) {}
WEAK void PostWorld(void) {}
WEAK void EndProcess(void) {}
WEAK void FrontendFrame(void) {}
WEAK void Render2d(void) {}
WEAK void RenderOverlay(void) {}
WEAK bool KeyDown(int32) { return false; }
WEAK void TextInput(const char *) {}
WEAK bool OverlayHasMouse(void) { return false; }
WEAK void OverlayMouse(float, float) {}
WEAK bool OverlayClick(float, float) { return false; }
WEAK void LocalPlayerDied(void) {}
WEAK void LocalPlayerRespawned(void) {}
// entities
WEAK bool IsPuppet(const CEntity *) { return false; }
WEAK bool PuppetPedControl(CPed *) { return false; }
WEAK bool PuppetVehicleControl(CVehicle *) { return false; }
WEAK bool MayGenerateAt(const CVector &, int32) { return true; }
WEAK bool MigrateInsteadOfRemove(CEntity *) { return false; }
WEAK void EntityDestroyed(CEntity *) {}
WEAK bool PlayerMayEnter(CVehicle *) { return true; }
WEAK bool MayGenerateParked(int32, const CVector &) { return true; }
// combat
WEAK bool PedDamage(CPed *, CEntity *, int32, float, int32, uint8, bool *) { return false; }
WEAK bool PedHitByCar(CPed *, CVehicle *, float) { return false; }
WEAK bool VehicleDamage(CVehicle *, CEntity *, int32, float, const CVector &) { return false; }
WEAK bool TyreBurst(CVehicle *, uint8) { return false; }
WEAK bool VehicleImpulse(CVehicle *, float, uint16) { return false; }
WEAK bool BlockBlowUp(CVehicle *) { return false; }
WEAK bool BlockPuppetDeath(CPed *) { return false; }
WEAK bool BlockFire(CEntity *) { return false; }
WEAK void BulletTrace(const CVector &, const CVector &, int32, CEntity *) {}
WEAK void BulletAdded(int32, CEntity *, const CVector &, const CVector &) {}
WEAK void FlameShot(CEntity *, const CVector &, const CVector &) {}
WEAK void ProjectileAdded(int32) {}
WEAK bool ProjectileSilent(int32) { return false; }
WEAK void ExplosionAdded(CEntity *, CEntity *, int32, const CVector &, uint32) {}
WEAK bool SkipAreaDamage(CEntity *) { return false; }
WEAK void PickupCreated(int32) {}
WEAK void PickupCollected(int32) {}
// world
WEAK bool ClockUpdate(void) { return false; }
WEAK bool WeatherUpdate(void) { return false; }
WEAK uint32 LightsTime(void);
WEAK bool HideoutMirrored(CGarage *) { return false; }
WEAK bool HideoutMayOpen(CGarage *) { return true; }
WEAK void HideoutStored(CGarage *, CStoredCar *) {}
WEAK bool GarageLeaveAlone(CVehicle *) { return false; }
// session
WEAK void GameLoaded(void) {}
WEAK bool ScriptNamed(CRunningScript *) { return false; }
WEAK const char *PlayerSkin(const char *original) { return original; }
WEAK bool DebugAllowed(void) { return true; }
WEAK void PauseMenuOpening(void) {}
// social
WEAK bool CheatString(char *) { return false; }
WEAK bool ListsPause(void) { return true; }

// ---------------------------------------------------------------- mmo_internal.h: core services
WEAK void CommitRecord(const uint8 *, int32, bool) {}
WEAK double Now(void) { return 0.0; }
WEAK double ServerNow(void) { return 0.0; }
WEAK bool NetAvailable(void) { return false; }
WEAK void Connect(void) {}
WEAK void Disconnect(void) {}
WEAK int32 LinkState(void) { return LINK_NONE; }
WEAK int32 Refusal(void) { return 0; }
WEAK void SignIn(void) {}
WEAK bool Testing(void) { return false; }
WEAK int32 Mode(void) { return MODE_OFF; }
WEAK void SetMode(int32) {}
WEAK int32 LocalSlot(void) { return NO_SLOT; }
WEAK uint16 LocalTag(void) { return 0; }
WEAK uint32 LocalRank(void) { return 0; }
WEAK uint32 NewNetId(void) { return 0; }
WEAK bool LangFrench(void) { return false; }
WEAK void GameText(const char *utf8, wchar *out, int32 size)
{
	int32 n = 0;
	for (; utf8[n] && n < size - 1; n++) out[n] = (uint8)utf8[n];
	out[n] = 0;
}
static LobbyData weakLobby;
WEAK const LobbyData &Lobby(void) { return weakLobby; }
WEAK void RequestCreate(const char *, const char *) {}
WEAK void RequestSkin(uint32, const char *) {}
WEAK void RequestDelete(uint32) {}
WEAK void RequestJoin(uint32) {}
WEAK void RequestLeave(void) {}
static Character weakMe;
WEAK const Character &Me(void) { return weakMe; }
WEAK void SetMySkin(const char *) {}
static WorldState weakWorld;
WEAK const WorldState &World(void) { return weakWorld; }
WEAK double WorldMinutes(void) { return 720.0; }
WEAK RemotePlayer *Player(int32) { return nil; }
WEAK NetEntity *Find(const CEntity *) { return nil; }
WEAK NetEntity *FindNet(uint32) { return nil; }
WEAK NetEntity *Register(CEntity *, uint32, int32, bool, uint8) { return nil; }
WEAK void Unregister(const CEntity *) {}
WEAK int32 RegistrySize(void) { return 0; }
WEAK NetEntity *RegistryAt(int32) { return nil; }
WEAK uint32 NetIdOf(const CEntity *) { return 0; }
WEAK CEntity *EntityOf(uint32) { return nil; }
WEAK bool OwnedHere(const CEntity *) { return true; }
WEAK void DebugAppend(const char *, ...) {}

// ---------------------------------------------------------------- module handlers
WEAK void Session_Welcome(void) {}
WEAK void Session_Refused(int32) {}
WEAK void Session_CharResult(int32, int32, uint32) {}
WEAK void Session_Left(void) {}
WEAK void Session_LinkReset(void) {}
WEAK void Session_BeginProcess(void) {}
WEAK void Session_PreWorld(void) {}
WEAK void Session_PostWorld(void) {}
WEAK void Session_EndProcess(void) {}
WEAK void Session_FrontendFrame(void) {}
WEAK void Session_OnEvent(int32, int32, In &) {}
WEAK void Session_OnTargeted(int32, uint32, int32, In &) {}
WEAK bool Session_TestCommand(const char *) { return false; }
WEAK void Session_Debug(void) {}
WEAK void Session_PlayerDied(void) {}
WEAK void Session_PlayerRespawned(void) {}

WEAK void Lobby_Changed(void) {}
WEAK void Lobby_FrontendFrame(void) {}
WEAK bool Lobby_KeyDown(int32) { return false; }
WEAK void Lobby_TextInput(const char *) {}
WEAK wchar *Lobby_Text(const char *) { return nil; }
WEAK void Lobby_ScreenChanged(int32) {}
WEAK void Lobby_DrawFrontend(int32) {}
WEAK bool Lobby_TestCommand(const char *) { return false; }
WEAK void Lobby_Debug(void) {}

WEAK void Players_OnState(int32, In &) {}
WEAK void Players_Joined(int32) {}
WEAK void Players_Left(int32) {}
WEAK void Players_Info(int32) {}
WEAK void Players_Away(int32) {}
WEAK void Players_GameLoaded(void) {}
WEAK void Players_PreWorld(void) {}
WEAK void Players_PostWorld(void) {}
WEAK void Players_Render2d(void) {}
WEAK bool Players_PuppetControl(CPed *, NetEntity *) { return false; }
WEAK void Players_Reset(void) {}
WEAK void Players_OnEvent(int32, int32, In &) {}
WEAK void Players_OnTargeted(int32, uint32, int32, In &) {}
WEAK bool Players_TestCommand(const char *) { return false; }
WEAK void Players_Debug(void) {}

WEAK void Entities_OnState(int32, In &) {}
WEAK void Entities_OnGone(int32, uint32, int32) {}
WEAK void Entities_OnOwner(uint32, int32, int32) {}
WEAK void Entities_GameLoaded(void) {}
WEAK void Entities_PreWorld(void) {}
WEAK void Entities_PostWorld(void) {}
WEAK void Entities_Reset(void) {}
WEAK void Entities_OnEvent(int32, int32, In &) {}
WEAK void Entities_OnTargeted(int32, uint32, int32, In &) {}
WEAK bool Entities_TestCommand(const char *) { return false; }
WEAK void Entities_Debug(void) {}

WEAK void Combat_PreWorld(void) {}
WEAK void Combat_PostWorld(void) {}
WEAK void Combat_Reset(void) {}
WEAK void Combat_OnEvent(int32, int32, In &) {}
WEAK void Combat_OnTargeted(int32, uint32, int32, In &) {}
WEAK bool Combat_TestCommand(const char *) { return false; }
WEAK void Combat_Debug(void) {}
WEAK void Combat_PlayerDied(void) {}
WEAK void Combat_PlayerRespawned(void) {}

WEAK void World_Changed(void) {}
WEAK void World_OnWorldEvent(int32, int32, int32) {}
WEAK void World_OnRecord(uint16, int32, const uint8 *, int32) {}
WEAK void World_GameLoaded(void) {}
WEAK void World_BeginProcess(void) {}
WEAK void World_PreWorld(void) {}
WEAK void World_Reset(void) {}
WEAK void World_OnEvent(int32, int32, In &) {}
WEAK bool World_TestCommand(const char *) { return false; }
WEAK void World_Debug(void) {}
WEAK bool World_CheatRequested(const char *) { return false; }

WEAK void Social_OnChat(int32, int32, const char *, const char *) {}
WEAK void Social_OnDenied(int32, int32) {}
WEAK void Social_AddLine(int32, const char *) {}
WEAK void Social_BeginProcess(void) {}
WEAK void Social_RenderOverlay(void) {}
WEAK bool Social_KeyDown(int32) { return false; }
WEAK void Social_TextInput(const char *) {}
WEAK bool Social_OverlayHasMouse(void) { return false; }
WEAK void Social_OverlayMouse(float, float) {}
WEAK bool Social_OverlayClick(float, float) { return false; }
WEAK void Social_Reset(void) {}
WEAK void Social_OnEvent(int32, int32, In &) {}
WEAK bool Social_TestCommand(const char *) { return false; }
WEAK void Social_Debug(void) {}
}

// (CTimer's milliseconds: the lights' time without a shared world)
#include "Timer.h"
WEAK uint32 OrthrosMmo::LightsTime(void) { return CTimer::GetTimeInMilliseconds(); }
#endif
