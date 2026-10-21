// The shared world's session (Orthros' docs/mmo.md, section 7 "session"; decisions 1, 2, 4's slow motion, 6): entering a
// shared world — its base, the free mode's start, loaded as a new free game is —, the world made a shared one (the
// story's threads stopped, the save point's leftovers undone, the 100 % perks taken back), the character applied and
// kept up to date on the server (CHAR_SAVE), the pause menu's "Leave Multiplayer" and the way back to the main menu, and
// the single-player rules a shared world has not (missions, slow motion, the debug tools for non-admins, the world's
// clean-up and clock at a death).
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "orthros_extras.h"
#include "Frontend.h"
#include "frontendoption.h"
#include "General.h"
#include "Timer.h"
#include "Game.h"
#include "World.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Wanted.h"
#include "Automobile.h"
#include "Bike.h"
#include "Script.h"
#include "Pickups.h"
#include "Streaming.h"
#include "Directory.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "WeaponInfo.h"
#include "Camera.h"
#include "Timecycle.h"
#include "CarCtrl.h"
#include "Rubbish.h"
#include "PathFind.h"
#include "Restart.h"
#include "Draw.h"
#include "DMAudio.h"

namespace OrthrosMmo
{
// ---------------------------------------------------------------- rules and numbers
enum {
	SAVE_PERIOD = 10000,  // (ms: the character saved this often while played…)
	SAVE_SOON = 1000,     // (…and this soon after its money or weapons changed…)
	SAVE_MIN_GAP = 2000,  // (…but not more often than this for a change)
	LEAVE_WAIT = 3000,    // (LEAVE sent on an open link: the main menu when LEFT arrives, or after this)
	LEAVE_GIVE_UP = 30000, // (the link lost while leaving: the last save waits this long for a reconnection)
	JOIN_STALL = 3000,    // (WELCOME received: the world's load has started within this, or the join is given up)
	TEST_WAIT = 20000,    // (the "join" test command's steps)
};

// The threads of the free mode's start a shared world keeps (the root, stunt bonuses, the car parks, the safehouses'
// pickups, ambient sounds, food vendors, the interiors, Ammu-Nation, hardware stores, the bank vault, the army base's
// soldiers); every other one is the story's or a side activity's (save points, phone calls, odd jobs, races…).
static const char *keepThreads[] = {
	"main", "hj", "cargen", "pickups", "audio1", "fud", "shit", "ammu1", "ammu2", "ammu3", "hard1", "hard2", "hard3",
	"ambbank", "armybas",
};
// The ones stopped again whenever a script starts one later (COMMAND_SCRIPT_NAME).
static const char *stopThreads[] = {
	"psave1", "psave2", "psave3", "psave4", "psave5", "psave6", "psave7", "psave8", "psave9", "psave10",
	"save1", "save2", "save3", "save4", "save5", "save6", "save7",
	"cell", "gangmem", "cokrun", "shoot", "o4x4_1", "bmxloop", "multi", "taxi_l", "ambula", "fire", "cop", "pizz",
	"rchely1", "rc_race", "rc_plne", "heli1", "heli2", "heli3", "heli4", "stadium", "races", "ice1", "usj", "bus",
	"shop1", "shop2", "shop3", "shop4", "shop5", "securi",
};
// The ped paths the interiors thread's reset switches off (main.scm's subroutine 132298; its "inside" flags are
// Social_LeaveInterior's).
static const float interiorPedRoads[][6] = {
	{ -956.0f, -355.0f, 5.0f, -898.0f, -328.0f, 25.0f },
	{ -414.0f, -597.0f, 12.0f, -332.0f, -555.0f, 30.0f },
	{ 474.0f, 1250.0f, 17.0f, 356.0f, 1003.0f, 32.0f },
	{ 354.9f, -483.1f, 21.0f, 406.0f, -490.0f, 0.0f },
	{ 468.0f, -77.0f, 0.0f, 490.0f, -54.0f, 30.0f },
};

// A new character (decision 2): $5000 and a pistol, at the Ocean View hotel's door (where the interiors thread puts
// whoever leaves the hotel; the ground's coordinates, heading in degrees).
static const int32 NEW_MONEY = 5000;
static const int32 NEW_PISTOL_AMMO = 68;
static const CVector NEW_SPAWN(233.5f, -1278.45f, 11.0f);
static const float NEW_SPAWN_HEADING = 270.0f;

// ---------------------------------------------------------------- the character's state (CHAR_SAVE, WELCOME)
// Opaque to the server; little-endian, version 1:
//   u8 version, f32 x y z (the ped's centre), f32 heading (radians), u8 area, f32 health, f32 armour, i32 money,
//   u8 current weapon slot, u8 n, n x { u8 weapon type, i32 ammo }
// (money also goes in CHAR_SAVE's own i32, for the character lists). A new version must come with a new PROTOCOL
// (mmo_internal.h): the server then refuses the older engines, which cannot read it - and never write over it, as
// ApplyCharacter keeps a state it cannot read.
enum { STATE_VERSION = 1 };

struct SavedState
{
	CVector pos;
	float heading;
	bool onGround; // (pos is the ground's, not the ped's centre)
	int32 area;
	float health, armour;
	int32 money;
	int32 slot;
	int32 numWeapons;
	struct { int32 type, ammo; } weapons[TOTAL_WEAPON_SLOTS];
};

class Blob
{
public:
	uint8 b[128];
	int32 n;
	Blob() : n(0) {}
	void Raw(const void *p, int32 k) { if (n + k <= (int32)sizeof(b)) memcpy(b + n, p, k); n += k; }
	void U8(uint32 v) { uint8 x = v; Raw(&x, 1); }
	void U32(uint32 v) { uint8 x[4] = { (uint8)v, (uint8)(v >> 8), (uint8)(v >> 16), (uint8)(v >> 24) }; Raw(x, 4); }
	void I32(int32 v) { U32((uint32)v); }
	void F32(float v) { uint32 u; memcpy(&u, &v, 4); U32(u); }
};

// ---------------------------------------------------------------- the session's own state
static bool bLeaving;          // (the player chose to leave the world: the main menu next)
static bool bLeaveSent;        // (its last save and LEAVE committed on an open link - sent again after a reconnection)
static double leaveSince;      // (since bLeaveSent, or since the rejoin that will send them)
static double leaveStart;      // (since the player chose to leave)
static bool bLeaveGivenUp;     // (no reconnection in time: the player told, the main menu after LEAVE_WAIT)
static bool bPendingSave;      // (another game chosen from the world: its last save and LEAVE not known delivered yet)
static uint32 pendingCharId;
static bool bPendingRejoin;    // (its character's JOIN sent again after a reconnection, WELCOME awaited)
static double pendingStart;
static bool bStateUnreadable;  // (the server's state of this character could not be read: never written over)
static bool bBustedSaved;      // (the arrest saved as its outcome will make it)
static double welcomeAt;       // (WELCOME: the world's load asked for)
static bool bRejoining;        // (the link came back: JOIN sent again, the world kept as it is)
static bool bUnwantedWorld;    // (the world loaded after its join was given up: the main menu at the next frame)
static bool bDead;             // (between the local player's death and respawn)
static double nextSave, lastSave;
static uint32 lastSignature;
static SavedState lastState;   // (the character as the leave, or the other game, was chosen: what bLeaving or
static bool bHasLastState;     // bPendingSave send)
static CVector safePos;        // (where the character comes back: the last place it stood outdoors, on land)
static float safeHeading;
static bool bHasSafePos;
static char deathSkin[24];

// (the "weapon" test command: given once its models are in)
static struct { int32 type, ammo; bool requested; } pendingWeapons[4];
static int32 numPendingWeapons;

// (the "join" test command)
enum { JT_IDLE, JT_CONNECTING, JT_CREATING, JT_JOINING, JT_LOADING, JT_DONE, JT_FAILED };
static struct {
	int32 step;
	char name[20], skin[24];
	double since;
	uint32 lobbySerial;
	char status[64];
} joinTest = { JT_IDLE, "", "", 0.0, 0, "idle" };

static bool
InList(const char *name, const char **list, int32 count)
{
	for (int32 i = 0; i < count; i++)
		if (strncmp(name, list[i], 8) == 0)
			return true;
	return false;
}

static bool
InWorld(void)
{
	return Mode() == MODE_ACTIVE || Mode() == MODE_LEAVING;
}

static void
JoinTestStatus(int32 step, const char *status)
{
	joinTest.step = step;
	joinTest.since = Now();
	strncpy(joinTest.status, status, sizeof(joinTest.status) - 1);
	joinTest.status[sizeof(joinTest.status) - 1] = '\0';
}

// ---------------------------------------------------------------- skins
// (a special model — Tommy's outfits, the story's characters: a DFF without an IDE entry, worn by renaming the player's
// own model slot — or an ordinary pedestrian's model, worn as the random-look cheat does)
static bool
IsSpecialSkin(const char *skin)
{
	uint32 pos, size;
	return skin[0] != '\0' && CStreaming::ms_pExtraObjectsDir && CStreaming::ms_pExtraObjectsDir->FindItem(skin, pos, size);
}

static void
ApplySkin(CPlayerPed *ped, const char *skin)
{
	if (skin[0] == '\0' || ped->bInVehicle)
		return;
	const int32 old = ped->GetModelIndex();
	if (IsSpecialSkin(skin)) {
		if (old == MI_PLAYER && !CGeneral::faststricmp(CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName(), skin))
			return;
		// (as ChangePlayerModel, Pad.cpp; the player's slot whatever the model worn before)
		ped->Undress(skin);
		CStreaming::LoadAllRequestedModels(false);
		ped->m_modelIndex = MI_PLAYER;
		ped->Dress();
	} else {
		int32 mi = -1;
		CBaseModelInfo *info = CModelInfo::GetModelInfo(skin, &mi);
		if (info == nil || info->GetModelType() != MITYPE_PED || mi == MI_PLAYER || (mi >= MI_SPECIAL01 && mi <= MI_SPECIAL21) || mi == old)
			return;
		// (as ChangePlayerCheat, Pad.cpp)
		AssocGroupId group = ped->m_animGroup;
		ped->DeleteRwObject();
		CStreaming::RequestModel(mi, STREAMFLAGS_DEPENDENCY | STREAMFLAGS_DONT_REMOVE);
		CStreaming::LoadAllRequestedModels(false);
		ped->m_modelIndex = -1;
		ped->SetModelIndex(mi);
		ped->m_animGroup = group;
	}
	if (old != MI_PLAYER && old != ped->GetModelIndex())
		CStreaming::SetModelIsDeletable(old);
}

// ---------------------------------------------------------------- the character's state, read and written
static void
NewCharacter(SavedState *s)
{
	s->pos = NEW_SPAWN;
	s->heading = DEGTORAD(NEW_SPAWN_HEADING);
	s->onGround = true;
	s->area = AREA_MAIN_MAP;
	s->health = 100.0f;
	s->armour = 0.0f;
	s->money = NEW_MONEY;
	s->numWeapons = 1;
	s->weapons[0].type = WEAPONTYPE_COLT45;
	s->weapons[0].ammo = NEW_PISTOL_AMMO;
	s->slot = CWeaponInfo::GetWeaponInfo(WEAPONTYPE_COLT45)->m_nWeaponSlot;
}

static bool
ReadState(const uint8 *data, int32 length, SavedState *s)
{
	if (length <= 0)
		return false;
	In in(data, length);
	if (in.U8() != STATE_VERSION)
		return false;
	s->pos = in.Vec();
	s->heading = in.F32();
	s->onGround = false;
	s->area = in.U8();
	s->health = in.F32();
	s->armour = in.F32();
	s->money = in.I32();
	s->slot = in.U8();
	const int32 n = in.U8();
	s->numWeapons = 0;
	for (int32 i = 0; i < n && in.Ok(); i++) {
		const int32 type = in.U8(), ammo = in.I32();
		if (type > WEAPONTYPE_UNARMED && type < WEAPONTYPE_TOTALWEAPONS && s->numWeapons < TOTAL_WEAPON_SLOTS) {
			s->weapons[s->numWeapons].type = type;
			s->weapons[s->numWeapons].ammo = Clamp(ammo, 0, 99999);
			s->numWeapons++;
		}
	}
	if (!in.Ok())
		return false;
	// (a place off the map — a broken state — : the hotel's door, the rest kept)
	if (!(Abs(s->pos.x) < 4000.0f && Abs(s->pos.y) < 4000.0f && s->pos.z > -50.0f && s->pos.z < 1000.0f)) {
		s->pos = NEW_SPAWN;
		s->heading = DEGTORAD(NEW_SPAWN_HEADING);
		s->onGround = true;
	}
	if (!(Abs(s->heading) < 100.0f))
		s->heading = 0.0f;
	if (!(s->health > 0.0f))
		s->health = 100.0f;
	if (!(s->armour > 0.0f))
		s->armour = 0.0f;
	s->money = Clamp(s->money, 0, 999999999);
	if (s->slot >= TOTAL_WEAPON_SLOTS)
		s->slot = WEAPONSLOT_UNARMED;
	return true;
}

static int32
WriteState(const SavedState &s, uint8 *out, int32 cap)
{
	Blob blob;
	blob.U8(STATE_VERSION);
	blob.F32(s.pos.x);
	blob.F32(s.pos.y);
	blob.F32(s.pos.z);
	blob.F32(s.heading);
	blob.U8(s.area);
	blob.F32(s.health);
	blob.F32(s.armour);
	blob.I32(s.money);
	blob.U8(s.slot);
	blob.U8(s.numWeapons);
	for (int32 i = 0; i < s.numWeapons; i++) {
		blob.U8(s.weapons[i].type);
		blob.I32(s.weapons[i].ammo);
	}
	if (blob.n > cap || blob.n > (int32)sizeof(blob.b))
		return 0;
	memcpy(out, blob.b, blob.n);
	return blob.n;
}

// (where the game's respawn will put the player, as CRestart finds it - a script's override read, not used up)
static void
RestartPoint(bool police, const CVector &from, CVector *pos, float *heading)
{
	if (CRestart::bOverrideRestart) {
		*pos = CRestart::OverridePosition;
		*heading = CRestart::OverrideHeading;
	} else if (police)
		CRestart::FindClosestPoliceRestartPoint(from, pos, heading);
	else
		CRestart::FindClosestHospitalRestartPoint(from, pos, heading);
}

// (the fine of an arrest at a wanted level, as GameLogic.cpp's WBSTATE_BUSTED takes it)
static int32
ArrestFine(int32 wantedLevel)
{
	static const int32 fines[] = { 100, 100, 200, 400, 600, 900, 1500 };
	return fines[Clamp(wantedLevel, 0, (int32)ARRAY_SIZE(fines) - 1)];
}

// (the state as played now; after a death or an arrest, as the respawn will make it - the hospital fee or the fine
// paid, the weapons lost, at the nearest hospital or police station -, so that a tab closed or the world left while the
// player lies dead or is being arrested changes nothing)
static void
CurrentState(SavedState *s, bool afterDeath)
{
	CPlayerPed *ped = FindPlayerPed();
	CPlayerInfo &player = CWorld::Players[CWorld::PlayerInFocus];
	const bool busted = !afterDeath && player.m_WBState == WBSTATE_BUSTED;
	s->pos = bHasSafePos ? safePos : ped->GetPosition();
	s->heading = bHasSafePos ? safeHeading : ped->m_fRotationCur;
	s->onGround = false;
	s->area = AREA_MAIN_MAP;
	s->health = Clamp(ped->m_fHealth, 1.0f, 255.0f);
	s->armour = Clamp(ped->m_fArmour, 0.0f, 255.0f);
	s->money = player.m_nMoney;
	s->slot = ped->m_currentWeapon;
	s->numWeapons = 0;
	for (int32 i = 0; i < TOTAL_WEAPON_SLOTS; i++) {
		CWeapon &weapon = ped->GetWeapon(i);
		if (weapon.m_eWeaponType == WEAPONTYPE_UNARMED)
			continue;
		s->weapons[s->numWeapons].type = weapon.m_eWeaponType;
		s->weapons[s->numWeapons].ammo = weapon.m_nAmmoTotal;
		s->numWeapons++;
	}
	if (afterDeath || busted) {
		CVector restart;
		float heading;
		RestartPoint(busted, ped->GetPosition(), &restart, &heading);
		s->pos = restart + CVector(0.0f, 0.0f, 1.0f);
		s->heading = DEGTORAD(heading);
		s->health = player.m_nMaxHealth;
		s->armour = 0.0f;
		if (busted ? !player.m_bGetOutOfJailFree : !player.m_bGetOutOfHospitalFree)
			s->money = Max(0, s->money - (busted ? ArrestFine(ped->m_pWanted->GetWantedLevel()) : 100));
		// (an arrest takes the weapons even out of jail free: the respawn clears them again after the taxi's set-up)
		if (busted || !player.m_bGetOutOfHospitalFree) {
			s->numWeapons = 0;
			s->slot = WEAPONSLOT_UNARMED;
		}
	}
}

// (what a save is soon after a change of: the money, each weapon and its ammo)
static uint32
Signature(CPlayerPed *ped)
{
	uint32 h = 2166136261u;
	const int32 values[] = { CWorld::Players[CWorld::PlayerInFocus].m_nMoney, ped->m_currentWeapon };
	for (int32 i = 0; i < (int32)ARRAY_SIZE(values); i++)
		h = (h ^ (uint32)values[i]) * 16777619u;
	for (int32 i = 0; i < TOTAL_WEAPON_SLOTS; i++) {
		h = (h ^ (uint32)ped->GetWeapon(i).m_eWeaponType) * 16777619u;
		h = (h ^ (uint32)ped->GetWeapon(i).m_nAmmoTotal) * 16777619u;
	}
	return h;
}

// (the state a save would send now: false without a player)
static bool
StateToSave(SavedState *s, bool afterDeath)
{
	CPlayerPed *ped = FindPlayerPed();
	if (ped == nil)
		return false;
	// (dying already - its last seconds before PED_DEAD, or wasted -: saved as the respawn will make it, never alive at 1
	// health, which the game kills again at once)
	CurrentState(s, afterDeath || CWorld::Players[CWorld::PlayerInFocus].m_WBState == WBSTATE_WASTED ||
	                ped->DyingOrDead() || ped->m_fHealth <= 1.0f);
	return true;
}

static void
SendState(const SavedState &s)
{
	uint8 data[128];
	if (bStateUnreadable)
		return;
	const int32 n = WriteState(s, data, sizeof(data));
	if (n == 0)
		return;
	Out out(C_CHAR_SAVE);
	out.I32(s.money);
	out.U16(n);
	out.Raw(data, n);
	out.End();
}

static void
SaveNow(bool afterDeath)
{
	SavedState s;
	if (!StateToSave(&s, afterDeath))
		return;
	SendState(s);
	lastSave = Now();
	nextSave = lastSave + SAVE_PERIOD;
	lastSignature = Signature(FindPlayerPed());
}

// (the state kept for a leave or another game: sent now and again after a reconnection, the world here may have moved on)
static void
KeepLastState(void)
{
	if (!bHasLastState)
		bHasLastState = StateToSave(&lastState, bDead);
}

static void
SendLastStateAndLeave(void)
{
	if (bHasLastState)
		SendState(lastState);
	RequestLeave();
}

static void
SaveAt(double when)
{
	if (when < nextSave)
		nextSave = when;
}

static bool
OnWheels(CVehicle *veh)
{
	if (veh->bIsInWater || veh->IsRealHeli() || veh->IsRealPlane())
		return false;
	if (veh->IsCar())
		return ((CAutomobile *)veh)->m_nWheelsOnGround > 0;
	if (veh->IsBike())
		return ((CBike *)veh)->m_nWheelsOnGround > 0;
	return false;
}

// (the place a character comes back to: where it last stood outdoors on land, never in the sea nor in the air — a
// boat's or a flying vehicle's place would drown or drop it; an interior's would miss its thread's flags)
static void
TrackSafePosition(CPlayerPed *ped)
{
	if (CGame::currArea != AREA_MAIN_MAP)
		return;
	if (ped->bInVehicle) {
		CVehicle *veh = ped->m_pMyVehicle;
		if (veh == nil || !OnWheels(veh))
			return;
		safePos = veh->GetPosition();
		safeHeading = veh->GetForward().Heading();
	} else {
		if (!ped->bIsStanding || ped->bIsInWater || ped->m_fHealth <= 0.0f)
			return;
		safePos = ped->GetPosition();
		safeHeading = ped->m_fRotationCur;
	}
	bHasSafePos = true;
}

// ---------------------------------------------------------------- the world made a shared one
static void
StopThread(CRunningScript *script)
{
	// (as TERMINATE_THIS_SCRIPT does, Script.cpp)
	if (script->m_bMissionFlag)
		CTheScripts::bAlreadyRunningAMissionScript = false;
	script->RemoveScriptFromList(&CTheScripts::pActiveScripts);
	script->AddScriptToList(&CTheScripts::pIdleScripts);
	script->m_bIsActive = false;
}

static void
PrepareWorld(void)
{
	// (the story's threads, the save points' included: psave2, the one the free mode's start was saved from, stopped
	// half way through its save — the player's controls off, "on a mission" — is undone below)
	for (CRunningScript *script = CTheScripts::pActiveScripts, *next; script; script = next) {
		next = script->next;
		if (!InList(script->m_abScriptName, keepThreads, ARRAY_SIZE(keepThreads)))
			StopThread(script);
	}
	CTheScripts::bAlreadyRunningAMissionScript = false;
	if (CTheScripts::OnAMissionFlag)
		*(int32 *)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag] = 0;

