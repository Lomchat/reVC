// The shared world's players (docs/mmo.md, section 7; the owner's decisions 5 and 9: each sees the others, their names
// and wanted stars over their heads):
//   - the local player's state (PLAYER_STATE, unreliable) after CWorld::Process: about 15 a second while moving or
//     acting, 2 while still, at once on a big change (vehicle in or out, death, weapon, area, wanted level, a jump);
//   - one puppet per other player near enough: a CCivilianPed (UNK_CHAR: no AI, never deleted by the game's removers)
//     in the player's skin (a special model slot from MI_SPECIAL21 down, shared by equal skins, or an ordinary
//     pedestrian's model), moved by this module (interpolated 100 ms behind on the wall clock, big jumps snapped), its
//     animations mirrored from the owner's strongest ones, its weapon in hand, its arms aimed by IK, seated in the
//     vehicle the entities module replicates, dead and revived as its owner, hidden in another area;
//   - the name tags: the character's pseudo above each visible puppet within 70 m, its wanted stars below, "(away)".
//
// PLAYER_STATE body (little-endian; the core reads x, y, z; version 1):
//   f32 x, y, z        the ped's position (its centre; seated: its seat)
//   u8  version (1), u8 life (+1 at every discontinuity: teleport, respawn, area change - the others snap)
//   u32 time           the sender's ServerNow() in ms (low 32 bits): the sample's time on every client
//   u16 heading        m_fRotationCur, [0, 2 pi) over 65536
//   i16 vx, vy, vz     velocity, m/s x 100 (the vehicle's when seated)
//   u8  move state (eMoveState), u8 ped state (PedState, informative)
//   u16 flags          PF_* below
//   u8  weapon (eWeaponType in hand), u8 health, u8 armour (rounded up, 255 max), u8 wanted level, u8 area
//   u32 vehicle        its network id (NetIdOf), 0 on foot or not networked
//   u8  seat           0 driver, 1 + n passenger n, 255 none
//   u16 aim yaw        world, as heading; i16 aim pitch (rad x 10000)
//   u8  n (0..4), n x { u8 group (AssocGroupId), u16 anim (AnimationId), u16 time (ms), u8 blend (x 255),
//                       u8 speed (x 64), u8 flags (AF_*) }  the clump's strongest associations, strongest first
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "skeleton.h"
#include "General.h"
#include "Pad.h"
#include "Camera.h"
#include "Font.h"
#include "Sprite.h"
#include "Hud.h"
#include "Game.h"
#include "World.h"
#include "Pools.h"
#include "Streaming.h"
#include "Directory.h"
#include "ModelInfo.h"
#include "ModelIndices.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "CivilianPed.h"
#include "Wanted.h"
#include "Vehicle.h"
#include "WeaponInfo.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "AnimBlendAssocGroup.h"
#include "RpAnimBlend.h"
#include "Timer.h"