	// (the clothes pickups — each one a new look — and the save points' ones)
	for (int32 i = 0; i < NUMPICKUPS; i++) {
		CPickup &pickup = CPickups::aPickUps[i];
		if (pickup.m_eType != PICKUP_NONE && (pickup.m_eModelIndex == MI_PICKUP_CLOTHES || pickup.m_eModelIndex == MI_PICKUP_SAVEGAME))
			CPickups::RemovePickUp(pickup.m_nIndex << 16 | i);
	}

	// (outdoors: the start was saved in the mansion — main.scm's subroutines 132277 and 132298, as the interiors thread
	// resets everything when its player is in no interior; the interiors' own ped paths switched off as it does)
	Social_LeaveInterior();
	if (CTheScripts::MainScriptSize == MAIN_SCRIPT_SIZE)
		for (int32 i = 0; i < (int32)ARRAY_SIZE(interiorPedRoads); i++) {
			const float *b = interiorPedRoads[i];
			ThePaths.SwitchPedRoadsOffInArea(Min(b[0], b[3]), Max(b[0], b[3]), Min(b[1], b[4]), Max(b[1], b[4]), Min(b[2], b[5]), Max(b[2], b[5]), true);
		}

	// (the player in control again, and no more than a new game's: the start's 100 % perks — 200 health and armour,
	// fireproof, infinite sprint, fast reload — are not a character's; death and arrest cost as in the game)
	CPlayerInfo &player = CWorld::Players[CWorld::PlayerInFocus];
	player.MakePlayerSafe(false);
	player.m_nMaxHealth = 100;
	player.m_nMaxArmour = 100;
	player.m_bInfiniteSprint = false;
	player.m_bFastReload = false;
	player.m_bFireproof = false;
	player.m_bGetOutOfHospitalFree = false;
	player.m_bGetOutOfJailFree = false;
	player.m_bDriveByAllowed = true;
	CTimer::SetTimeScale(1.0f);
}