namespace OrthrosMmo
{
enum { STATE_VERSION = 1, MAX_ANIMS = 4, NUM_SAMPLES = 12, NUM_SPECIAL = MI_SPECIAL21 - MI_SPECIAL01 + 1, NO_SEAT = 255 };
// PLAYER_STATE flags
enum {
	PF_DUCKING = 1, PF_IN_AIR = 2, PF_AIMING = 4, PF_DEAD = 8, PF_SEATED = 0x10, PF_DRIVER = 0x20, PF_SHOOTING = 0x40,
	PF_WEAPON_SHOWN = 0x80, PF_LANDING = 0x100, PF_ENTERING = 0x200, PF_LEAVING = 0x400, PF_STANDING = 0x800,
};
// an animation's flags
enum { AF_RUNNING = 1, AF_REPEAT = 2, AF_PARTIAL = 4 };

static const double INTERP_MS = 100.0;      // the puppets shown this far behind their owners
static const double EXTRAPOLATE_MS = 250.0; // at most this far past the last state
static const double SEND_MOVING_MS = 66.0, SEND_STILL_MS = 500.0;
static const float CREATE_RANGE = 280.0f, DELETE_RANGE = 300.0f; // (from the local player)
static const float TAG_RANGE = 70.0f, TAG_FADE = 50.0f;
static const float SNAP_DISTANCE = 30.0f;   // a jump between two states this long, too fast to be a move: snapped

struct AnimState {
	uint8 group, flags;
	uint16 anim;
	float time, blend, speed;
};

struct PlayerState {
	CVector pos, velocity;
	uint8 life, moveState, pedState, weapon, health, armour, wanted, area, seat;
	uint16 flags;
	float heading, aimYaw, aimPitch;
	uint32 vehicle;
	int32 numAnims;
	AnimState anims[MAX_ANIMS];
};

struct Sample {
	double t; // its time on this client's wall clock (Now())
	PlayerState s;
};

// One other player (by slot): its states and its puppet.
struct Remote {
	bool used;
	uint32 rank;               // (who it is: a slot given again to someone else is told apart)
	CPed *ped;                 // the puppet (a registered reference: nil when the game deletes it)
	int32 model, special;      // its model, its special slot (-1: an ordinary pedestrian's model)
	char skin[24];             // the skin it wears
	Sample samples[NUM_SAMPLES];
	int32 count;
	bool full;                 // the states are this module's layout (else positions only)
	uint8 life;
	bool dead;
	int32 weapon;              // the weapon in its hand (-1: none given yet)
	bool aiming, restoringGun;
	float aimYaw, aimPitch;
	CVehicle *vehicle;         // the vehicle it is seated in (a registered reference)
	int32 seat;
	uint64 blocks;             // the animation blocks it holds a reference on (bit n: block n)
	bool tagDrawn;             // its tag drawn at the last Render2d
	bool visible;
	uint32 states;             // states received
};

// A special model slot (MI_SPECIAL01 + index): its skin, the puppets wearing it.
struct Special {
	char name[24];
	int32 refs;
};

static Remote remotes[MAX_SLOTS];
static Special specials[NUM_SPECIAL];
static void Ready(void);

// The local player's side.
static struct {
	bool haveSent, havePrevious, wasDead;
	PlayerState sent;           // the last state sent
	double sentAt, shootingUntil;
	CVector previousPos;        // last frame's position (teleports seen)
	double previousAt;
	int32 previousArea;
	uint8 life;
	uint32 count, bytes, lastSize;
	char skinSeen[24];          // the model the local player wears, told to the others once stable
	double skinSince;
} local;

// The test commands' controls (?mmotest): the walk's end in the game's own time (a slow test machine still walks
// as far), the aim's on the wall clock.
static struct {
	int32 aimSlot;
	double aimUntil;
	uint32 walkUntil;
	bool sprint;
	int32 jumpFrames, crouchFrames;
} test = { -1 };

// ---------------------------------------------------------------- small helpers
static inline float
Clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline bool
Finite(float v)
{
	return v == v && v < 1e30f && v > -1e30f;
}

static uint32
AngleToU16(float a)
{
	if (!Finite(a))
		return 0;
	a = fmodf(a, TWOPI);
	if (a < 0.0f)
		a += TWOPI;
	return (uint32)(a * (65536.0f / TWOPI)) & 0xFFFF;
}

static float
U16ToAngle(uint32 v)
{
	return CGeneral::LimitRadianAngle(v * (TWOPI / 65536.0f));
}

static float
LerpAngle(float a, float b, float f)
{
	return CGeneral::LimitRadianAngle(a + CGeneral::LimitRadianAngle(b - a) * f);
}

static uint32
ServerMs32(void)
{
	return (uint32)(uint64)ServerNow();
}

static bool
SameName(const char *a, const char *b)
{
	return CGeneral::faststricmp(a, b) == 0;
}

static void
CopyName(char *dst, const char *src, int32 size)
{
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

// ---------------------------------------------------------------- skins and model slots
// (a skin: a special model - Tommy's outfits, the story's characters: DFFs without an IDE entry, worn through a special
// slot - or an ordinary pedestrian's model, worn by its index; anything else wears Tommy's "player")
struct Skin {
	bool special, female;
	int32 model; // the ordinary model's index (special: -1)
	char name[24];
};

static bool
IsSpecialName(const char *name)
{
	uint32 pos, size;
	return name[0] != '\0' && CStreaming::ms_pExtraObjectsDir && CStreaming::ms_pExtraObjectsDir->FindItem(name, pos, size);
}

static void
ResolveSkin(const char *wanted, Skin *skin)
{
	static const char *const women[] = { "igcandy", "igmerc", "igmerc2" };
	const char *name = wanted && wanted[0] ? wanted : "player";
	skin->model = -1;
	skin->female = false;
	if (!IsSpecialName(name)) {
		int32 mi = -1;
		CBaseModelInfo *info = CModelInfo::GetModelInfo(name, &mi);
		if (info && info->GetModelType() == MITYPE_PED && mi != MI_PLAYER && (mi < MI_SPECIAL01 || mi > MI_SPECIAL21)) {
			const ePedType type = ((CPedModelInfo *)info)->m_pedType;
			skin->special = false;
			skin->model = mi;
			skin->female = type == PEDTYPE_CIVFEMALE || type == PEDTYPE_PROSTITUTE;
			CopyName(skin->name, name, sizeof(skin->name));
			return;
		}
		name = "player";
	}
	skin->special = true;
	CopyName(skin->name, name, sizeof(skin->name));
	for (const char *w : women)
		if (SameName(name, w))
			skin->female = true;
}

static bool
SpecialHolds(int32 i, const char *name)
{
	return SameName(CModelInfo::GetModelInfo(MI_SPECIAL01 + i)->GetModelName(), name);
}

// (a special slot for this skin: one already wearing it, else a free one - from MI_SPECIAL21 down, the scripts taking
// theirs from MI_SPECIAL01 up - renamed; -1 if none)
static int32
TakeSpecial(const char *name)
{
	for (int32 i = NUM_SPECIAL - 1; i >= 0; i--)
		if (specials[i].name[0] && SameName(specials[i].name, name) && SpecialHolds(i, name)) {
			specials[i].refs++;
			return i;
		}
	for (int32 pass = 0; pass < 2; pass++)
		for (int32 i = NUM_SPECIAL - 1; i >= 0; i--) {
			Special &s = specials[i];
			// (first the slots never used here, then the ones no puppet wears any more; never one the game uses)
			if (s.refs > 0 || (pass == 0 && s.name[0]) || CModelInfo::GetModelInfo(MI_SPECIAL01 + i)->GetNumRefs() > 0)
				continue;
			CStreaming::RequestSpecialChar(i, name, STREAMFLAGS_DONT_REMOVE);
			CopyName(s.name, name, sizeof(s.name));
			s.refs = 1;
			return i;
		}
	return -1;
}

static void
ReleaseSpecial(int32 i)
{
	if (i < 0 || i >= NUM_SPECIAL || specials[i].refs <= 0)
		return;
	if (--specials[i].refs == 0)
		CStreaming::SetMissionDoesntRequireSpecialChar(i);
}

// ---------------------------------------------------------------- the states read and written
static void
WriteState(Out &o, const PlayerState &s, uint32 time)
{
	o.Vec(s.pos);
	o.U8(STATE_VERSION);
	o.U8(s.life);
	o.U32(time);
	o.U16(AngleToU16(s.heading));
	o.I16((int32)Clampf(s.velocity.x * 100.0f, -32767.0f, 32767.0f));
	o.I16((int32)Clampf(s.velocity.y * 100.0f, -32767.0f, 32767.0f));
	o.I16((int32)Clampf(s.velocity.z * 100.0f, -32767.0f, 32767.0f));
	o.U8(s.moveState);
	o.U8(s.pedState);
	o.U16(s.flags);
	o.U8(s.weapon);
	o.U8(s.health);
	o.U8(s.armour);
	o.U8(s.wanted);
	o.U8(s.area);
	o.U32(s.vehicle);
	o.U8(s.seat);
	o.U16(AngleToU16(s.aimYaw));
	o.I16((int32)Clampf(s.aimPitch * 10000.0f, -32767.0f, 32767.0f));
	o.U8(s.numAnims);
	for (int32 i = 0; i < s.numAnims; i++) {
		const AnimState &a = s.anims[i];
		o.U8(a.group);
		o.U16(a.anim);
		o.U16((uint32)Clampf(a.time * 1000.0f, 0.0f, 65535.0f));
		o.U8((uint32)Clampf(a.blend * 255.0f + 0.5f, 0.0f, 255.0f));
		o.U8((uint32)Clampf(a.speed * 64.0f + 0.5f, 0.0f, 255.0f));
		o.U8(a.flags);
	}
}

// (false: not this module's layout - the position alone is known)
static bool
ReadState(In &in, PlayerState &s, uint32 *time)
{
	memset(&s, 0, sizeof(s));
	s.pos = in.Vec();
	s.seat = NO_SEAT;
	s.health = 100;
	s.moveState = PEDMOVE_STILL;
	if (!in.Ok() || in.Left() < 2 || in.U8() != STATE_VERSION)
		return false;
	s.life = in.U8();
	*time = in.U32();
	s.heading = U16ToAngle(in.U16());
	s.velocity.x = in.I16() / 100.0f;
	s.velocity.y = in.I16() / 100.0f;
	s.velocity.z = in.I16() / 100.0f;
	s.moveState = in.U8();
	s.pedState = in.U8();
	s.flags = in.U16();
	s.weapon = in.U8();
	s.health = in.U8();
	s.armour = in.U8();
	s.wanted = in.U8();
	s.area = in.U8();
	s.vehicle = in.U32();
	s.seat = in.U8();
	s.aimYaw = U16ToAngle(in.U16());
	s.aimPitch = in.I16() / 10000.0f;
	const int32 n = in.U8();
	for (int32 i = 0; i < n && in.Ok(); i++) {
		AnimState a;
		a.group = in.U8();
		a.anim = in.U16();
		a.time = in.U16() / 1000.0f;
		a.blend = in.U8() / 255.0f;
		a.speed = in.U8() / 64.0f;
		a.flags = in.U8();
		if (i < MAX_ANIMS && in.Ok())
			s.anims[s.numAnims++] = a;
	}
	if (s.weapon >= WEAPONTYPE_TOTALWEAPONS)
		s.weapon = WEAPONTYPE_UNARMED;
	return in.Ok();
}

// ---------------------------------------------------------------- the local player's state
static void
CaptureAnims(CPed *ped, PlayerState &s)
{
	s.numAnims = 0;
	RpClump *clump = ped->GetClump();
	if (clump == nil)
		return;
	for (CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(clump); a; a = RpAnimBlendGetNextAssociation(a)) {
		if (a->groupId < 0 || a->groupId >= NUM_ANIM_ASSOC_GROUPS || a->animId < 0 || a->hierarchy == nil || a->blendAmount < 0.02f)
			continue;
		AnimState as;
		as.group = (uint8)a->groupId;
		as.anim = (uint16)a->animId;
		as.time = a->currentTime;
		as.blend = Min(a->blendAmount, 1.0f);
		as.speed = Max(a->speed, 0.0f);
		as.flags = (a->IsRunning() ? AF_RUNNING : 0) | (a->IsRepeating() ? AF_REPEAT : 0) | (a->IsPartial() ? AF_PARTIAL : 0);
		// (the strongest ones kept, strongest first)
		int32 at = s.numAnims;
		while (at > 0 && s.anims[at - 1].blend < as.blend)
			at--;
		if (at >= MAX_ANIMS)
			continue;
		const int32 last = Min(s.numAnims, MAX_ANIMS - 1);
		for (int32 k = last; k > at; k--)
			s.anims[k] = s.anims[k - 1];
		s.anims[at] = as;
		if (s.numAnims < MAX_ANIMS)
			s.numAnims++;
	}
}

static void
Capture(CPlayerPed *ped, PlayerState &s, double now)
{
	memset(&s, 0, sizeof(s));
	CVehicle *veh = ped->bInVehicle ? ped->m_pMyVehicle : nil;
	s.pos = ped->GetPosition();
	s.heading = ped->m_fRotationCur;
	s.velocity = (veh ? veh->m_vecMoveSpeed : ped->m_vecMoveSpeed) * 50.0f;
	s.moveState = (uint8)ped->m_nMoveState;
	s.pedState = (uint8)ped->m_nPedState;
	if (ped->bIsShooting)
		local.shootingUntil = now + 250.0;
	uint32 f = 0;
	if (ped->bIsDucking) f |= PF_DUCKING;
	if (ped->bIsInTheAir) f |= PF_IN_AIR;
	if (ped->bIsLanding) f |= PF_LANDING;
	if (ped->bIsStanding) f |= PF_STANDING;
	if (ped->bIsAimingGun) f |= PF_AIMING;
	if (ped->DyingOrDead() || ped->m_fHealth <= 0.0f) f |= PF_DEAD;
	if (now < local.shootingUntil) f |= PF_SHOOTING;
	if (ped->m_pWeaponModel) f |= PF_WEAPON_SHOWN;
	if (ped->EnteringCar()) f |= PF_ENTERING;
	if (ped->m_nPedState == PED_EXIT_CAR) f |= PF_LEAVING;
	s.seat = NO_SEAT;
	if (veh) {
		f |= PF_SEATED;
		if (veh->pDriver == ped) {
			f |= PF_DRIVER;
			s.seat = 0;
		} else
			for (int32 i = 0; i < (int32)ARRAY_SIZE(veh->pPassengers); i++)
				if (veh->pPassengers[i] == ped)
					s.seat = 1 + i;
		s.vehicle = NetIdOf(veh);
	}
	s.flags = f;
	s.weapon = (uint8)ped->GetWeapon()->m_eWeaponType;
	s.health = (uint8)Clampf(ceilf(ped->m_fHealth), 0.0f, 255.0f);
	s.armour = (uint8)Clampf(ceilf(ped->m_fArmour), 0.0f, 255.0f);
	s.wanted = ped->m_pWanted ? (uint8)Clamp(ped->m_pWanted->GetWantedLevel(), 0, 255) : 0;
	s.area = (uint8)CGame::currArea;
	// (the aim: at the locked target as CPedIK::PointGunAtPosition, else the free aim's direction and pitch)
	s.aimYaw = ped->m_fLookDirection;
	s.aimPitch = ped->m_fFPSMoveHeading;
	if (ped->m_pSeekTarget && ped->bIsAimingGun) {
		const CVector target = ped->m_pSeekTarget->GetPosition(), from = ped->GetPosition();
		s.aimYaw = CGeneral::GetRadianAngleBetweenPoints(target.x, target.y, from.x, from.y);
		s.aimPitch = CGeneral::GetRadianAngleBetweenPoints(target.z, (target - from).Magnitude2D(), from.z + 0.5f, 0.0f);
	}
	if (!Finite(s.aimYaw) || Abs(s.aimYaw) > 100.0f)
		s.aimYaw = s.heading;
	if (!Finite(s.aimPitch) || Abs(s.aimPitch) > 3.0f)
		s.aimPitch = 0.0f;
	CaptureAnims(ped, s);
}

// (a change the others must see at once)
static bool
BigChange(const PlayerState &a, const PlayerState &b)
{
	const uint32 flags = PF_DUCKING | PF_AIMING | PF_DEAD | PF_SEATED | PF_WEAPON_SHOWN | PF_ENTERING | PF_LEAVING;
	return a.life != b.life || a.weapon != b.weapon || a.area != b.area || a.wanted != b.wanted || a.vehicle != b.vehicle ||
	       a.seat != b.seat || ((a.flags ^ b.flags) & flags) != 0;
}

static bool
Moving(const PlayerState &s)
{
	return s.velocity.MagnitudeSqr() > 0.2f * 0.2f || s.moveState > PEDMOVE_STILL ||
	       (s.flags & (PF_AIMING | PF_SHOOTING | PF_IN_AIR | PF_LANDING | PF_ENTERING | PF_LEAVING)) != 0 ||
	       (s.numAnims > 0 && (s.anims[0].flags & AF_RUNNING) && !(s.anims[0].flags & AF_REPEAT));
}

static void
SendLocal(double now)
{
	CPlayerPed *ped = FindPlayerPed();
	if (ped == nil || LocalSlot() == NO_SLOT)
		return;
	const bool dead = ped->DyingOrDead() || ped->m_fHealth <= 0.0f;
	// (discontinuities: the others snap instead of sliding there)
	const CVector pos = ped->GetPosition();
	if (local.havePrevious) {
		const float moved = (pos - local.previousPos).Magnitude();
		const float speed = (ped->bInVehicle && ped->m_pMyVehicle ? ped->m_pMyVehicle->m_vecMoveSpeed : ped->m_vecMoveSpeed).Magnitude() * 50.0f;
		const float dt = (float)Max(now - local.previousAt, 1.0) / 1000.0f;
		if ((moved > 20.0f && moved > speed * dt * 2.0f + 10.0f) || (local.wasDead && !dead) || CGame::currArea != local.previousArea)
			local.life++;
	}
	local.havePrevious = true;
	local.previousPos = pos;
	local.previousAt = now;
	local.previousArea = CGame::currArea;
	local.wasDead = dead;

	PlayerState s;
	Capture(ped, s, now);
	s.life = local.life;
	const bool due = !local.haveSent || BigChange(s, local.sent) ||
	                 now - local.sentAt >= (Moving(s) || Moving(local.sent) ? SEND_MOVING_MS : SEND_STILL_MS);
	if (!due)
		return;
	Out o(R_PLAYER_STATE, true);
	WriteState(o, s, ServerMs32());
	if (!o.ok)
		return;
	local.lastSize = o.n;
	local.bytes += o.n;
	local.count++;
	local.sent = s;
	local.sentAt = now;
	local.haveSent = true;
}

// (the local player's skin changed in the world - a skin cheat, an administrator's: the others told, through the
// character's PLAYER_INFO; the session dresses the player at the join and the respawn with Me().skin, equal to it)
static void
WatchLocalSkin(double now)
{
	CPlayerPed *ped = FindPlayerPed();
	if (ped == nil || ped->bInVehicle || ped->GetModelIndex() < 0)
		return;
	const char *name = CModelInfo::GetModelInfo(ped->GetModelIndex())->GetModelName();
	if (!SameName(name, local.skinSeen)) {
		CopyName(local.skinSeen, name, sizeof(local.skinSeen));
		local.skinSince = now;
		return;
	}
	if (now - local.skinSince < 2000.0 || Me().skin[0] == '\0' || SameName(name, Me().skin))
		return;
	// (the server's skin rule: [A-Za-z0-9_]{1,20})
	const int32 k = (int32)strlen(name);
	if (k < 1 || k > 20)
		return;
	for (int32 i = 0; i < k; i++)
		if (!isalnum((uint8)name[i]) && name[i] != '_')
			return;
	debug("Orthros MMO: the local skin is now %s, told to the others\n", name);
	SetMySkin(name);
}

// ---------------------------------------------------------------- the others' states
static void
ClearRecord(Remote &r)
{
	r.used = false;
	r.rank = 0;
	r.ped = nil;
	r.model = r.special = -1;
	r.skin[0] = '\0';
	r.count = 0;
	r.full = false;
	r.life = 0;
	r.dead = false;
	r.weapon = -1;
	r.aiming = r.restoringGun = false;
	r.aimYaw = r.aimPitch = 0.0f;
	r.vehicle = nil;
	r.seat = -1;
	r.blocks = 0;
	r.tagDrawn = false;
	r.visible = false;
	r.states = 0;
}

static const PlayerState *
Latest(const Remote &r)
{
	return r.count > 0 ? &r.samples[r.count - 1].s : nil;
}

void
Players_OnState(int32 from, In &in)
{
	Ready();
	if (from < 0 || from >= MAX_SLOTS)
		return;
	RemotePlayer *p = Player(from);
	if (p == nil)
		return;
	Remote &r = remotes[from];
	if (!r.used || r.rank != p->rank) {
		ClearRecord(r);
		r.used = true;
		r.rank = p->rank;
	}
	Sample smp;
	uint32 time = 0;
	const bool full = ReadState(in, smp.s, &time);
	const double now = Now();
	double age = 0.0;
	if (full) {
		age = (double)(int32)(ServerMs32() - time);
		age = age < 0.0 ? 0.0 : age > 5000.0 ? 5000.0 : age;
	}
	smp.t = now - age;
	r.states++;
	p->area = smp.s.area;
	p->wanted = smp.s.wanted;
	if (r.count > 0) {
		const Sample &last = r.samples[r.count - 1];
		if (smp.t <= last.t)
			smp.t = last.t + 1.0;
		// (a jump: a teleport, a respawn - the puppet snaps there, no slide across the map)
		const float d = (smp.s.pos - last.s.pos).Magnitude();
		const double dt = Max(smp.t - last.t, 1.0) / 1000.0;
		if ((full && smp.s.life != r.life) || (d > SNAP_DISTANCE && d / dt > 80.0))
			r.count = 0;
	}
	r.life = smp.s.life;
	r.full = full;
	if (r.count == NUM_SAMPLES) {
		memmove(&r.samples[0], &r.samples[1], sizeof(Sample) * (NUM_SAMPLES - 1));
		r.count--;
	}
	r.samples[r.count++] = smp;
}

// (the state to show at time t: the position and heading interpolated between the two states around it - or carried
// on from the last one by its velocity, a little; the rest from the state just before t)
static const PlayerState *
StateAt(const Remote &r, double t, CVector *pos, float *heading, const PlayerState **next, float *f, double *since)
{
	*next = nil;
	*f = 0.0f;
	*since = 0.0;
	if (r.count == 0)
		return nil;
	const Sample &first = r.samples[0];
	if (t <= first.t) {
		*pos = first.s.pos;
		*heading = first.s.heading;
		return &first.s;
	}
	for (int32 i = 0; i + 1 < r.count; i++) {
		const Sample &a = r.samples[i], &b = r.samples[i + 1];
		if (t >= a.t && t < b.t) {
			const float k = (float)((t - a.t) / (b.t - a.t));
			*pos = a.s.pos + (b.s.pos - a.s.pos) * k;
			*heading = LerpAngle(a.s.heading, b.s.heading, k);
			*next = &b.s;
			*f = k;
			*since = t - a.t;
			return &a.s;
		}
	}
	const Sample &last = r.samples[r.count - 1];
	const double ahead = Min(t - last.t, EXTRAPOLATE_MS);
	const bool still = (last.s.flags & PF_DEAD) != 0;
	*pos = still ? last.s.pos : last.s.pos + last.s.velocity * (float)(ahead / 1000.0);
	*heading = last.s.heading;
	*since = t - last.t;
	return &last.s;
}

// ---------------------------------------------------------------- the puppets' animations
static int32
BlockIndex(const CAnimBlendAssocGroup &g)
{
	if (g.animBlock == nil)
		return -1;
	const int32 b = (int32)(g.animBlock - CAnimManager::GetAnimationBlock(0));
	return b >= 0 && b < 64 && b < NUMANIMBLOCKS ? b : -1;
}

// (a reference on the group's block while one of its animations plays on the puppet: streaming leaves it loaded)
static void
HoldBlock(Remote &r, int32 block)
{
	if (block < 0 || (r.blocks >> block) & 1)
		return;
	CAnimManager::AddAnimBlockRef(block);
	r.blocks |= (uint64)1 << block;
}

static void
ReleaseBlocks(Remote &r)
{
	for (int32 b = 0; b < NUMANIMBLOCKS && b < 64; b++)
		if ((r.blocks >> b) & 1) {
			CAnimBlock *block = CAnimManager::GetAnimationBlock(b);
			if (block->isLoaded && block->refCount > 0)
				CAnimManager::RemoveAnimBlockRefWithoutDelete(b);
		}
	r.blocks = 0;
}

// (the blocks still used by the puppet's animations kept, the others let go)
static void
TrimBlocks(Remote &r, CPed *ped)
{
	if (r.blocks == 0 || ped->GetClump() == nil)
		return;
	uint64 used = 0;
	CAnimBlendAssocGroup *groups = CAnimManager::GetAnimAssocGroups();
	for (CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(ped->GetClump()); a; a = RpAnimBlendGetNextAssociation(a))
		if (a->groupId >= 0 && a->groupId < NUM_ANIM_ASSOC_GROUPS) {
			const int32 b = BlockIndex(groups[a->groupId]);
			if (b >= 0)
				used |= (uint64)1 << b;
		}
	const uint64 drop = r.blocks & ~used;
	for (int32 b = 0; b < NUMANIMBLOCKS && b < 64; b++)
		if ((drop >> b) & 1)
			CAnimManager::RemoveAnimBlockRefWithoutDelete(b);
	r.blocks &= used;
}

static float
AnimTime(const AnimState &a, CAnimBlendAssociation *assoc, double since)
{
	float t = a.time;
	if (a.flags & AF_RUNNING)
		t += (float)(since / 1000.0) * a.speed;
	const float length = assoc->hierarchy ? assoc->hierarchy->totalLength : 0.0f;
	if (length > 0.0f) {
		if (a.flags & AF_REPEAT)
			t = fmodf(t, length);
		else
			t = Min(t, length);
	}
	return t;
}

// (the owner's strongest animations played as they are there; the puppet's others faded out)
static void
MirrorAnims(Remote &r, CPed *ped, const PlayerState &s, const PlayerState *next, float f, double since)
{
	RpClump *clump = ped->GetClump();
	if (clump == nil)
		return;
	CAnimBlendAssocGroup *groups = CAnimManager::GetAnimAssocGroups();
	CAnimBlendAssociation *kept[MAX_ANIMS + 1];
	int32 numKept = 0;
	for (int32 i = 0; i < s.numAnims; i++) {
		const AnimState &a = s.anims[i];
		if (a.group >= NUM_ANIM_ASSOC_GROUPS)
			continue;
		CAnimBlendAssocGroup &g = groups[a.group];
		const int32 block = BlockIndex(g);
		if (g.assocList == nil) {
			// (a group not loaded here - an idle's, a weapon's not in hand: asked for, played once there)
			if (block >= 0)
				CStreaming::RequestAnim(block, STREAMFLAGS_DEPENDENCY);
			continue;
		}
		if ((int32)a.anim < g.firstAnimId || (int32)a.anim >= g.firstAnimId + g.numAssociations)
			continue;
		CAnimBlendAssociation *assoc = nil;
		for (CAnimBlendAssociation *x = RpAnimBlendClumpGetFirstAssociation(clump); x; x = RpAnimBlendGetNextAssociation(x))
			if (x->animId == (int16)a.anim && x->groupId == (int16)a.group) {
				assoc = x;
				break;
			}
		if (assoc == nil) {
			HoldBlock(r, block);
			assoc = CAnimManager::AddAnimation(clump, (AssocGroupId)a.group, (AnimationId)a.anim);
			if (assoc == nil)
				continue;
			assoc->SetCurrentTime(AnimTime(a, assoc, since));
		}
		float blend = a.blend;
		if (next)
			for (int32 k = 0; k < next->numAnims; k++)
				if (next->anims[k].group == a.group && next->anims[k].anim == a.anim) {
					blend += (next->anims[k].blend - a.blend) * f;
					break;
				}
		assoc->blendAmount = blend;
		assoc->blendDelta = 0.0f;
		assoc->speed = a.speed;
		assoc->flags &= ~(ASSOC_FADEOUTWHENDONE | ASSOC_DELETEFADEDOUT);
		if (a.flags & AF_RUNNING)
			assoc->flags |= ASSOC_RUNNING;
		else
			assoc->flags &= ~ASSOC_RUNNING;
		const float want = AnimTime(a, assoc, since);
		float drift = Abs(assoc->currentTime - want);
		const float length = assoc->hierarchy ? assoc->hierarchy->totalLength : 0.0f;
		if ((a.flags & AF_REPEAT) && length > 0.0f)
			drift = Min(drift, length - drift);
		if (drift > 0.15f || !(a.flags & AF_RUNNING))
			assoc->SetCurrentTime(want);
		kept[numKept++] = assoc;
	}
	// (nothing of the owner's playable here: the move state's own walk, run or stand)
	if (numKept == 0) {
		AnimationId id = ANIM_STD_IDLE;
		AssocGroupId group = ped->m_animGroup;
		if (ped->bInVehicle)
			id = ANIM_STD_CAR_SIT, group = ASSOCGRP_STD;
		else if (r.dead)
			id = ANIM_STD_KO_FRONT, group = ASSOCGRP_STD;
		else if (s.moveState == PEDMOVE_WALK)
			id = ANIM_STD_WALK;
		else if (s.moveState == PEDMOVE_RUN || s.moveState == PEDMOVE_JOG)
			id = ANIM_STD_RUN;
		else if (s.moveState == PEDMOVE_SPRINT)
			id = ANIM_STD_RUNFAST;
		CAnimBlendAssociation *assoc = RpAnimBlendClumpGetAssociation(clump, id);
		if (assoc == nil || assoc->groupId != group)
			assoc = CAnimManager::AddAnimation(clump, group, id);
		if (assoc) {
			assoc->blendAmount = 1.0f;
			assoc->blendDelta = 0.0f;
			assoc->flags &= ~(ASSOC_FADEOUTWHENDONE | ASSOC_DELETEFADEDOUT);
			kept[numKept++] = assoc;
		}
	}
	for (CAnimBlendAssociation *x = RpAnimBlendClumpGetFirstAssociation(clump); x; x = RpAnimBlendGetNextAssociation(x)) {
		bool listed = false;
		for (int32 k = 0; k < numKept; k++)
			listed |= kept[k] == x;
		if (!listed && (x->blendDelta >= 0.0f || !(x->flags & ASSOC_DELETEFADEDOUT))) {
			x->blendDelta = -8.0f;
			x->flags |= ASSOC_DELETEFADEDOUT;
		}
	}
	ped->m_pVehicleAnim = nil; // (the mirror owns the puppet's animations: no pointer kept on one it may fade)
}

// ---------------------------------------------------------------- the puppets
static void
ClearCollision(CPed *ped)
{
	ped->bIsInSafePosition = true;
	ped->bWasPostponed = false;
	ped->m_nCollisionRecords = 0;
	ped->bHasCollided = false;
	ped->m_pDamageEntity = nil;
	ped->m_fDamageImpulse = 0.0f;
	ped->m_nDamagePieceType = 0;
}

static void
Unseat(Remote &r, CPed *ped)
{
	CVehicle *veh = ped->m_pMyVehicle;
	if (veh) {
		if (veh->pDriver == ped)
			veh->pDriver = nil; // (not RemoveDriver: the vehicle's status is its owner's)
		else
			veh->RemovePassenger(ped);
		veh->CleanUpOldReference((CEntity **)&ped->m_pMyVehicle);
	}
	ped->m_pMyVehicle = nil;
	ped->bInVehicle = false;
	ped->m_pVehicleAnim = nil;
	if (r.vehicle)
		r.vehicle->CleanUpOldReference((CEntity **)&r.vehicle);
	r.vehicle = nil;
	r.seat = -1;
	ped->SetPedState(r.dead ? PED_DIE : PED_IDLE);
}

// (in the owner's seat: as WarpPedIntoCar without what it does to the vehicle - its status, engine, counters)
static bool
Seat(Remote &r, CPed *ped, CVehicle *veh, int32 seat)
{
	if (ped->bInVehicle && ped->m_pMyVehicle == veh && r.seat == seat &&
	    (seat == 0 ? veh->pDriver == ped : seat - 1 < (int32)ARRAY_SIZE(veh->pPassengers) && veh->pPassengers[seat - 1] == ped))
		return true;
	if (ped->bInVehicle || ped->m_pMyVehicle)
		Unseat(r, ped);
	if (seat == 0) {
		if (veh->pDriver && veh->pDriver != ped)
			return false;
		veh->SetDriver(ped);
	} else {
		const int32 n = seat - 1;
		if (n >= veh->m_nNumMaxPassengers || veh->pPassengers[n] != nil || !veh->AddPassenger(ped, n))
			return false;
	}
	ped->m_pMyVehicle = veh;
	veh->RegisterReference((CEntity **)&ped->m_pMyVehicle);
	ped->bInVehicle = true;
	ped->m_pVehicleAnim = nil;
	ped->SetPedState(PED_DRIVING);
	ped->bUsesCollision = false;
	r.vehicle = veh;
	veh->RegisterReference((CEntity **)&r.vehicle);
	r.seat = seat;
	return true;
}

static void
ForgetPuppet(int32 slot, Remote &r)
{
	ReleaseSpecial(r.special);
	r.special = -1;
	r.model = -1;
	r.skin[0] = '\0';
	r.weapon = -1;
	r.dead = r.aiming = r.restoringGun = false;
	if (r.vehicle)
		r.vehicle->CleanUpOldReference((CEntity **)&r.vehicle);
	r.vehicle = nil;
	r.seat = -1;
	r.visible = false;
	RemotePlayer *p = Player(slot);
	if (p && (p->puppet == r.ped || r.ped == nil))
		p->puppet = nil;
	r.ped = nil;
}

// (outside CWorld::Process: Pump, PreWorld, PostWorld)
static void
DeletePuppet(int32 slot, Remote &r)
{
	CPed *ped = r.ped;
	if (ped) {
		if (ped->bInVehicle || ped->m_pMyVehicle)
			Unseat(r, ped);
		ped->CleanUpOldReference((CEntity **)&r.ped);
		r.ped = nil;
		Unregister(ped);
		RemotePlayer *p = Player(slot);
		if (p && p->puppet == ped)
			p->puppet = nil;
		CWorld::Remove(ped);
		CWorld::RemoveReferencesToDeletedObject(ped);
		delete ped;
	}
	ReleaseBlocks(r);
	ForgetPuppet(slot, r);
}

// (the skin's model asked for; true once it can be worn)
static bool
SkinReady(const Skin &skin, int32 *special, int32 *model)
{
	if (skin.special) {
		if (*special < 0)
			*special = TakeSpecial(skin.name);
		if (*special < 0)
			return false;
		CStreaming::RequestSpecialChar(*special, skin.name, STREAMFLAGS_DONT_REMOVE);
		*model = MI_SPECIAL01 + *special;
		return CStreaming::HasSpecialCharLoaded(*special);
	}
	*model = skin.model;
	if (!CStreaming::HasModelLoaded(skin.model)) {
		CStreaming::RequestModel(skin.model, STREAMFLAGS_DEPENDENCY);
		return false;
	}
	return true;
}

// The skin a slot's puppet is being dressed in (the model may take frames to come): per slot.
static struct { char name[24]; int32 special; } pendingSkin[MAX_SLOTS];

static void
DropPending(int32 slot)
{
	if (pendingSkin[slot].special >= 0)
		ReleaseSpecial(pendingSkin[slot].special);
	pendingSkin[slot].special = -1;
	pendingSkin[slot].name[0] = '\0';
}

// (the tables' empty state, once: no slot is the special slot 0)
static void
Ready(void)
{
	static bool ready;
	if (ready)
		return;
	ready = true;
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		ClearRecord(remotes[i]);
		pendingSkin[i].special = -1;
		pendingSkin[i].name[0] = '\0';
	}
}

static CPed *
CreatePuppet(int32 slot, Remote &r, const Skin &skin, int32 model, int32 special, const CVector &pos, float heading)
{
	if (CPools::GetPedPool()->GetNoOfFreeSpaces() < 4)
		return nil;
	CCivilianPed *ped = new CCivilianPed(skin.female ? PEDTYPE_CIVFEMALE : PEDTYPE_CIVMALE, model);
	ped->CharCreatedBy = UNK_CHAR;
	ped->bRespondsToThreats = false;
	ped->bAllowMedicsToReviveMe = false;
	ped->SetIsStatic(false);
	ped->SetPosition(pos);
	ped->SetHeading(heading);
	ped->m_fRotationCur = ped->m_fRotationDest = heading;
	ped->bIsInSafePosition = true;
	ped->SetPedState(PED_IDLE);
	CWorld::Add(ped);
	NetEntity *ne = Register(ped, PlayerNetId(slot), slot, true, KIND_PED);
	if (ne)
		ne->player = (int8)slot;
	r.ped = ped;
	ped->RegisterReference((CEntity **)&r.ped);
	r.model = model;
	r.special = special;
	CopyName(r.skin, skin.name, sizeof(r.skin));
	r.weapon = -1;
	r.dead = false;
	r.seat = -1;
	r.vehicle = nil;
	if (RemotePlayer *p = Player(slot))
		p->puppet = ped;
	debug("Orthros MMO: puppet of player %d (%s), model %d\n", slot, skin.name, model);
	return ped;
}

// (the weapon the owner holds, in the puppet's hand once its models are here)
static void
ApplyWeapon(Remote &r, CPed *ped, const PlayerState &s)
{
	const eWeaponType type = (eWeaponType)s.weapon;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(type);
	const int32 m1 = info->m_nModelId, m2 = info->m_nModel2Id;
	const bool ready = (m1 < 0 || CStreaming::HasModelLoaded(m1)) && (m2 < 0 || CStreaming::HasModelLoaded(m2));
	if (r.weapon != type) {
		if (!ready) {
			if (m1 >= 0) CStreaming::RequestModel(m1, STREAMFLAGS_DEPENDENCY);
			if (m2 >= 0) CStreaming::RequestModel(m2, STREAMFLAGS_DEPENDENCY);
			if (r.weapon < 0) // (until then, empty hands)
				r.weapon = WEAPONTYPE_UNARMED;
			return;
		}
		ped->ClearWeapons();
		if (type != WEAPONTYPE_UNARMED) {
			const int32 slot = ped->GiveWeapon(type, 9999, true);
			ped->SetCurrentWeapon(slot);
		}
		r.weapon = type;
	}
	const bool shown = (s.flags & PF_WEAPON_SHOWN) != 0 && !r.dead;
	if (!shown && ped->m_pWeaponModel)
		ped->RemoveWeaponModel(-1);
	else if (shown && ped->m_pWeaponModel == nil && type != WEAPONTYPE_UNARMED && m1 >= 0 && CStreaming::HasModelLoaded(m1))
		ped->AddWeaponModel(m1);
}

// (dead as its owner: SetDie on foot - the combat module lets it through while bReplaying -, by hand when seated (a
// driver's SetDie deletes it); alive again: standing)
static void
ApplyDeath(Remote &r, CPed *ped, const PlayerState &s)
{
	const bool dead = (s.flags & PF_DEAD) != 0;
	if (dead) {
		if (!ped->DyingOrDead() && !ped->bInVehicle) {
			bReplaying = true;
			ped->SetDie(ANIM_STD_NUM, 4.0f, 0.0f);
			bReplaying = false;
		}
		ped->m_fHealth = 0.0f;
		r.dead = true;
		return;
	}
	if (r.dead || ped->DyingOrDead()) {
		ped->SetPedState(ped->bInVehicle ? PED_DRIVING : PED_IDLE);
		ped->m_nLastPedState = PED_NONE;
		ped->bIsPedDieAnimPlaying = false;
	}
	r.dead = false;
	ped->m_fHealth = Max(1.0f, (float)s.health);
}

// (the puppet as its owner is at render time)
static void
ApplyState(int32 slot, Remote &r, CPed *ped, double now)
{
	CVector pos;
	float heading;
	const PlayerState *next;
	float f;
	double since;
	const PlayerState *s = StateAt(r, now - INTERP_MS, &pos, &heading, &next, &f, &since);
	if (s == nil)
		return;
	RemotePlayer *p = Player(slot);
	const bool sameArea = s->area == CGame::currArea;
	r.visible = sameArea;

	// (the game's code may have flagged it: a vehicle deleted under it)
	if (ped->bRemoveFromWorld) {
		ped->bRemoveFromWorld = false;
		ped->bInVehicle = false;
		ped->m_pMyVehicle = nil;
		r.vehicle = nil;
		r.seat = -1;
	}

	ApplyDeath(r, ped, *s);

	// seated in the vehicle the entities module replicates, else standing at the state's place
	CVehicle *veh = nil;
	if ((s->flags & PF_SEATED) && s->seat != NO_SEAT && s->vehicle != 0) {
		CEntity *e = EntityOf(s->vehicle);
		if (e && e->IsVehicle())
			veh = (CVehicle *)e;
	}
	if (veh == nil || !Seat(r, ped, veh, s->seat)) {
		if (ped->bInVehicle || ped->m_pMyVehicle)
			Unseat(r, ped);
	}
	if (!ped->bInVehicle) {
		ped->SetPosition(pos);
		ped->m_fRotationCur = ped->m_fRotationDest = heading;
		ped->SetHeading(heading);
		ped->m_vecMoveSpeed = s->velocity / 50.0f;
		ped->GetMatrix().UpdateRW();
		ped->UpdateRwFrame();
		ped->RemoveAndAdd();
		if (!r.dead)
			ped->SetPedState(PED_IDLE);
	} else
		ped->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);

	// what the game's code would do to it, undone
	ped->m_nMoveState = (eMoveState)Clamp((int32)s->moveState, (int32)PEDMOVE_NONE, (int32)PEDMOVE_SPRINT);
	ped->m_nStoredMoveState = ped->m_nMoveState;
	ped->m_nWaitState = WAITSTATE_FALSE;
	ped->m_objective = OBJECTIVE_NONE;
	ped->bIsDucking = (s->flags & PF_DUCKING) != 0;
	ped->bIsInTheAir = (s->flags & PF_IN_AIR) != 0;
	ped->bIsLanding = (s->flags & PF_LANDING) != 0;
	ped->bIsStanding = !ped->bInVehicle && !ped->bIsInTheAir;
	ped->m_fArmour = s->armour;
	ped->bIsVisible = sameArea;
	ped->bUsesCollision = sameArea && !ped->bInVehicle && !r.dead;
	ped->bRenderPedInCar = true;
	ClearCollision(ped);

	ApplyWeapon(r, ped, *s);
	MirrorAnims(r, ped, *s, next, f, since);
	TrimBlocks(r, ped);
	r.aiming = (s->flags & PF_AIMING) != 0 && !r.dead;
	r.aimYaw = s->aimYaw;
	r.aimPitch = s->aimPitch;
	(void)p;
}