static void
GiveWeapons(CPlayerPed *ped, const SavedState &s)
{
	ped->ClearWeapons();
	for (int32 i = 0; i < s.numWeapons; i++) {
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo((eWeaponType)s.weapons[i].type);
		if (info->m_nModelId != -1)
			CStreaming::RequestModel(info->m_nModelId, STREAMFLAGS_DONT_REMOVE);
		if (info->m_nModel2Id != -1)
			CStreaming::RequestModel(info->m_nModel2Id, STREAMFLAGS_DONT_REMOVE);
	}
	CStreaming::LoadAllRequestedModels(false);
	for (int32 i = 0; i < s.numWeapons; i++) {
		const eWeaponType type = (eWeaponType)s.weapons[i].type;
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(type);
		int32 ammo = s.weapons[i].ammo;
		if (ammo <= 0 && info->m_eWeaponFire == WEAPON_FIRE_MELEE)
			ammo = 1;
		if (ammo > 0)
			ped->GiveWeapon(type, ammo);
		if (info->m_nModelId != -1)
			CStreaming::SetModelIsDeletable(info->m_nModelId);
		if (info->m_nModel2Id != -1)
			CStreaming::SetModelIsDeletable(info->m_nModel2Id);
	}
	ped->MakeChangesForNewWeapon(ped->HasWeaponSlot(s.slot) ? s.slot : WEAPONSLOT_UNARMED);
}

// (the scene loaded around a place, the player put there — the screen is still faded out after the load)
static void
Place(CPlayerPed *ped, CVector pos, float heading, bool onGround)
{
	CStreaming::LoadSceneCollision(pos);
	CStreaming::LoadScene(pos);
	if (onGround)
		pos.z += ped->GetDistanceFromCentreOfMassToBaseOfModel();
	ped->Teleport(pos);
	ped->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	ped->m_fRotationCur = ped->m_fRotationDest = heading;
	ped->SetHeading(heading);
	TheCamera.RestoreWithJumpCut();
	TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
}

static void
ApplyCharacter(void)
{
	CPlayerPed *ped = FindPlayerPed();
	CPlayerInfo &player = CWorld::Players[CWorld::PlayerInFocus];
	const Character &me = Me();
	SavedState s;
	// (a state this engine cannot read - a newer version's, or broken - is kept on the server as it is: the character
	// played as a new one here, nothing saved over it)
	bStateUnreadable = me.stateLength > 0 && !ReadState(me.state, me.stateLength, &s);
	if (me.stateLength <= 0 || bStateUnreadable)
		NewCharacter(&s);
	if (bStateUnreadable) {
		debug("Orthros MMO: the character's state (%d bytes, version %d) is unreadable: not saved this session\n",
		      me.stateLength, me.state[0]);
		Social_AddLine(LINE_ERROR, LangFrench() ?
		               "Personnage enregistr\xC3\xA9 par une version plus r\xC3\xA9" "cente du jeu : rien ne sera sauvegard\xC3\xA9. Rechargez la page." :
		               "This character was saved by a newer game version: nothing will be saved. Reload the page.");
	}

	ApplySkin(ped, me.skin);
	player.m_nMoney = player.m_nVisibleMoney = s.money;
	GiveWeapons(ped, s);
	ped->m_fHealth = Min(s.health, (float)player.m_nMaxHealth);
	ped->m_fArmour = Min(s.armour, (float)player.m_nMaxArmour);
	ped->ClearAdrenaline();
	ped->m_pWanted->Reset();
	Place(ped, s.pos, s.heading, s.onGround);

	safePos = ped->GetPosition();
	safeHeading = s.heading;
	bHasSafePos = true;
}

// ---------------------------------------------------------------- the pause menu: "Leave Multiplayer" instead of saving
static const char *LEAVE_KEY = "ORT_LMP";

static void BeginLeave(void);