// (an other player's puppet made, kept, re-dressed or deleted)
static void
UpdatePuppet(int32 slot, double now, const CVector &here)
{
	RemotePlayer *p = Player(slot);
	Remote &r = remotes[slot];
	if (r.ped == nil && (r.special >= 0 || r.model >= 0 || r.blocks)) {
		// (deleted by the game: forgotten, made again below)
		ReleaseBlocks(r);
		ForgetPuppet(slot, r);
	}
	const PlayerState *last = Latest(r);
	if (p == nil || last == nil)
		return;
	if (r.ped && Find(r.ped) == nil) { // (its registry entry lost: given again)
		NetEntity *ne = Register(r.ped, PlayerNetId(slot), slot, true, KIND_PED);
		if (ne)
			ne->player = (int8)slot;
		p->puppet = r.ped;
	}
	const float distance = (last->pos - here).Magnitude();
	if (r.ped && distance > DELETE_RANGE) {
		DeletePuppet(slot, r);
		DropPending(slot);
		return;
	}
	Skin skin;
	ResolveSkin(p->skin, &skin);
	if (r.ped && SameName(r.skin, skin.name)) {
		DropPending(slot);
		ApplyState(slot, r, r.ped, now);
		return;
	}
	if (r.ped == nil && distance > CREATE_RANGE)
		return;
	// (a new puppet, or new clothes: the model first, then the puppet made - the old one kept until then)
	if (!SameName(pendingSkin[slot].name, skin.name)) {
		DropPending(slot);
		CopyName(pendingSkin[slot].name, skin.name, sizeof(pendingSkin[slot].name));
	}
	int32 model = -1;
	if (!SkinReady(skin, &pendingSkin[slot].special, &model)) {
		if (r.ped)
			ApplyState(slot, r, r.ped, now);
		return;
	}
	CVector pos;
	float heading;
	const PlayerState *next;
	float f;
	double since;
	if (StateAt(r, now - INTERP_MS, &pos, &heading, &next, &f, &since) == nil)
		return;
	if (r.ped)
		DeletePuppet(slot, r);
	const int32 special = pendingSkin[slot].special;
	pendingSkin[slot].special = -1; // (the puppet's now)
	pendingSkin[slot].name[0] = '\0';
	CPed *ped = CreatePuppet(slot, r, skin, model, special, pos, heading);
	if (ped == nil) {
		ReleaseSpecial(special);
		return;
	}
	ApplyState(slot, r, ped, now);
}

// ---------------------------------------------------------------- the test commands' controls
static void
ApplyTestControls(double now)
{
	CPlayerPed *me = FindPlayerPed();
	if (me == nil)
		return;
	CPad *pad = CPad::GetPad(0);
	if (CTimer::GetTimeInMilliseconds() < test.walkUntil) {
		pad->NewState.LeftStickY = -128;
		if (test.sprint)
			pad->NewState.Cross = 255;
	}
	if (test.jumpFrames > 0) {
		test.jumpFrames--;
		pad->NewState.Square = 255;
	}
	if (test.crouchFrames > 0) {
		test.crouchFrames--;
		pad->NewState.LeftShock = 255;
	}
	if (now < test.aimUntil) {
		RemotePlayer *p = Player(test.aimSlot);
		if (p == nil) {
			test.aimUntil = 0.0;
			return;
		}
		// (a gun in hand: its aim held, as the target button does)
		const eWeaponType weapon = me->GetWeapon()->m_eWeaponType;
		if (weapon >= WEAPONTYPE_COLT45 && weapon <= WEAPONTYPE_MINIGUN)
			pad->NewState.RightShoulder1 = 255;
		const CVector target = p->puppet ? p->puppet->GetPosition() : p->pos;
		const CVector d = target - me->GetPosition();
		if (d.Magnitude2D() < 0.1f)
			return;
		const float heading = Atan2(-d.x, d.y);
		me->m_fRotationCur = me->m_fRotationDest = heading;
		me->SetHeading(heading);
		me->m_fLookDirection = heading;
		// (the follow camera's Front = (-cos(Beta) cos(Alpha), -sin(Beta) cos(Alpha), sin(Alpha)): looking at the target)
		CCam &cam = TheCamera.Cams[TheCamera.ActiveCam];
		cam.Beta = Atan2(-d.y, -d.x);
		cam.Alpha = Clampf(Atan2(d.z, d.Magnitude2D()), -DEGTORAD(89.0f), DEGTORAD(44.0f));
	}
}