static wchar *
LeaveDraw(bool *disabled, bool)
{
	*disabled = bLeaving;
	return nil;
}

static void
LeavePress(int8 action)
{
	if (action == FEOPTION_ACTION_SELECT)
		BeginLeave();
}

// (the row after "Resume", in a shared world only; the rows after the first placed again from it — the menu writes
// down where it drew each one)
static void
SetPauseLeaveEntry(bool on)
{
	static CMenuScreenCustom::CMenuEntry entry;
	CMenuScreenCustom::CMenuEntry *entries = aScreens[MENUPAGE_PAUSE_MENU].m_aEntries;
	int32 count = 0, at = -1;
	while (count < NUM_MENUROWS && entries[count].m_Action != MENUACTION_NOTHING) {
		if (entries[count].m_Action == MENUACTION_CFO_DYNAMIC && strcmp(entries[count].m_EntryName, LEAVE_KEY) == 0)
			at = count;
		count++;
	}
	if (on == (at >= 0))
		return;
	if (on) {
		if (count < 1 || count >= NUM_MENUROWS)
			return;
		if (entry.m_Action == MENUACTION_NOTHING) {
			entry.m_Action = MENUACTION_CFO_DYNAMIC;
			strncpy(entry.m_EntryName, LEAVE_KEY, sizeof(entry.m_EntryName));
			entry.m_CFODynamic = new CCFODynamic(nil, nil, nil, LeaveDraw, LeavePress);
			entry.m_Align = MENUALIGN_CENTER;
		}
		memmove(&entries[2], &entries[1], (count - 1) * sizeof(entries[0]));
		entries[1] = entry;
		count++;
	} else {
		memmove(&entries[at], &entries[at + 1], (count - at - 1) * sizeof(entries[0]));
		memset(&entries[count - 1], 0, sizeof(entries[0]));
		count--;
	}
	for (int32 i = 1; i < count; i++)
		entries[i].m_X = entries[i].m_Y = 0;
}

// ---------------------------------------------------------------- leaving
static void
ResetModules(void)
{
	Players_Reset();
	Entities_Reset();
	Combat_Reset();
	World_Reset();
	Social_Reset();
}

static void
ResetSession(void)
{
	bLeaving = false;
	bLeaveSent = false;
	bLeaveGivenUp = false;
	bRejoining = false;
	bDead = false;
	bBustedSaved = false;
	bStateUnreadable = false;
	bHasSafePos = false;
	numPendingWeapons = 0;
}

// (back to the game's start menu from a game: the game shut down at the end of this frame, then the start as when the
// game starts — sdl2.cpp's main loop restarts to GS_INIT_FRONTEND when m_bFirstTime is set, after CGame::ShutDown, as
// the pause menu's "New Game" does before CGame::Initialise; without it a restart starts a new story game.
// GS_FRONTEND starts that game too unless its first frame opens the menu, CMenuManager::SwitchMenuOnAndOff: the pause
// menu, if open, closed without its closing steps — the restart unloads its textures — so that the start opens it anew
// as at the game's start, no request left to close it, and the screen unfaded and not widescreen as CCamera::Init
// leaves it then — the menus answer nothing else, and nothing fades them in before a game starts)
static void
BackToMainMenu(void)
{
	if (FrontEndMenuManager.m_bGameNotLoaded)
		return;
	FrontEndMenuManager.m_bFirstTime = true;
	FrontEndMenuManager.m_bWantToLoad = false;
	FrontEndMenuManager.m_bWantToRestart = true;
	FrontEndMenuManager.m_bMenuActive = false;
	FrontEndMenuManager.m_bShutDownFrontEndRequested = false;
	FrontEndMenuManager.m_bStartUpFrontEndRequested = false;
	FrontEndMenuManager.m_bWaitingForNewKeyBind = false;
	FrontEndMenuManager.pEditString = nil;
	FrontEndMenuManager.pControlEdit = nil;
	TheCamera.m_bFading = false;
	TheCamera.m_fFLOATingFade = 0.0f;
	TheCamera.m_bMusicFading = false;
	TheCamera.m_fFLOATingFadeMusic = 0.0f;
	TheCamera.m_WideScreenOn = false;
	TheCamera.m_bWantsToSwitchWidescreenOff = false;
	TheCamera.m_ScreenReductionPercentage = 0.0f;
	CDraw::FadeValue = 0;
	DMAudio.SetEffectsFadeVol(127);
	DMAudio.SetMusicFadeVol(127);
}

// (the pause menu's rows follow at its next opening: PauseMenuOpening)
static void
FinishLeave(void)
{
	ResetModules();
	ResetSession();
	SetMode(MODE_OFF);
	Disconnect();
	BackToMainMenu();
}

// (the last save and LEAVE: known sent only on an open link - the page drops what it cannot send, and a reconnection
// then rejoins and sends them again, Session_LinkReset)
static void
SendLeave(void)
{
	SendLastStateAndLeave();
	bLeaveSent = LinkState() == LINK_OPEN;
	leaveSince = Now();
}

// (the state as the leave is chosen: the main menu once the server has it - LEFT -, never before while the link is lost
// and may come back, LEAVE_GIVE_UP)
static void
BeginLeave(void)
{
	if (Mode() != MODE_ACTIVE || bLeaving)
		return;
	bHasLastState = false;
	KeepLastState();
	bLeaving = true;
	bLeaveGivenUp = false;
	leaveStart = Now();
	SetMode(MODE_LEAVING);
	SendLeave();
	if (!bLeaveSent)
		Social_AddLine(LINE_NOTICE, LangFrench() ? "Serveur injoignable : sauvegarde avant de partir..." :
		                                           "Server unreachable: saving before leaving...");
}

// (leaving, every frame: LEFT not come on an open link - the server slow -, or the link not back in time)
static void
LeaveStep(void)
{
	const double now = Now();
	if (bLeaveGivenUp || (bLeaveSent && LinkState() == LINK_OPEN && !bRejoining)) {
		if (now - leaveSince > LEAVE_WAIT)
			FinishLeave();
	} else if (now - leaveStart > LEAVE_GIVE_UP) {
		debug("Orthros MMO: leaving without the last save, the link did not come back\n");
		Social_AddLine(LINE_ERROR, LangFrench() ? "Serveur injoignable : la progression depuis la coupure est perdue." :
		                                          "Server unreachable: the progress since the link was lost is not saved.");
		bLeaveGivenUp = true;
		leaveSince = now;
	}
}

// (another game chosen while the world's last save may not have gone: sent again after a reconnection, the character
// rejoined for it - Session_LinkReset, Session_Welcome -, until LEFT or LEAVE_GIVE_UP)
static void
PendingSaveStep(void)
{
	if (!bPendingSave || Now() - pendingStart <= LEAVE_GIVE_UP)
		return;
	debug("Orthros MMO: the last save before another game was not delivered, the link did not come back\n");
	bPendingSave = false;
	if (Mode() == MODE_OFF && !FrontEndMenuManager.m_bGameNotLoaded)
		Disconnect();
}