// ---------------------------------------------------------------- the module's handlers
void
Players_Joined(int32 slot)
{
	Ready();
	if (slot < 0 || slot >= MAX_SLOTS)
		return;
	RemotePlayer *p = Player(slot);
	Remote &r = remotes[slot];
	if (r.used && p && r.rank == p->rank)
		return;
	// (the slot was someone else's: its puppet goes)
	if (r.ped)
		DeletePuppet(slot, r);
	else {
		ReleaseBlocks(r);
		ForgetPuppet(slot, r);
	}
	DropPending(slot);
	ClearRecord(r);
	r.used = true;
	r.rank = p ? p->rank : 0;
}

void
Players_Left(int32 slot)
{
	Ready();
	if (slot < 0 || slot >= MAX_SLOTS)
		return;
	Remote &r = remotes[slot];
	if (r.ped)
		DeletePuppet(slot, r);
	else {
		ReleaseBlocks(r);
		ForgetPuppet(slot, r);
	}
	DropPending(slot);
	ClearRecord(r);
	if (test.aimSlot == slot)
		test.aimUntil = 0.0;
}

void
Players_Info(int32 slot)
{
	// (a new skin: the next frames dress the puppet in it - UpdatePuppet compares)
	(void)slot;
}

void
Players_Away(int32 slot)
{
	(void)slot; // (shown by the tag; the puppet stays where its owner left it)
}