// (the join given up: the server told; from the menus, back to the start page, the lobby's link kept; from a game — the
// world's load failed and the game started another —, back to the main menu as when leaving)
static void
AbortJoin(void)
{
	OrthrosExtras::CancelSharedWorld();
	RequestLeave();
	ResetSession();
	if (FrontEndMenuManager.m_bGameNotLoaded)
		SetMode(LinkState() == LINK_OPEN ? MODE_LOBBY : MODE_OFF);
	else {
		ResetModules();
		SetMode(MODE_OFF);
		Disconnect();
		BackToMainMenu();
	}
	if (joinTest.step == JT_LOADING)
		JoinTestStatus(JT_FAILED, "failed: the world did not load");
}

// (the server took us out of the world — another tab joined with the character, the account signed out —: left at once,
// nothing to save)
static void
DropOut(void)
{
	switch (Mode()) {
	case MODE_ACTIVE:
	case MODE_LEAVING:
		ResetModules();
		ResetSession();
		SetMode(MODE_OFF);
		BackToMainMenu();
		break;
	case MODE_JOINING:
		OrthrosExtras::CancelSharedWorld();
		ResetSession();
		SetMode(MODE_OFF);
		break;
	}
}

// ---------------------------------------------------------------- every frame (main menu and game)
static void
JoinTestFail(const char *why)
{
	char status[64];
	snprintf(status, sizeof(status), "failed: %s", why);
	JoinTestStatus(JT_FAILED, status);
}

static void
JoinTestChoose(void)
{
	const LobbyData &lobby = Lobby();
	for (int32 i = 0; i < lobby.count; i++)
		if (!CGeneral::faststricmp(lobby.chars[i].name, joinTest.name)) {
			RequestJoin(lobby.chars[i].id);
			JoinTestStatus(JT_JOINING, "joining");
			return;
		}
	RequestCreate(joinTest.name, joinTest.skin);
	JoinTestStatus(JT_CREATING, "creating");
}

static void
JoinTestStep(void)
{
	const double waited = Now() - joinTest.since;
	switch (joinTest.step) {
	case JT_CONNECTING:
		if (LinkState() == LINK_REFUSED) {
			char why[32];
			snprintf(why, sizeof(why), "refused %d", Refusal());
			JoinTestFail(why);
		} else if (LinkState() == LINK_OPEN && Lobby().valid && Lobby().serial != joinTest.lobbySerial)
			JoinTestChoose();
		else if (waited > TEST_WAIT)
			JoinTestFail("no lobby");
		break;
	case JT_CREATING:
	case JT_JOINING:
		if (waited > TEST_WAIT)
			JoinTestFail(joinTest.step == JT_CREATING ? "no answer to create" : "no welcome");
		break;
	case JT_LOADING:
		if (Mode() == MODE_ACTIVE)
			JoinTestStatus(JT_DONE, "in world");
		break;
	}
}

// (the mode as the link is, outside a world: in the lobby while connected or connecting)
static void
FollowLink(void)
{
	const int32 link = LinkState();
	const bool linked = link == LINK_CONNECTING || link == LINK_OPEN || link == LINK_RETRY;
	if (Mode() == MODE_OFF && linked)
		SetMode(MODE_LOBBY);
	else if (Mode() == MODE_LOBBY && !linked)
		SetMode(MODE_OFF);
}

static void
EveryFrame(void)
{
	if (bUnwantedWorld) {
		bUnwantedWorld = false;
		BackToMainMenu();
	}
	if (bLeaving)
		LeaveStep();
	PendingSaveStep();
	if (Mode() == MODE_JOINING && Now() - welcomeAt > JOIN_STALL && !FrontEndMenuManager.m_bWantToLoad)
		AbortJoin();
	JoinTestStep();
}

void
Session_FrontendFrame(void)
{
	if (FrontEndMenuManager.m_bGameNotLoaded)
		FollowLink();
	EveryFrame();
}

void
Session_BeginProcess(void)
{
	EveryFrame();
	if (Mode() != MODE_ACTIVE || numPendingWeapons == 0)
		return;
	// (the "weapon" test command's: given once their models are in, never waited for)
	CPlayerPed *ped = FindPlayerPed();
	for (int32 i = 0; i < numPendingWeapons; ) {
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo((eWeaponType)pendingWeapons[i].type);
		if (!pendingWeapons[i].requested) {
			if (info->m_nModelId != -1)
				CStreaming::RequestModel(info->m_nModelId, STREAMFLAGS_DONT_REMOVE);
			if (info->m_nModel2Id != -1)
				CStreaming::RequestModel(info->m_nModel2Id, STREAMFLAGS_DONT_REMOVE);
			pendingWeapons[i].requested = true;
		}
		if ((info->m_nModelId != -1 && !CStreaming::HasModelLoaded(info->m_nModelId)) ||
		    (info->m_nModel2Id != -1 && !CStreaming::HasModelLoaded(info->m_nModel2Id))) {
			i++;
			continue;
		}
		ped->GiveWeapon((eWeaponType)pendingWeapons[i].type, pendingWeapons[i].ammo);
		if (!ped->bInVehicle)
			ped->MakeChangesForNewWeapon((eWeaponType)pendingWeapons[i].type);
		if (info->m_nModelId != -1)
			CStreaming::SetModelIsDeletable(info->m_nModelId);
		if (info->m_nModel2Id != -1)
			CStreaming::SetModelIsDeletable(info->m_nModel2Id);
		pendingWeapons[i] = pendingWeapons[--numPendingWeapons];
	}
}

void
Session_PostWorld(void)
{
	if (Mode() != MODE_ACTIVE || bLeaving)
		return;
	CPlayerPed *ped = FindPlayerPed();
	if (ped == nil)
		return;
	TrackSafePosition(ped);
	// (arrested: saved at once as the arrest's outcome will make it - the fine paid, the weapons taken -, as a death is)
	if (!bDead && !bBustedSaved && CWorld::Players[CWorld::PlayerInFocus].m_WBState == WBSTATE_BUSTED) {
		bBustedSaved = true;
		SaveNow(false);
	}
	if (bDead || CWorld::Players[CWorld::PlayerInFocus].m_WBState != WBSTATE_PLAYING || ped->m_fHealth <= 0.0f)
		return;
	const double now = Now();
	const uint32 signature = Signature(ped);
	if (signature != lastSignature) {
		lastSignature = signature;
		SaveAt(Max(now + SAVE_SOON, lastSave + SAVE_MIN_GAP));
	}
	if (now >= nextSave)
		SaveNow(false);
}