void
Players_Reset(void)
{
	Ready();
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		if (r.ped)
			DeletePuppet(i, r);
		else {
			ReleaseBlocks(r);
			ForgetPuppet(i, r);
		}
		DropPending(i);
		ClearRecord(r);
	}
	test.aimUntil = 0.0;
	test.walkUntil = 0;
	test.jumpFrames = test.crouchFrames = 0;
}

void
Players_GameLoaded(void)
{
	Ready();
	// (a world just loaded: the puppets of an earlier one are gone with it, the special slots' names may be the save's)
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		r.ped = nil;
		r.vehicle = nil;
		r.blocks = 0;
		r.special = r.model = -1;
		r.skin[0] = '\0';
		r.weapon = -1;
		r.seat = -1;
		pendingSkin[i].special = -1;
		pendingSkin[i].name[0] = '\0';
		if (RemotePlayer *p = Player(i))
			p->puppet = nil;
	}
	memset(specials, 0, sizeof(specials));
	memset(&local, 0, sizeof(local));
	local.previousArea = CGame::currArea;
	CopyName(local.skinSeen, Me().skin, sizeof(local.skinSeen));
}

void
Players_PreWorld(void)
{
	Ready();
	if (Mode() != MODE_ACTIVE)
		return;
	CPlayerPed *me = FindPlayerPed();
	if (me == nil)
		return;
	const double now = Now();
	ApplyTestControls(now);
	const CVector here = FindPlayerCoors();
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		RemotePlayer *p = Player(i);
		if (p == nil) {
			if (r.used || r.ped)
				Players_Left(i);
			continue;
		}
		if (!r.used) {
			ClearRecord(r);
			r.used = true;
			r.rank = p->rank;
		}
		UpdatePuppet(i, now, here);
	}
}

void
Players_PostWorld(void)
{
	Ready();
	if (Mode() != MODE_ACTIVE)
		return;
	const double now = Now();
	SendLocal(now);
	WatchLocalSkin(now);
	// (after the animations' update: the arms aimed, the footsteps heard)
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		CPed *ped = r.ped;
		if (ped == nil || ped->GetClump() == nil)
			continue;
		if (r.aiming && ped->bIsVisible) {
			CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType);
			if (info->IsFlagSet(WEAPONFLAG_CANAIM_WITHARM) && !ped->bIsDucking)
				ped->m_pedIK.m_flags |= CPedIK::AIMS_WITH_ARM;
			else
				ped->m_pedIK.m_flags &= ~CPedIK::AIMS_WITH_ARM;
			ped->bIsAimingGun = true;
			ped->m_fLookDirection = r.aimYaw;
			ped->m_pedIK.PointGunInDirection(r.aimYaw, r.aimPitch);
			r.restoringGun = true;
		} else if (r.restoringGun) {
			ped->bIsAimingGun = false;
			ped->m_pedIK.m_flags &= ~CPedIK::AIMS_WITH_ARM;
			if (ped->m_pedIK.RestoreGunPosn())
				r.restoringGun = false;
		}
		if (ped->bIsVisible && ped->bIsStanding && !r.dead)
			ped->PlayFootSteps();
	}
}

bool
Players_PuppetControl(CPed *ped, NetEntity *ne)
{
	// (the game's ProcessControl of a remote player's puppet: none of the game's own; what PreWorld set stays)
	(void)ne;
	ClearCollision(ped);
	return true;
}

void
Players_EntityDestroyed(CEntity *e, NetEntity *ne)
{
	Ready();
	// (deleted by the game anyway: forgotten, made again with the next states)
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		if (r.ped != (CPed *)e)
			continue;
		r.ped = nil; // (its reference left for ~CEntity, which frees it)
		ReleaseBlocks(r);
		ForgetPuppet(i, r);
	}
	(void)ne;
}

// ---------------------------------------------------------------- the name tags
void
Players_Render2d(void)
{
	Ready();
	if (Mode() != MODE_ACTIVE || !CHud::m_Wants_To_Draw_Hud || TheCamera.m_WideScreenOn) {
		for (int32 i = 0; i < MAX_SLOTS; i++)
			remotes[i].tagDrawn = false;
		return;
	}
	const CVector cam = TheCamera.GetPosition();
	bool started = false;
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		Remote &r = remotes[i];
		RemotePlayer *p = Player(i);
		CPed *ped = r.ped;
		r.tagDrawn = false;
		if (p == nil || ped == nil || !ped->bIsVisible || ped->GetClump() == nil)
			continue;
		const CVector head = ped->GetNodePosition(PED_HEAD) + CVector(0.0f, 0.0f, 0.3f);
		const float dist = (head - cam).Magnitude();
		if (dist > TAG_RANGE || !CWorld::GetIsLineOfSightClear(cam, head, true, false, false, false, false, true))
			continue;
		RwV3d screen;
		float w, h;
		if (!CSprite::CalcScreenCoors(head, &screen, &w, &h, true))
			continue;
		if (!started) {
			started = true;
			CFont::DrawFonts();
			CFont::SetBackgroundOff();
			CFont::SetBackGroundOnlyTextOff();
			CFont::SetPropOn();
			CFont::SetJustifyOff();
			CFont::SetRightJustifyOff();
			CFont::SetCentreOn();
			CFont::SetCentreSize(SCREEN_WIDTH);
			CFont::SetWrapx(SCREEN_WIDTH);
			CFont::SetAlphaFade(255.0f);
			CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
			CFont::SetDropShadowPosition(1);
		}
		const float k = Clampf(1.3f - dist / 50.0f, 0.55f, 1.0f);
		const float sx = 0.5f * k, sy = 1.05f * k;
		const uint8 alpha = (uint8)(dist > TAG_FADE ? 255.0f * Clampf(1.0f - (dist - TAG_FADE) / (TAG_RANGE - TAG_FADE), 0.0f, 1.0f) : 255.0f);
		const float row = 18.0f * SCREEN_SCALE_Y(sy);
		const int32 stars = Min((int32)p->wanted, 6);
		float y = screen.y - row * (stars > 0 ? 2.0f : 1.0f);
		char text[64];
		snprintf(text, sizeof(text), "%s%s", p->name, p->away ? (LangFrench() ? " (absent)" : " (away)") : "");
		for (char *c = text; *c; c++)
			if (*c == '~' || (uint8)*c < 0x20)
				*c = ' ';
		wchar line[64];
		AsciiToUnicode(text[0] == '*' ? text + 1 : text, line);
		CFont::SetScale(SCREEN_SCALE_X(sx), SCREEN_SCALE_Y(sy));
		CFont::SetDropColor(CRGBA(0, 0, 0, alpha));
		CFont::SetColor(p->away ? CRGBA(170, 170, 170, alpha) : CRGBA(255, 255, 255, alpha));
		CFont::PrintString(screen.x, y, line);
		if (stars > 0) {
			char glyphs[8];
			for (int32 s = 0; s < stars; s++)
				glyphs[s] = '>';
			glyphs[stars] = '\0';
			AsciiToUnicode(glyphs, line);
			CFont::SetScale(SCREEN_SCALE_X(sx * 1.2f), SCREEN_SCALE_Y(sy * 1.1f));
			CFont::SetColor(CRGBA(97, 194, 247, alpha)); // (the HUD's WANTED_COLOR)
			CFont::PrintString(screen.x, y + row, line);
		}
		r.tagDrawn = true;
	}
	if (started) {
		CFont::DrawFonts();
		CFont::SetDropShadowPosition(0);
		CFont::SetCentreOff();
	}
}