// ---------------------------------------------------------------- the server's records
void
Session_Welcome(void)
{
	switch (Mode()) {
	case MODE_ACTIVE:
		// (the link came back and the server has us again: the world and the character stay as they are here — newer
		// than the server's copy, which a save brings up to date at once)
		bRejoining = false;
		bBustedSaved = false;
		SaveAt(Now());
		return;
	case MODE_JOINING:
		return;
	case MODE_LEAVING:
		// (rejoined to leave: the last save and LEAVE again, the ones sent before may have been lost with the link)
		if (bRejoining && !bLeaveGivenUp) {
			bRejoining = false;
			SendLeave();
		}
		return;
	}
	if (bPendingSave && bPendingRejoin && Me().id == pendingCharId) {
		bPendingRejoin = false;
		// (rejoined only for the last save of the world left for another game)
		SendLastStateAndLeave();
		return;
	}
	SetMode(MODE_JOINING);
	welcomeAt = Now();
	if (joinTest.step == JT_JOINING || joinTest.step == JT_CREATING)
		JoinTestStatus(JT_LOADING, "loading");
	OrthrosExtras::StartSharedWorld();
}

void
Session_Refused(int32 reason)
{
	if (joinTest.step >= JT_CONNECTING && joinTest.step <= JT_LOADING) {
		char why[32];
		snprintf(why, sizeof(why), "refused %d", reason);
		JoinTestFail(why);
	}
	bPendingSave = false; // (no reconnection after a refusal)
	if (Mode() == MODE_LOBBY)
		SetMode(MODE_OFF);
	else
		DropOut();
}

void
Session_CharResult(int32 op, int32 status, uint32 id)
{
	if (joinTest.step == JT_CREATING && op == 2) {
		if (status == 0) {
			RequestJoin(id);
			JoinTestStatus(JT_JOINING, "joining");
		} else {
			char why[32];
			snprintf(why, sizeof(why), "create status %d", status);
			JoinTestFail(why);
		}
		return;
	}
	if (op != 5 || status == 0)
		return;
	if (joinTest.step == JT_JOINING) {
		char why[32];
		snprintf(why, sizeof(why), "join status %d", status);
		JoinTestFail(why);
	}
	// (the world would not take us back after the link came back - full now -: left; leaving, the last save lost)
	if (Mode() == MODE_ACTIVE && bRejoining) {
		DropOut();
		Disconnect();
	} else if (Mode() == MODE_LEAVING && bRejoining) {
		debug("Orthros MMO: leaving without the last save, the world did not take the character back\n");
		FinishLeave();
	} else if (bPendingSave && (Mode() == MODE_OFF || Mode() == MODE_LOBBY)) {
		bPendingSave = false;
		if (Mode() == MODE_OFF && !FrontEndMenuManager.m_bGameNotLoaded)
			Disconnect();
	}
}

void
Session_Left(void)
{
	if (bLeaving)
		FinishLeave();
	else {
		// (a rejoin only to save: what it was told of the others forgotten)
		if (bPendingSave && !InWorld() && Mode() != MODE_JOINING)
			Players_Reset();
		bPendingSave = false;
		bPendingRejoin = false;
		if (Mode() == MODE_OFF && !FrontEndMenuManager.m_bGameNotLoaded)
			Disconnect(); // (left for another game: no more link)
	}
}

void
Session_LinkReset(void)
{
	switch (Mode()) {
	case MODE_ACTIVE:
	case MODE_JOINING:
		// (the same character, the game going on: the world as it is here)
		RequestJoin(Me().id);
		bRejoining = true;
		break;
	case MODE_LEAVING:
		// (the last save and LEAVE may have been lost with the link: the character rejoined to send them again, Session_Welcome)
		if (bLeaveGivenUp)
			break;
		RequestJoin(Me().id);
		bRejoining = true;
		bLeaveSent = false;
		leaveSince = Now();
		break;
	case MODE_OFF:
	case MODE_LOBBY:
		if (bPendingSave) {
			RequestJoin(pendingCharId);
			bPendingRejoin = true;
		}
		else if (Mode() == MODE_OFF && !FrontEndMenuManager.m_bGameNotLoaded)
			Disconnect();
		break;
	}
	if (joinTest.step == JT_CREATING || joinTest.step == JT_JOINING) {
		joinTest.lobbySerial = Lobby().serial;
		JoinTestStatus(JT_CONNECTING, "connecting");
	}
}

void
Session_PlayerDied(void)
{
	if (Mode() != MODE_ACTIVE || bDead)
		return;
	bDead = true;
	const char *skin = CModelInfo::GetModelInfo(MI_PLAYER)->GetModelName();
	strncpy(deathSkin, skin, sizeof(deathSkin) - 1);
	deathSkin[sizeof(deathSkin) - 1] = '\0';
	SaveNow(true);
}

void
Session_PlayerRespawned(void)
{
	bDead = false;
	bBustedSaved = false;
	if (Mode() != MODE_ACTIVE)
		return;
	CPlayerPed *ped = FindPlayerPed();
	safePos = ped->GetPosition();
	safeHeading = ped->m_fRotationCur;
	bHasSafePos = true;
	SaveNow(false);
}

// ---------------------------------------------------------------- the game's hooks (mmo.h)
void
GameLoaded(void)
{
	if (Mode() != MODE_JOINING) {
		// (its join given up while it loaded; this frame's end is the load's, the restart asked for at the next one)
		bUnwantedWorld = true;
		return;
	}
	PrepareWorld();
	ApplyCharacter();
	Players_GameLoaded();
	Entities_GameLoaded();
	World_GameLoaded();
	SetMode(MODE_ACTIVE);
	bLeaving = false;
	bDead = false;
	bRejoining = false;
	CPlayerPed *ped = FindPlayerPed();
	lastSignature = Signature(ped);
	lastSave = Now();
	// (soon saved: a new character's state on the server at once)
	nextSave = lastSave + SAVE_SOON;
}

void
GameStarting(bool shared)
{
	if (shared)
		return;
	switch (Mode()) {
	case MODE_ACTIVE:
	case MODE_LEAVING:
		// (from the world: its last save - the one a leave already chose - kept until LEFT says it arrived; a reconnection
		// before sends it again, Session_LinkReset)
		if (!bLeaving)
			bHasLastState = false;
		KeepLastState();
		if (bStateUnreadable)
			bHasLastState = false;
		if (!bLeaving || !bLeaveSent)
			SendLastStateAndLeave();
		if (!bLeaveGivenUp) {
			bPendingSave = true;
			bPendingRejoin = false;
			pendingCharId = Me().id;
			pendingStart = Now();
		}
		// fall through
	case MODE_JOINING:
		// (another game chosen in the shared world's pause menu, or while its join was on: left — the link closes once
		// LEFT arrives)
		if (Mode() == MODE_JOINING)
			RequestLeave();
		OrthrosExtras::CancelSharedWorld();
		if (InWorld())
			ResetModules();
		ResetSession();
		SetMode(MODE_OFF);
		break;
	case MODE_LOBBY:
		// (from the lobby's pages to another game: the link has nothing more to do)
		Disconnect();
		SetMode(MODE_OFF);
		break;
	}
}