// ---------------------------------------------------------------- the test commands (?mmotest)
bool
Players_TestCommand(const char *cmd)
{
	const double now = Now();
	int32 slot;
	float seconds;
	// (aiming at a player and leaving the area are an administrator's only: an aimbot, an invisible player otherwise)
	if (sscanf(cmd, "aim %d", &slot) == 1) {
		// aim <slot>: the local player (and the camera) turned toward that player for 3 s, a gun's aim held
		test.aimSlot = slot;
		test.aimUntil = Player(slot) && (IsAdmin() || Mode() != MODE_ACTIVE) ? now + 3000.0 : 0.0;
		return true;
	}
	if (sscanf(cmd, "area %d", &slot) == 1) {
		// area <n>: the local player's area set (an interior's number, 0 the main map) as entering one does to it
		if (IsAdmin() && Mode() == MODE_ACTIVE && slot >= 0 && slot < AREA_EVERYWHERE)
			CGame::currArea = slot;
		return true;
	}
	if (sscanf(cmd, "walk %f", &seconds) == 1 || strcmp(cmd, "walk") == 0) {
		// walk [seconds]: forward held for that long in the game's time (1 s by default)
		test.walkUntil = CTimer::GetTimeInMilliseconds() + (uint32)(1000.0f * (strcmp(cmd, "walk") == 0 ? 1.0f : Clampf(seconds, 0.0f, 60.0f)));
		test.sprint = false;
		return true;
	}
	if (sscanf(cmd, "sprint %f", &seconds) == 1) {
		test.walkUntil = CTimer::GetTimeInMilliseconds() + (uint32)(1000.0f * Clampf(seconds, 0.0f, 60.0f));
		test.sprint = true;
		return true;
	}
	if (strcmp(cmd, "jump") == 0) {
		test.jumpFrames = 1;
		return true;
	}
	if (strcmp(cmd, "crouch") == 0) {
		test.crouchFrames = 1;
		return true;
	}
	if (strcmp(cmd, "stop") == 0) {
		test.walkUntil = 0;
		test.aimUntil = 0.0;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- the debug state (?mmotest)
static void
DebugName(const char *s)
{
	char out[48];
	int32 n = 0;
	out[n++] = '"';
	for (const uint8 *c = (const uint8 *)s; *c && n < (int32)sizeof(out) - 3; c++)
		if (*c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\')
			out[n++] = *c;
	out[n++] = '"';
	out[n] = '\0';
	DebugAppend("%s", out);
}

void
Players_Debug(void)
{
	Ready();
	CPlayerPed *me = Mode() == MODE_ACTIVE ? FindPlayerPed() : nil;
	const CVector here = me ? FindPlayerCoors() : CVector(0.0f, 0.0f, 0.0f);
	const double now = Now();
	// (the core's "players" given again with the puppets': JSON's last key wins)
	DebugAppend(",\"players\":[");
	for (int32 i = 0, k = 0; i < MAX_SLOTS; i++) {
		const RemotePlayer *p = Player(i);
		if (p == nil)
			continue;
		const Remote &r = remotes[i];
		const PlayerState *s = Latest(r);
		CPed *ped = r.ped;
		const CVector statePos = p->pos;
		const float stateDist = me && p->hasState ? (statePos - here).Magnitude() : -1.0f;
		// (pos: where this client shows the player - its puppet; with none: where it is (out of range, no state yet),
		// or null when a puppet should be there)
		const bool expected = me && p->hasState && stateDist >= 0.0f && stateDist < CREATE_RANGE;
		DebugAppend("%s{\"slot\":%d,\"name\":", k++ ? "," : "", i);
		DebugName(p->name);
		DebugAppend(",\"skin\":");
		DebugName(p->skin);
		DebugAppend(",\"rank\":%u,\"admin\":%s,\"away\":%s,\"hasState\":%s,\"dist\":%.1f,\"area\":%d,\"stars\":%d",
		            p->rank, p->admin ? "true" : "false", p->away ? "true" : "false", p->hasState ? "true" : "false",
		            stateDist, p->area, p->wanted);
		if (ped)
			DebugAppend(",\"pos\":[%.2f,%.2f,%.2f]", ped->GetPosition().x, ped->GetPosition().y, ped->GetPosition().z);
		else if (expected)
			DebugAppend(",\"pos\":null");
		else
			DebugAppend(",\"pos\":[%.2f,%.2f,%.2f]", statePos.x, statePos.y, statePos.z);
		DebugAppend(",\"statePos\":[%.2f,%.2f,%.2f],\"puppet\":%s", statePos.x, statePos.y, statePos.z, ped ? "true" : "false");
		if (ped) {
			const CAnimBlendAssociation *main = nil;
			if (ped->GetClump())
				for (CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(ped->GetClump()); a; a = RpAnimBlendGetNextAssociation(a))
					if (main == nil || a->blendAmount > main->blendAmount)
						main = a;
			DebugAppend(",\"gap\":%.2f,\"model\":%d,\"special\":%d,\"visible\":%s,\"tag\":%s,\"seated\":%u,\"seat\":%d,"
			            "\"dead\":%s,\"weapon\":%d,\"weaponShown\":%s,\"aiming\":%s,\"moveState\":%d,\"pedState\":%d,\"anim\":[%d,%d,%.2f],"
			            "\"anims\":%d,\"blocks\":%u",
			            (ped->GetPosition() - statePos).Magnitude(), r.model, r.special, ped->bIsVisible ? "true" : "false",
			            r.tagDrawn ? "true" : "false", ped->bInVehicle && r.vehicle ? NetIdOf(r.vehicle) : 0u, r.seat,
			            r.dead ? "true" : "false", (int32)ped->GetWeapon()->m_eWeaponType, ped->m_pWeaponModel ? "true" : "false",
			            r.aiming ? "true" : "false", (int32)ped->m_nMoveState, (int32)ped->m_nPedState,
			            main ? main->groupId : -1, main ? main->animId : -1, main ? main->blendAmount : 0.0f,
			            s ? s->numAnims : 0, (uint32)r.blocks);
			DebugAppend(",\"puppetAnims\":[");
			int32 n = 0;
			if (ped->GetClump())
				for (CAnimBlendAssociation *a = RpAnimBlendClumpGetFirstAssociation(ped->GetClump()); a && n < 6; a = RpAnimBlendGetNextAssociation(a))
					DebugAppend("%s[%d,%d,%.2f,%.2f]", n++ ? "," : "", a->groupId, a->animId, a->currentTime, a->blendAmount);
			DebugAppend("]");
		}
		{
			Skin skin;
			ResolveSkin(p->skin, &skin);
			DebugAppend(",\"resolved\":{\"special\":%s,\"model\":%d,\"female\":%s,\"name\":", skin.special ? "true" : "false",
			            skin.model, skin.female ? "true" : "false");
			DebugName(skin.name);
			DebugAppend(",\"pending\":");
			DebugName(pendingSkin[i].name);
			DebugAppend(",\"pendingSlot\":%d}", pendingSkin[i].special);
		}
		if (s)
			DebugAppend(",\"state\":{\"life\":%d,\"move\":%d,\"pedState\":%d,\"flags\":%d,\"weapon\":%d,\"health\":%d,\"armour\":%d,"
			            "\"vehicle\":%u,\"seat\":%d,\"anims\":%d,\"full\":%s,\"count\":%d,\"received\":%u,\"age\":%.0f}",
			            s->life, s->moveState, s->pedState, s->flags, s->weapon, s->health, s->armour, s->vehicle, s->seat,
			            s->numAnims, r.full ? "true" : "false", r.count, r.states, now - r.samples[r.count - 1].t);
		DebugAppend("}");
	}
	DebugAppend("],\"playersModule\":{\"sent\":%u,\"bytes\":%u,\"lastSize\":%u,\"life\":%d,\"specials\":[", local.count, local.bytes,
	            local.lastSize, local.life);
	for (int32 i = 0, k = 0; i < NUM_SPECIAL; i++)
		if (specials[i].name[0]) {
			DebugAppend("%s{\"slot\":%d,\"name\":", k++ ? "," : "", MI_SPECIAL01 + i);
			DebugName(specials[i].name);
			DebugAppend(",\"refs\":%d}", specials[i].refs);
		}
	DebugAppend("],\"slotRefs\":[");
	for (int32 i = 0; i < NUM_SPECIAL; i++) {
		CBaseModelInfo *info = CModelInfo::GetModelInfo(MI_SPECIAL01 + i);
		DebugAppend("%s[%d,%d,%d,", i ? "," : "", info->GetNumRefs(), specials[i].refs, CStreaming::HasModelLoaded(MI_SPECIAL01 + i) ? 1 : 0);
		DebugName(info->GetModelName());
		DebugAppend("]");
	}
	DebugAppend("]");
	if (local.haveSent) {
		const PlayerState &s = local.sent;
		DebugAppend(",\"local\":{\"move\":%d,\"pedState\":%d,\"flags\":%d,\"weapon\":%d,\"vehicle\":%u,\"seat\":%d,\"anims\":[",
		            s.moveState, s.pedState, s.flags, s.weapon, s.vehicle, s.seat);
		for (int32 i = 0; i < s.numAnims; i++)
			DebugAppend("%s[%d,%d,%.2f,%.2f]", i ? "," : "", s.anims[i].group, s.anims[i].anim, s.anims[i].time, s.anims[i].blend);
		DebugAppend("]}");
	}
	DebugAppend("}");
}
}
#endif