bool
ScriptNamed(CRunningScript *script)
{
	if (!IsActive() || !InList(script->m_abScriptName, stopThreads, ARRAY_SIZE(stopThreads)))
		return false;
	StopThread(script);
	return true;
}

const char *
PlayerSkin(const char *original)
{
	if (!IsActive())
		return original;
	if (IsSpecialSkin(Me().skin))
		return Me().skin;
	// (an ordinary pedestrian's look — worn by model index, kept by Dress — or none: the slot as it was at the death)
	return IsSpecialSkin(deathSkin) ? deathSkin : original;
}

bool
DebugAllowed(void)
{
	return !IsActive() || IsAdmin();
}

void
PauseMenuOpening(void)
{
	SetPauseLeaveEntry(InWorld());
}

// ---------------------------------------------------------------- test commands (?mmotest) and debug
static bool
Command(const char *cmd, const char *name, const char **args)
{
	const size_t n = strlen(name);
	if (strncmp(cmd, name, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' '))
		return false;
	*args = cmd + n;
	while (**args == ' ')
		(*args)++;
	return true;
}

bool
Session_TestCommand(const char *cmd)
{
	const char *args;
	CPlayerPed *ped = InWorld() ? FindPlayerPed() : nil;
	if (Command(cmd, "join", &args)) {
		// join <name> [skin]: from the main menu, the character created if missing
		char name[20] = "", skin[24] = "";
		sscanf(args, "%19s %23s", name, skin);
		if (name[0] == '\0') {
			JoinTestFail("no name");
			return true;
		}
		if (Mode() == MODE_JOINING || InWorld()) {
			JoinTestFail("already in a world");
			return true;
		}
		if (!NetAvailable()) {
			JoinTestFail("no shared world here");
			return true;
		}
		strcpy(joinTest.name, name);
		strcpy(joinTest.skin, skin[0] ? skin : "player");
		joinTest.lobbySerial = Lobby().serial;
		JoinTestStatus(JT_CONNECTING, "connecting");
		Connect();
		if (LinkState() == LINK_OPEN && Lobby().valid)
			JoinTestChoose();
		return true;
	}
	if (Command(cmd, "leave", &args)) {
		if (Mode() == MODE_JOINING)
			AbortJoin();
		else
			BeginLeave();
		return true;
	}
	if (Command(cmd, "menu", &args)) {
		FrontEndMenuManager.m_bStartUpFrontEndRequested = true;
		return true;
	}
	if (ped == nil)
		return false;
	if (Command(cmd, "tp", &args)) {
		// tp <x> <y> <z>: z under the map's floor = on the ground there
		CVector pos;
		if (sscanf(args, "%f %f %f", &pos.x, &pos.y, &pos.z) != 3)
			return true;
		CEntity *entity = FindPlayerEntity();
		CStreaming::LoadSceneCollision(pos);
		if (pos.z <= MAP_Z_LOW_LIMIT)
			pos.z = CWorld::FindGroundZForCoord(pos.x, pos.y) + entity->GetDistanceFromCentreOfMassToBaseOfModel();
		entity->Teleport(pos);
		return true;
	}
	if (Command(cmd, "weapon", &args)) {
		// weapon <type> <ammo>
		int32 type = 0, ammo = 0;
		if (sscanf(args, "%d %d", &type, &ammo) == 2 && type > WEAPONTYPE_UNARMED && type < WEAPONTYPE_TOTALWEAPONS &&
		    numPendingWeapons < (int32)ARRAY_SIZE(pendingWeapons)) {
			pendingWeapons[numPendingWeapons].type = type;
			pendingWeapons[numPendingWeapons].ammo = Clamp(ammo, 1, 99999);
			pendingWeapons[numPendingWeapons].requested = false;
			numPendingWeapons++;
		}
		return true;
	}
	if (Command(cmd, "money", &args)) {
		CPlayerInfo &player = CWorld::Players[CWorld::PlayerInFocus];
		const int32 money = atoi(args);
		player.m_nMoney = player.m_nVisibleMoney = Clamp(money, 0, 999999999);
		return true;
	}
	if (Command(cmd, "die", &args)) {
		// (as the suicide cheat)
		ped->InflictDamage(nil, WEAPONTYPE_UNARMED, 1000.0f, PEDPIECE_TORSO, 0);
		return true;
	}
	return false;
}

// (a name in JSON: the server's names are plain ASCII, quotes and backslashes taken out all the same)
static void
JsonText(const char *in, char *out, int32 size)
{
	int32 n = 0;
	for (; *in && n < size - 1; in++)
		if (*in != '"' && *in != '\\' && (uint8)*in >= 0x20)
			out[n++] = *in;
	out[n] = '\0';
}

void
Session_Debug(void)
{
	char text[64];
	CPlayerPed *ped = InWorld() ? FindPlayerPed() : nil;
	JsonText(joinTest.status, text, sizeof(text));
	DebugAppend(",\"inWorld\":%s,\"join\":\"%s\",\"leaving\":%s", ped ? "true" : "false", text, bLeaving ? "true" : "false");
	JsonText(Me().name, text, sizeof(text));
	DebugAppend(",\"char\":\"%s\",\"charId\":%u,\"fade\":%d,\"menuActive\":%s,\"paused\":%s", text, Me().id,
	            TheCamera.GetScreenFadeStatus(), FrontEndMenuManager.m_bMenuActive ? "true" : "false",
	            CTimer::GetIsPaused() ? "true" : "false");
	if (ped == nil)
		return;
	const CVector &pos = ped->GetPosition();
	CWeapon *weapon = ped->GetWeapon();
	DebugAppend(",\"pos\":[%.2f,%.2f,%.2f],\"heading\":%.1f,\"area\":%d,\"health\":%.1f,\"armour\":%.1f,\"money\":%d",
	            pos.x, pos.y, pos.z, RADTODEG(ped->m_fRotationCur), CGame::currArea, ped->m_fHealth, ped->m_fArmour,
	            CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
	DebugAppend(",\"weapon\":%d,\"ammo\":%d,\"weapons\":[", weapon->m_eWeaponType, weapon->m_nAmmoTotal);
	bool first = true;
	for (int32 i = 0; i < TOTAL_WEAPON_SLOTS; i++) {
		CWeapon &w = ped->GetWeapon(i);
		if (w.m_eWeaponType == WEAPONTYPE_UNARMED)
			continue;
		DebugAppend("%s[%d,%d]", first ? "" : ",", w.m_eWeaponType, w.m_nAmmoTotal);
		first = false;
	}
	DebugAppend("],\"wanted\":%d,\"dead\":%s", ped->m_pWanted->GetWantedLevel(),
	            CWorld::Players[CWorld::PlayerInFocus].m_WBState != WBSTATE_PLAYING ? "true" : "false");
	if (ped->bInVehicle && ped->m_pMyVehicle)
		DebugAppend(",\"vehicle\":%d", ped->m_pMyVehicle->GetModelIndex());
	else
		DebugAppend(",\"vehicle\":null");
}
}
#endif
