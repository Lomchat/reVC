// The shared world's ambient entities (docs/mmo.md, section 7 "Ownership and population"; owner's decisions 5 and 7):
// every pedestrian and vehicle of the pools is either owned here (simulated by this client, its state told to the
// remote players near it) or a puppet (another client's: kinematic, drawn 100 ms behind its owner's states, left alone
// by the game's removers). The closest player populates (MayGenerateAt), an entity the game would remove for distance
// goes to a nearer remote player instead (MigrateInsteadOfRemove), the server's OWNER records turn copies into real
// entities and back, entering another client's car claims it first, car generators are linked across clients by
// their index.
// Not shared: the script's own entities (MISSION_CHAR peds, MISSION_VEHICLE cars: every client's scripts make the
// same ones), planes and trains (the game drives them from its clock), objects. A script car the local player drives is
// shared from then on.
// Counters: a copy counts in the game's ped and car counters (the population's caps) only while it is inside the local
// population's radius (the one beyond which the game removes our own: 51 m x the creation multiplier for peds, 120 m
// for cars, x1.5 for an extended range, both x the camera's generation multiplier). Where players overlap the density
// stays the game's for one player; a remote crowd 200 m away does not starve our own streets.
// Records (ENTITY_STATE, after the core's 18-byte header): u32 the sender's server clock (ms, low 32 bits), u8 area,
// u16 model, then the ped's or the vehicle's body (WritePed, WriteVehicle). ENTITY_GONE reasons: GONE_* below.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmo_internal.h"
#include "AnimBlendAssociation.h"
#include "AnimManager.h"
#include "Automobile.h"
#include "Bike.h"
#include "Boat.h"
#include "Camera.h"
#include "CarCtrl.h"
#include "CarGen.h"
#include "CivilianPed.h"
#include "CopPed.h"
#include "EmergencyPed.h"
#include "Game.h"
#include "General.h"
#include "Heli.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Ped.h"
#include "PedModelInfo.h"
#include "PedType.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Pools.h"
#include "Population.h"
#include "RpAnimBlend.h"
#include "Streaming.h"
#include "SurfaceTable.h"
#include "Timer.h"
#include "Vehicle.h"
#include "VehicleModelInfo.h"
#include "VisibilityPlugins.h"
#include "Wanted.h"
#include "WeaponInfo.h"
#include "World.h"

namespace OrthrosMmo
{
// ---------------------------------------------------------------- tuning
static const float SEND_RANGE = 250.0f;     // owned entities told to a remote player this near (same area)
static const float CREATE_RANGE = 300.0f;   // a copy made of a state this near the local player
static const float DELETE_RANGE = 330.0f;   // a copy beyond this is deleted here (it comes back with a state)
static const float GEN_RANGE = 300.0f;      // remote players who may populate instead of us
static const float KEEP_RANGE = 120.0f;     // an off-screen car or a wreck kept for a remote player this near
static const double DELAY = 100.0;          // copies shown this many ms behind their states
static const double EXTRAPOLATE = 250.0;    // at most this far ahead of the last state
static const double SILENCE = 15000.0;      // a copy without states this long is deleted
static const double ORPHAN = 3000.0;        // a copy whose owner left, without an OWNER record, this long
static const double OWNER_FAR = 3000.0;     // a copy without states this long while its owner's player is far from it
static const double PENDING_LIFE = 5000.0;  // a state waiting for its model or vehicle kept this long
static const double CLAIM_RETRY = 1000.0;   // the local player's car claimed again while the server says another's
static const double CAR_WAIT = 30000.0;     // the "car" test command's model waited for this long
static const double WRECK_WAIT = 300.0;     // a wrecked state waits this long for the owner's explosion event
static const double WRECK_RECENT = 3000.0;  // a wreck told by a state younger than this blows the copy up

enum { MAX_SAMPLES = 6, MAX_PENDING = 96, MAX_PASSENGERS = 8, MAX_GEN_SPAWNS = 8, MAX_CREATE_PER_FRAME = 6, MAX_IDS = 96 };
// (pool room: a copy is made only while the local population's quota still to come plus this margin stays free (its
// drivers, passengers, cops, roadblocks and the script's), and copies take at most half a pool; below RELIEF free
// slots the farthest uncounted copy is deleted, before the game's own pool-full removers (8) delete ours)
enum { PED_MARGIN = 40, VEHICLE_MARGIN = 30, POOL_RELIEF = 20 };
// ENTITY_GONE reasons
enum { GONE_REMOVED = 0, GONE_NOT_HERE = 1, GONE_DRIVER_LEFT = 2, GONE_RESET = 3 };
// vehicle state flags (u16)
enum {
	VF_HANDBRAKE = 1 << 0, VF_ENGINE = 1 << 1, VF_SIREN = 1 << 2, VF_HORN = 1 << 3, VF_TAXILIGHT = 1 << 4,
	VF_POLICE = 1 << 5, VF_FADE = 1 << 6, VF_STANDING = 1 << 7, VF_SCORCHED = 1 << 8, VF_EXTENDED = 1 << 9,
	VF_LAWENFORCER = 1 << 10, VF_OWNEDBYPLAYER = 1 << 11,
};
// ped state flags (u8)
enum { PF_DUCKING = 1, PF_FADE = 2, PF_INAIR = 4 };
// fixed-point scales: speeds in metres per 1/50 s, turn speeds in radians per 1/50 s
static const float VEL_SCALE = 1024.0f;
static const float TURN_SCALE = 8192.0f;
static const float EXTRA_SCALE = 10000.0f;

// ---------------------------------------------------------------- data
struct Quat { float x, y, z, w; };

// (a state record, read: the core's 18-byte header, then the kind's body)
struct State {
	uint32 netid;
	uint8 kind, flags;
	CVector pos;
	uint32 ts;          // the sender's server clock (ms, low 32 bits)
	double at;          // its time on our clock
	uint8 area;
	uint16 model;
	CVector vel;
	// peds
	uint8 pedType, copType, moveState, pedState, health, weapon, seat, pflags;
	float heading;
	uint32 vehicle;
	// vehicles
	uint8 createdBy, status, colour1, colour2, gear, doorLock, engine, wheels, lights, wheelStates;
	int8 extra0, extra1;
	uint16 seed, vflags, vhealth, doors, panels;
	Quat q;
	CVector turn;
	float steer, gas, brake;
	uint8 susp[4];
	int16 e1, e2, cargen;
	uint32 driver;
	uint8 numPassengers;
	uint8 seats[MAX_PASSENGERS];
	uint32 passengers[MAX_PASSENGERS];
};

struct Sample {
	double t;
	CVector pos, vel, turn;
	Quat q;
	float heading;
};

// (this module's side of a networked entity, by pool slot: NetEntity.data)
struct EntData {
	uint32 netid;
	int8 area;
	int16 cargen;          // the car generator that made this parked car (-1)
	// owned
	bool announced;        // a state was sent for it: the server knows it, its deletion is told (ENTITY_GONE)
	double nextSend, lastSent;
	uint8 sentFlags, sentSeat, sentPedState, sentHealth;
	uint32 sentVehicle, sentDamage;
	bool claiming, giving;
	double claimAt;
	// puppet
	bool counted;          // in the game's ped/car counters (only inside the local population's radius)
	bool hidden;           // a passenger whose vehicle is not here
	bool rebuild;          // made again from its next state (revived, model changed)
	double lastState, ownerMissing;
	double deadAt;         // since when the copy is dying or dead here
	double wreckSeen;      // since when its states say wrecked while the copy is not yet
	Sample samples[MAX_SAMPLES];
	int32 numSamples;
	State st;
	bool hasState;
};
static EntData pedData[NUMPEDS], vehData[NUMVEHICLES];

// (states of entities not here yet: their model loading, their vehicle not made, or their owner just made us theirs)
struct Pending {
	bool used, owned;
	int32 from;
	double at;
	State st;
};
static Pending pending[MAX_PENDING];

// (car generators that passed MayGenerateParked this frame: the parked car they make is theirs)
static struct { int32 index; CVector pos; } genSpawns[MAX_GEN_SPAWNS];
static int32 numGenSpawns;

// ("car <model>" test command)
static int32 carModel = -1;
static double carAt;

// (counters for the debug state)
static struct {
	uint32 sent, received, created, deleted, gone, migrations, received_owner, claims, claimsRefused, keeps,
		refusedSpawns, parkedLinked, orphaned, notHere, rebuilt, relieved, evicted;
} stats;

// ---------------------------------------------------------------- small helpers
// (a shared world being played: the world loaded, a slot and a tag from the server)
static bool
Live(void)
{
	return Mode() == MODE_ACTIVE && LocalSlot() != NO_SLOT && LocalTag() != 0;
}

static inline float
Dist2D(const CVector &a, const CVector &b)
{
	return (CVector2D(a) - CVector2D(b)).Magnitude();
}

static inline float
Clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline int32
Fixed16(float v, float scale)
{
	float f = v * scale;
	if (!(f == f))
		return 0;
	return (int32)Clampf(f, -32767.0f, 32767.0f);
}

static float
WrapAngle(float a)
{
	a = fmodf(a, TWOPI);
	if (a < 0.0f)
		a += TWOPI;
	return a;
}

static float
LerpAngle(float a, float b, float f)
{
	float d = WrapAngle(b - a);
	if (d > PI)
		d -= TWOPI;
	return a + d * f;
}

static Quat
Normalised(Quat q)
{
	float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (!(n > 1e-6f)) {
		Quat id = { 0.0f, 0.0f, 0.0f, 1.0f };
		return id;
	}
	q.x /= n; q.y /= n; q.z /= n; q.w /= n;
	return q;
}

// (a rotation as a quaternion: the matrix's columns are right, forward, up)
static Quat
QuatOf(const CMatrix &m)
{
	const CVector &r = m.GetRight(), &f = m.GetForward(), &u = m.GetUp();
	const float m00 = r.x, m10 = r.y, m20 = r.z, m01 = f.x, m11 = f.y, m21 = f.z, m02 = u.x, m12 = u.y, m22 = u.z;
	Quat q;
	const float tr = m00 + m11 + m22;
	if (tr > 0.0f) {
		float s = sqrtf(tr + 1.0f) * 2.0f;
		q.w = 0.25f * s; q.x = (m21 - m12) / s; q.y = (m02 - m20) / s; q.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s; q.x = 0.25f * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25f * s; q.z = (m12 + m21) / s;
	} else {
		float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25f * s;
	}
	return Normalised(q);
}

static void
SetRotation(CMatrix &m, const Quat &q)
{
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	m.GetRight() = CVector(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy));
	m.GetForward() = CVector(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx));
	m.GetUp() = CVector(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy));
}

static Quat
Nlerp(const Quat &a, Quat b, float f)
{
	if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
		b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
	}
	Quat q = { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f };
	return Normalised(q);
}

static uint32
ServerClock32(void)
{
	const double now = ServerNow();
	return now > 0.0 ? (uint32)(uint64_t)now : 0;
}

// (a state's time on our clock, from the sender's server clock: the network's jitter left out)
static double
SampleTime(uint32 ts)
{
	const double now = Now();
	const int32 age = (int32)(ServerClock32() - ts);
	if (ts == 0 || age < -200 || age > 5000)
		return now;
	const double t = now - (age > 0 ? age : 0);
	return t;
}

static EntData *
DataFor(const CEntity *e)
{
	if (e == nil)
		return nil;
	if (e->GetType() == ENTITY_TYPE_PED) {
		const int32 i = CPools::GetPedPool()->GetJustIndex_NoFreeAssert((CPed *)e);
		return i >= 0 && i < NUMPEDS ? &pedData[i] : nil;
	}
	if (e->GetType() == ENTITY_TYPE_VEHICLE) {
		const int32 i = CPools::GetVehiclePool()->GetJustIndex_NoFreeAssert((CVehicle *)e);
		return i >= 0 && i < NUMVEHICLES ? &vehData[i] : nil;
	}
	return nil;
}

// (this module's data of a registry entry: nil for the players module's entries)
static EntData *
Data(NetEntity *ne)
{
	if (ne == nil || ne->player >= 0 || ne->data == nil)
		return nil;
	return (EntData *)ne->data;
}

static void
InitData(EntData *d, uint32 netid)
{
	memset(d, 0, sizeof(*d));
	d->netid = netid;
	d->cargen = -1;
	d->area = (int8)CGame::currArea;
	d->counted = true; // (an entity in the pools is in the game's counters)
	d->lastState = Now();
}

static int32
WantedLevel(void)
{
	CPlayerPed *player = FindPlayerPed();
	return player && player->m_pWanted ? player->m_pWanted->GetWantedLevel() : 0;
}

static bool
IsLaw(CEntity *e)
{
	if (WantedLevel() <= 0)
		return false;
	if (e->IsPed())
		return ((CPed *)e)->m_nPedType == PEDTYPE_COP;
	if (e->IsVehicle()) {
		CVehicle *veh = (CVehicle *)e;
		return veh->bIsLawEnforcer || veh->IsHeli() || veh->bCreatedAsPoliceVehicle || veh->IsLawEnforcementVehicle();
	}
	return false;
}

static bool
IsMission(CEntity *e)
{
	if (e->IsPed())
		return ((CPed *)e)->CharCreatedBy == MISSION_CHAR;
	if (e->IsVehicle())
		return ((CVehicle *)e)->VehicleCreatedBy == MISSION_VEHICLE;
	return false;
}

static bool
IsDead(CEntity *e)
{
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		return ped->DyingOrDead() || ped->m_fHealth <= 0.0f;
	}
	if (e->IsVehicle())
		return e->GetStatus() == STATUS_WRECKED;
	return false;
}

static bool
DrivenByLocalPlayer(CEntity *e)
{
	CPlayerPed *player = FindPlayerPed();
	return e->IsVehicle() && player && player->InVehicle() && player->m_pMyVehicle == e;
}

// (the vehicle the local player is getting into, the one it sits in)
static bool
LocalPlayerEntering(CEntity *e)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || !e->IsVehicle())
		return false;
	if (player->m_pMyVehicle == e && (player->EnteringCar() || player->bInVehicle))
		return true;
	return (player->m_objective == OBJECTIVE_ENTER_CAR_AS_DRIVER || player->m_objective == OBJECTIVE_ENTER_CAR_AS_PASSENGER) &&
	       player->m_carInObjective == e;
}

static uint8
KindOfVehicle(CVehicle *veh)
{
	switch (veh->m_vehType) {
	case VEHICLE_TYPE_CAR: return KIND_AUTOMOBILE;
	case VEHICLE_TYPE_BIKE: return KIND_BIKE;
	case VEHICLE_TYPE_BOAT: return KIND_BOAT;
	case VEHICLE_TYPE_HELI: return KIND_HELI;
	}
	return 0; // (planes and trains: the game's clock drives them on every client)
}

// (the remote players that count: present, a position known)
struct Near { int32 slot; CVector pos; int32 area; bool away; uint32 rank; };
static Near nearPlayers[MAX_SLOTS];
static int32 numNear;

static void
GatherPlayers(void)
{
	numNear = 0;
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		if (i == LocalSlot())
			continue;
		RemotePlayer *p = Player(i);
		if (p == nil || !p->hasState)
			continue;
		Near &n = nearPlayers[numNear++];
		n.slot = i;
		n.pos = p->pos;
		n.area = p->area;
		n.away = p->away;
		n.rank = p->rank;
	}
}

// (the nearest remote player to pos in that area, 2D or 3D, not away when asked; -1 if none within range)
static int32
NearestPlayer(const CVector &pos, int32 area, float range, bool notAway, bool flat, float *distance)
{
	int32 best = -1;
	float bestDist = range;
	uint32 bestRank = 0;
	for (int32 i = 0; i < numNear; i++) {
		const Near &n = nearPlayers[i];
		if (n.area != area || (notAway && n.away))
			continue;
		const float d = flat ? Dist2D(n.pos, pos) : (n.pos - pos).Magnitude();
		if (d > range)
			continue;
		if (best < 0 || d < bestDist || (d == bestDist && n.rank < bestRank)) {
			best = n.slot;
			bestDist = d;
			bestRank = n.rank;
		}
	}
	if (distance)
		*distance = bestDist;
	return best;
}

// ---------------------------------------------------------------- records
static void
SendOwn(uint32 netid, int32 op, int32 to)
{
	Out o(C_OWN);
	o.U32(netid);
	o.U8(op);
	o.U8(to);
}

static void
SendGone(uint32 netid, int32 reason)
{
	Out o(R_ENTITY_GONE);
	o.U32(netid);
	o.U8(reason);
	stats.gone++;
}

// (the core's header: u32 netid, u8 kind, u8 flags, f32 x, y, z; then u32 server clock, u8 area, u16 model)
static void
WriteHead(Out &o, uint32 netid, uint8 kind, uint8 flags, const CVector &pos, int32 area, int32 model)
{
	o.U32(netid);
	o.U8(kind);
	o.U8(flags);
	o.Vec(pos);
	o.U32(ServerClock32());
	o.U8(area);
	o.U16(model);
}

static void
WriteVec16(Out &o, const CVector &v, float scale)
{
	o.I16(Fixed16(v.x, scale));
	o.I16(Fixed16(v.y, scale));
	o.I16(Fixed16(v.z, scale));
}

static CVector
ReadVec16(In &in, float scale)
{
	const float x = in.I16() / scale, y = in.I16() / scale, z = in.I16() / scale;
	return CVector(x, y, z);
}

// (ped body: u8 pedType, u8 copType, u16 heading, i16 x3 speed, u8 moveState, u8 pedState, u8 health, u8 weapon,
// u32 vehicle netid, u8 seat (0 driver, n passenger n-1), u8 flags)
static void
WritePed(Out &o, CPed *ped, uint32 netid, uint8 flags, int32 area, uint32 vehicle, int32 seat)
{
	WriteHead(o, netid, KIND_PED, flags, ped->GetPosition(), area, ped->GetModelIndex());
	o.U8(ped->m_nPedType);
	o.U8(ped->m_nPedType == PEDTYPE_COP ? ((CCopPed *)ped)->m_nCopType : 0);
	o.U16((uint32)(WrapAngle(ped->m_fRotationCur) * (65536.0f / TWOPI)) & 0xFFFF);
	WriteVec16(o, ped->m_vecMoveSpeed, VEL_SCALE);
	o.U8(ped->m_nMoveState);
	o.U8(ped->m_nPedState);
	o.U8((uint32)Clampf(ped->m_fHealth, 0.0f, 255.0f));
	o.U8(ped->GetWeapon()->m_eWeaponType);
	o.U32(vehicle);
	o.U8(seat);
	o.U8((ped->bIsDucking ? PF_DUCKING : 0) | (ped->bFadeOut ? PF_FADE : 0) | (ped->bIsInTheAir ? PF_INAIR : 0));
}

// (the damage manager's state in 7 bytes: engine, wheels 4x2 bits, doors 6x2, panels 7x2, lights 4x2)
static void
DamageOf(CVehicle *veh, uint8 *engine, uint8 *wheels, uint16 *doors, uint16 *panels, uint8 *lights)
{
	*engine = 0; *wheels = 0; *doors = 0; *panels = 0; *lights = 0;
	if (veh->IsCar()) {
		CDamageManager &dm = ((CAutomobile *)veh)->Damage;
		*engine = dm.GetEngineStatus();
		for (int32 i = 0; i < 4; i++)
			*wheels |= (dm.GetWheelStatus(i) & 3) << (i * 2);
		for (int32 i = 0; i < 6; i++)
			*doors |= (dm.GetDoorStatus(i) & 3) << (i * 2);
		for (int32 i = 0; i < 7; i++)
			*panels |= (dm.GetPanelStatus(i) & 3) << (i * 2);
		for (int32 i = 0; i < 4; i++)
			*lights |= (dm.GetLightStatus((eLights)i) & 3) << (i * 2);
	} else if (veh->IsBike()) {
		CBike *bike = (CBike *)veh;
		*wheels = (bike->m_wheelStatus[0] & 3) | (bike->m_wheelStatus[1] & 3) << 2;
	}
}

// (what a change of the damage shows: a new state at once)
static uint32
DamageSig(uint8 engine, uint8 wheels, uint16 doors, uint16 panels, uint8 lights)
{
	return ((uint32)wheels | (uint32)doors << 8 | (uint32)panels << 18) ^ ((uint32)lights << 4 | (uint32)(engine / 25) << 28);
}

// (vehicle body: u8 createdBy, u8 status, u8 colour1, colour2, i8 extras x2, u16 random seed, i16 x4 orientation,
// i16 x3 speed, i16 x3 turn speed, i8 steer, i8 gas, u8 brake, u16 flags, u8 gear, u16 health, u8 door lock,
// damage 7 bytes, u8 x4 suspension, u8 wheel states, i16 x2 type extras, i16 car generator, u32 driver, u8 n,
// n x { u8 seat, u32 netid })
static void
WriteVehicle(Out &o, CVehicle *veh, uint32 netid, uint8 kind, uint8 flags, int32 area, int32 cargen, uint32 *damageSig)
{
	WriteHead(o, netid, kind, flags, veh->GetPosition(), area, veh->GetModelIndex());
	o.U8(veh->VehicleCreatedBy);
	o.U8(veh->GetStatus());
	o.U8(veh->m_currentColour1);
	o.U8(veh->m_currentColour2);
	o.U8((uint8)veh->m_aExtras[0]);
	o.U8((uint8)veh->m_aExtras[1]);
	o.U16(veh->m_randomSeed);
	const Quat q = QuatOf(veh->GetMatrix());
	o.I16(Fixed16(q.x, 32767.0f));
	o.I16(Fixed16(q.y, 32767.0f));
	o.I16(Fixed16(q.z, 32767.0f));
	o.I16(Fixed16(q.w, 32767.0f));
	WriteVec16(o, veh->m_vecMoveSpeed, VEL_SCALE);
	WriteVec16(o, veh->m_vecTurnSpeed, TURN_SCALE);
	o.U8((uint8)(int8)Clampf(veh->m_fSteerAngle * 100.0f, -127.0f, 127.0f));
	o.U8((uint8)(int8)Clampf(veh->m_fGasPedal * 127.0f, -127.0f, 127.0f));
	o.U8((uint32)Clampf(veh->m_fBrakePedal * 255.0f, 0.0f, 255.0f));
	uint32 vflags = 0;
	if (veh->bIsHandbrakeOn) vflags |= VF_HANDBRAKE;
	if (veh->bEngineOn) vflags |= VF_ENGINE;
	if (veh->m_bSirenOrAlarm) vflags |= VF_SIREN;
	if (veh->m_nCarHornTimer > 0) vflags |= VF_HORN;
	if (veh->IsCar() && ((CAutomobile *)veh)->bTaxiLight) vflags |= VF_TAXILIGHT;
	if (veh->bCreatedAsPoliceVehicle) vflags |= VF_POLICE;
	if (veh->bFadeOut) vflags |= VF_FADE;
	if (veh->IsBike() && ((CBike *)veh)->bIsStanding) vflags |= VF_STANDING;
	if (veh->bRenderScorched) vflags |= VF_SCORCHED;
	if (veh->bExtendedRange) vflags |= VF_EXTENDED;
	if (veh->bIsLawEnforcer) vflags |= VF_LAWENFORCER;
	if (veh->bHasBeenOwnedByPlayer) vflags |= VF_OWNEDBYPLAYER;
	o.U16(vflags);
	o.U8(veh->m_nCurrentGear);
	o.U16((uint32)Clampf(veh->m_fHealth, 0.0f, 65535.0f));
	o.U8(veh->m_nDoorLock);
	uint8 engine, wheels, lights;
	uint16 doors, panels;
	DamageOf(veh, &engine, &wheels, &doors, &panels, &lights);
	o.U8(engine);
	o.U8(wheels);
	o.U16(doors);
	o.U16(panels);
	o.U8(lights);
	*damageSig = DamageSig(engine, wheels, doors, panels, lights);
	uint8 wheelStates = 0;
	int16 e1 = 0, e2 = 0;
	if (veh->IsCar()) {
		CAutomobile *car = (CAutomobile *)veh;
		for (int32 i = 0; i < 4; i++)
			o.U8((uint32)(Clampf(car->m_aSuspensionSpringRatio[i], 0.0f, 1.0f) * 255.0f));
		for (int32 i = 0; i < 4; i++)
			wheelStates |= (car->m_aWheelState[i] & 3) << (i * 2);
		if (car->IsRealHeli()) {
			e1 = Fixed16(car->m_aWheelSpeed[1], EXTRA_SCALE);
		} else if (veh->GetModelIndex() == MI_RHINO || veh->GetModelIndex() == MI_FIRETRUCK) {
			e1 = Fixed16(car->m_fCarGunLR, EXTRA_SCALE);
			e2 = Fixed16(car->m_fCarGunUD, EXTRA_SCALE);
		}
	} else if (veh->IsBike()) {
		CBike *bike = (CBike *)veh;
		for (int32 i = 0; i < 4; i++)
			o.U8((uint32)(Clampf(bike->m_aSuspensionSpringRatio[i], 0.0f, 1.0f) * 255.0f));
		wheelStates = (bike->m_aWheelState[0] & 3) | (bike->m_aWheelState[1] & 3) << 2;
		e1 = Fixed16(bike->m_fLeanLRAngle, EXTRA_SCALE);
		e2 = Fixed16(bike->m_fWheelAngle, EXTRA_SCALE);
	} else {
		for (int32 i = 0; i < 4; i++)
			o.U8(255);
		if (veh->IsBoat())
			e1 = Fixed16(((CBoat *)veh)->m_fMovingSpeed, EXTRA_SCALE);
	}
	o.U8(wheelStates);
	o.I16(e1);
	o.I16(e2);
	o.I16(cargen);
	o.U32(veh->pDriver ? NetIdOf(veh->pDriver) : 0);
	int32 n = 0;
	for (int32 i = 0; i < veh->m_nNumMaxPassengers && i < MAX_PASSENGERS; i++)
		if (veh->pPassengers[i] && NetIdOf(veh->pPassengers[i]) != 0)
			n++;
	o.U8(n);
	for (int32 i = 0; i < veh->m_nNumMaxPassengers && i < MAX_PASSENGERS; i++) {
		if (veh->pPassengers[i] == nil)
			continue;
		const uint32 id = NetIdOf(veh->pPassengers[i]);
		if (id == 0)
			continue;
		o.U8(i);
		o.U32(id);
	}
}

static bool
ReadState(In &in, State &s)
{
	memset(&s, 0, sizeof(s));
	s.netid = in.U32();
	s.kind = in.U8();
	s.flags = in.U8();
	s.pos = in.Vec();
	s.ts = in.U32();
	s.area = in.U8();
	s.model = in.U16();
	if (s.kind == KIND_PED) {
		s.pedType = in.U8();
		s.copType = in.U8();
		s.heading = in.U16() * (TWOPI / 65536.0f);
		s.vel = ReadVec16(in, VEL_SCALE);
		s.moveState = in.U8();
		s.pedState = in.U8();
		s.health = in.U8();
		s.weapon = in.U8();
		s.vehicle = in.U32();
		s.seat = in.U8();
		s.pflags = in.U8();
	} else if (s.kind >= KIND_AUTOMOBILE && s.kind <= KIND_HELI) {
		s.createdBy = in.U8();
		s.status = in.U8();
		s.colour1 = in.U8();
		s.colour2 = in.U8();
		s.extra0 = (int8)(uint8)in.U8();
		s.extra1 = (int8)(uint8)in.U8();
		s.seed = in.U16();
		Quat q;
		q.x = in.I16() / 32767.0f;
		q.y = in.I16() / 32767.0f;
		q.z = in.I16() / 32767.0f;
		q.w = in.I16() / 32767.0f;
		s.q = Normalised(q);
		s.vel = ReadVec16(in, VEL_SCALE);
		s.turn = ReadVec16(in, TURN_SCALE);
		s.steer = (int8)(uint8)in.U8() / 100.0f;
		s.gas = (int8)(uint8)in.U8() / 127.0f;
		s.brake = in.U8() / 255.0f;
		s.vflags = in.U16();
		s.gear = in.U8();
		s.vhealth = in.U16();
		s.doorLock = in.U8();
		s.engine = in.U8();
		s.wheels = in.U8();
		s.doors = in.U16();
		s.panels = in.U16();
		s.lights = in.U8();
		for (int32 i = 0; i < 4; i++)
			s.susp[i] = in.U8();
		s.wheelStates = in.U8();
		s.e1 = in.I16();
		s.e2 = in.I16();
		s.cargen = in.I16();
		s.driver = in.U32();
		const int32 n = in.U8();
		for (int32 i = 0; i < n && in.Ok(); i++) {
			const uint8 seat = in.U8();
			const uint32 id = in.U32();
			if (s.numPassengers < MAX_PASSENGERS) {
				s.seats[s.numPassengers] = seat;
				s.passengers[s.numPassengers] = id;
				s.numPassengers++;
			}
		}
	} else
		return false;
	if (!in.Ok() || s.netid == 0 || IsPlayerNetId(s.netid))
		return false;
	if (!(s.pos.x == s.pos.x && s.pos.y == s.pos.y && s.pos.z == s.pos.z) || fabsf(s.pos.x) > 10000.0f ||
	    fabsf(s.pos.y) > 10000.0f || fabsf(s.pos.z) > 10000.0f)
		return false;
	return true;
}

// ---------------------------------------------------------------- owned entities: found in the pools, told around
// (should this entity be shared? The players' own peds, other modules' copies, the script's entities and the game's
// clock-driven planes and trains are not)
static bool
Shareable(CEntity *e)
{
	if (e->bRemoveFromWorld)
		return false;
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		if (ped->IsPlayer() || ped == FindPlayerPed())
			return false;
		if (ped->CharCreatedBy == UNK_CHAR || ped->CharCreatedBy == MISSION_CHAR)
			return false;
		const int32 mi = ped->GetModelIndex();
		if (mi <= 0 || mi > MI_LAST_PED || (mi >= MI_SPECIAL01 && mi <= MI_SPECIAL21))
			return false;
		switch (ped->m_nPedType) {
		case PEDTYPE_PLAYER1: case PEDTYPE_PLAYER2: case PEDTYPE_PLAYER3: case PEDTYPE_PLAYER4:
		case PEDTYPE_UNUSED1: case PEDTYPE_SPECIAL: case PEDTYPE_UNUSED2:
			return false;
		}
		return true;
	}
	if (e->IsVehicle()) {
		CVehicle *veh = (CVehicle *)e;
		if (KindOfVehicle(veh) == 0)
			return false;
		if (veh->VehicleCreatedBy == MISSION_VEHICLE && veh != FindPlayerVehicle())
			return false;
		return true;
	}
	return false;
}

// (a parked car just made by a car generator that passed MayGenerateParked: that generator's index)
static int32
GeneratorOf(CVehicle *veh)
{
	if (veh->VehicleCreatedBy != PARKED_VEHICLE)
		return -1;
	for (int32 i = 0; i < numGenSpawns; i++)
		if (Dist2D(genSpawns[i].pos, veh->GetPosition()) < 3.0f) {
			stats.parkedLinked++;
			return genSpawns[i].index;
		}
	return -1;
}

static NetEntity *
RegisterOwned(CEntity *e, uint8 kind)
{
	const uint32 netid = NewNetId();
	if (netid == 0)
		return nil;
	EntData *d = DataFor(e);
	if (d == nil)
		return nil;
	NetEntity *ne = Register(e, netid, LocalSlot(), false, kind);
	if (ne == nil)
		return nil;
	InitData(d, netid);
	ne->data = d;
	ne->flags = 0;
	if (e->IsVehicle())
		d->cargen = GeneratorOf((CVehicle *)e);
	return ne;
}

// (every entity of the pools not networked yet becomes ours: whatever made it — population, traffic, car generators,
// car parks, garages, cheats, roadblocks, the local player's car)
static void
Scan(void)
{
	CPedPool *peds = CPools::GetPedPool();
	for (int32 i = 0; i < peds->GetSize(); i++) {
		CPed *ped = peds->GetSlot(i);
		if (ped == nil || Find(ped) || !Shareable(ped))
			continue;
		// (a passenger of a vehicle not shared: neither is it)
		if (ped->InVehicle() && !Find(ped->m_pMyVehicle) && !Shareable(ped->m_pMyVehicle))
			continue;
		RegisterOwned(ped, KIND_PED);
	}
	CVehiclePool *vehicles = CPools::GetVehiclePool();
	for (int32 i = 0; i < vehicles->GetSize(); i++) {
		CVehicle *veh = vehicles->GetSlot(i);
		if (veh == nil || Find(veh) || !Shareable(veh))
			continue;
		RegisterOwned(veh, KindOfVehicle(veh));
	}
}

static uint8
FlagsOf(CEntity *e)
{
	uint8 flags = 0;
	if (DrivenByLocalPlayer(e))
		flags |= EF_DRIVEN;
	if (IsLaw(e))
		flags |= EF_LAW;
	if (e->IsVehicle() && ((CVehicle *)e)->VehicleCreatedBy == PARKED_VEHICLE && ((CVehicle *)e)->pDriver == nil)
		flags |= EF_PARKED;
	if (IsMission(e))
		flags |= EF_MISSION;
	if (IsDead(e))
		flags |= EF_DEAD;
	return flags;
}

// (the interval between two states of an owned entity, ms)
static double
Interval(CEntity *e, uint8 flags, float distance)
{
	const float speed = ((CPhysical *)e)->m_vecMoveSpeed.Magnitude() * 50.0f; // (m/s)
	if (e->IsVehicle()) {
		CVehicle *veh = (CVehicle *)e;
		const bool moving = speed > 0.3f || veh->m_vecTurnSpeed.Magnitude() > 0.002f;
		if (flags & EF_DRIVEN) // (as the player's own state)
			return moving ? 66.0 : 500.0;
		if (!moving)
			return 1000.0;
		return speed > 20.0f && distance < 100.0f ? 50.0 : 100.0;
	}
	CPed *ped = (CPed *)e;
	if (ped->InVehicle())
		return 1000.0;
	if (speed > 0.3f || ped->m_nMoveState >= PEDMOVE_WALK || ped->DyingOrDead())
		return distance < 50.0f ? 100.0 : 167.0;
	return 1000.0;
}

static void
SendState(NetEntity *ne, EntData *d, uint8 flags, double now)
{
	CEntity *e = ne->e;
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		uint32 vehicle = 0;
		int32 seat = 0;
		if (ped->InVehicle()) {
			vehicle = NetIdOf(ped->m_pMyVehicle);
			if (vehicle == 0) // (its vehicle not networked yet: next time)
				return;
			if (ped->m_pMyVehicle->pDriver != ped) {
				seat = 1;
				for (int32 i = 0; i < ARRAY_SIZE(ped->m_pMyVehicle->pPassengers); i++)
					if (ped->m_pMyVehicle->pPassengers[i] == ped)
						seat = i + 1;
			}
		}
		Out o(R_ENTITY_STATE, true);
		WritePed(o, ped, ne->netid, flags, d->area, vehicle, seat);
		d->sentVehicle = vehicle;
		d->sentSeat = seat;
		d->sentPedState = ped->m_nPedState;
		d->sentHealth = (uint8)Clampf(ped->m_fHealth, 0.0f, 255.0f);
	} else {
		Out o(R_ENTITY_STATE, true);
		uint32 sig = 0;
		WriteVehicle(o, (CVehicle *)e, ne->netid, ne->kind, flags, d->area, d->cargen, &sig);
		d->sentDamage = sig;
		d->sentVehicle = ((CVehicle *)e)->pDriver ? NetIdOf(((CVehicle *)e)->pDriver) : 0;
	}
	d->sentFlags = flags;
	d->announced = true;
	d->lastSent = now;
	ne->flags = flags;
	stats.sent++;
}

// (has something a remote copy shows changed since the last state? Then it goes at once)
static bool
Changed(CEntity *e, EntData *d, uint8 flags)
{
	if (flags != d->sentFlags)
		return true;
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		const uint32 vehicle = ped->InVehicle() ? NetIdOf(ped->m_pMyVehicle) : 0;
		if (vehicle != d->sentVehicle)
			return true;
		if ((ped->m_nPedState == PED_DIE || ped->m_nPedState == PED_DEAD) != (d->sentPedState == PED_DIE || d->sentPedState == PED_DEAD))
			return true;
		return false;
	}
	CVehicle *veh = (CVehicle *)e;
	uint8 engine, wheels, lights;
	uint16 doors, panels;
	DamageOf(veh, &engine, &wheels, &doors, &panels, &lights);
	if (DamageSig(engine, wheels, doors, panels, lights) != d->sentDamage)
		return true;
	return (veh->pDriver ? NetIdOf(veh->pDriver) : 0) != d->sentVehicle;
}

// (an entity of ours no longer networked: the game keeps it, unregistered)
static void
Unshare(NetEntity *ne, EntData *d)
{
	CEntity *e = ne->e;
	d->netid = 0;
	ne->data = nil;
	Unregister(e);
}

static void
SendStates(double now)
{
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d == nil || ne->puppet || ne->netid != d->netid)
			continue;
		if (ne->owner != LocalSlot()) // (the server gives it to another: our states would be dropped)
			continue;
		CEntity *e = ne->e;
		if (e->bRemoveFromWorld)
			continue;
		// (a script's entity no longer shareable, its mission car the local player left or a ped the script took: it
		// stays the script's alone, never claimed away and deleted under its handle)
		if (IsMission(e) && !Shareable(e)) {
			if (d->announced)
				SendGone(ne->netid, GONE_REMOVED);
			Unshare(ne, d);
			continue;
		}
		const uint8 flags = FlagsOf(e);
		float distance;
		const float range = (flags & EF_DRIVEN) ? CREATE_RANGE : SEND_RANGE;
		if (NearestPlayer(e->GetPosition(), d->area, range, false, false, &distance) < 0) {
			d->nextSend = 0.0;
			continue;
		}
		if (now < d->nextSend && !Changed(e, d, flags))
			continue;
		SendState(ne, d, flags, now);
		d->nextSend = now + Interval(e, flags, distance);
	}
}

// ---------------------------------------------------------------- puppets: the states' buffer
static void
PushSample(EntData *d, const State &s)
{
	Sample x;
	x.t = s.at;
	x.pos = s.pos;
	x.vel = s.vel;
	x.turn = s.turn;
	x.q = s.q;
	x.heading = s.heading;
	// (a jump: the old samples no longer lead to it)
	if (d->numSamples > 0 && (d->samples[d->numSamples - 1].pos - x.pos).Magnitude() > 30.0f)
		d->numSamples = 0;
	if (d->numSamples > 0 && x.t <= d->samples[d->numSamples - 1].t) {
		if (x.t < d->samples[0].t)
			return;
		int32 k = d->numSamples - 1;
		while (k > 0 && d->samples[k - 1].t > x.t)
			k--;
		if (d->samples[k].t == x.t)
			return;
		if (d->numSamples == MAX_SAMPLES) {
			memmove(&d->samples[0], &d->samples[1], (MAX_SAMPLES - 1) * sizeof(Sample));
			d->numSamples--;
			k--;
			if (k < 0)
				return;
		}
		memmove(&d->samples[k + 1], &d->samples[k], (d->numSamples - k) * sizeof(Sample));
		d->samples[k] = x;
		d->numSamples++;
		return;
	}
	if (d->numSamples == MAX_SAMPLES) {
		memmove(&d->samples[0], &d->samples[1], (MAX_SAMPLES - 1) * sizeof(Sample));
		d->numSamples--;
	}
	d->samples[d->numSamples++] = x;
}

// (the sample to show now: between the two around now - DELAY, or the last one carried on a little)
static bool
Interpolate(EntData *d, Sample &out)
{
	const int32 n = d->numSamples;
	if (n == 0)
		return false;
	const double rt = Now() - DELAY;
	const Sample *s = d->samples;
	if (rt <= s[0].t) {
		out = s[0];
		return true;
	}
	if (rt >= s[n - 1].t) {
		out = s[n - 1];
		double ahead = rt - s[n - 1].t;
		if (ahead > EXTRAPOLATE)
			ahead = EXTRAPOLATE;
		out.pos += s[n - 1].vel * (float)(ahead / 20.0); // (speeds are per 1/50 s)
		return true;
	}
	int32 k = 0;
	while (k + 1 < n && s[k + 1].t < rt)
		k++;
	const Sample &a = s[k], &b = s[k + 1];
	const float f = (float)((rt - a.t) / (b.t - a.t));
	out.t = rt;
	out.pos = a.pos + (b.pos - a.pos) * f;
	out.vel = a.vel + (b.vel - a.vel) * f;
	out.turn = a.turn + (b.turn - a.turn) * f;
	out.q = Nlerp(a.q, b.q, f);
	out.heading = LerpAngle(a.heading, b.heading, f);
	return true;
}

// ---------------------------------------------------------------- puppets: counting, damage, seats
// (a copy counts in the game's ped/car counters only inside the local population's radius — the density stays the
// game's where players overlap, and a remote crowd far away does not starve our own streets)
static void
SetCounted(CEntity *e, EntData *d, bool counted)
{
	if (d->counted == counted)
		return;
	d->counted = counted;
	if (e->IsPed())
		CPopulation::UpdatePedCount((ePedType)((CPed *)e)->m_nPedType, !counted);
	else if (e->IsVehicle())
		CCarCtrl::UpdateCarCount((CVehicle *)e, !counted);
}

static bool
InsideLocalPopulation(CEntity *e)
{
	if (e->IsVehicle() && ((CVehicle *)e)->GetStatus() == STATUS_WRECKED)
		return false;
	const float d = Dist2D(e->GetPosition(), FindPlayerCentreOfWorld(CWorld::PlayerInFocus));
	if (e->IsPed())
		return d < CPopulation::PedCreationDistMultiplier() * 51.0f * TheCamera.GenerationDistMultiplier;
	return d < 120.0f * TheCamera.GenerationDistMultiplier * (((CVehicle *)e)->bExtendedRange ? 1.5f : 1.0f);
}

static int32
DoorNode(int32 door)
{
	static const int32 nodes[6] = { CAR_BONNET, CAR_BOOT, CAR_DOOR_LF, CAR_DOOR_RF, CAR_DOOR_LR, CAR_DOOR_RR };
	return nodes[door];
}

static int32
PanelNode(int32 panel)
{
	static const int32 nodes[7] = { CAR_WING_LF, CAR_WING_RF, CAR_WING_LR, CAR_WING_RR, CAR_WINDSCREEN, CAR_BUMP_FRONT, CAR_BUMP_REAR };
	return nodes[panel];
}

// (the owner's damage on our copy: parts only ever get worse here, a full repair is a repair)
static void
ApplyDamage(CVehicle *veh, const State &s, bool flying)
{
	if (veh->IsBike()) {
		CBike *bike = (CBike *)veh;
		bike->m_wheelStatus[0] = s.wheels & 3;
		bike->m_wheelStatus[1] = (s.wheels >> 2) & 3;
		return;
	}
	if (!veh->IsCar())
		return;
	CAutomobile *car = (CAutomobile *)veh;
	CDamageManager &dm = car->Damage;
	if (s.doors == 0 && s.panels == 0 && s.lights == 0 && s.wheels == 0) {
		bool damaged = false;
		for (int32 i = 0; i < 6 && !damaged; i++)
			damaged = dm.GetDoorStatus(i) != 0 && !(car->pHandling->Flags & HANDLING_NO_DOORS);
		for (int32 i = 0; i < 7 && !damaged; i++)
			damaged = dm.GetPanelStatus(i) != 0;
		for (int32 i = 0; i < 4 && !damaged; i++)
			damaged = dm.GetWheelStatus(i) != 0 || dm.GetLightStatus((eLights)i) != 0;
		if (damaged)
			car->Fix(); // (Pay'n'Spray, a repair)
	}
	for (int32 i = 0; i < 6; i++) {
		const uint32 st = (s.doors >> (i * 2)) & 3;
		if (st > (uint32)dm.GetDoorStatus(i)) {
			dm.SetDoorStatus(i, st);
			car->SetDoorDamage(DoorNode(i), (eDoors)i, !flying);
		}
	}
	for (int32 i = 0; i < 7; i++) {
		const uint32 st = (s.panels >> (i * 2)) & 3;
		if (st > (uint32)dm.GetPanelStatus(i)) {
			dm.SetPanelStatus(i, st);
			if (i <= VEHPANEL_WINDSCREEN)
				car->SetPanelDamage(PanelNode(i), (ePanels)i, !flying);
			else
				car->SetBumperDamage(PanelNode(i), (ePanels)i, !flying);
		}
	}
	for (int32 i = 0; i < 4; i++) {
		const uint32 light = (s.lights >> (i * 2)) & 3, wheel = (s.wheels >> (i * 2)) & 3;
		if (light != (uint32)dm.GetLightStatus((eLights)i))
			dm.SetLightStatus((eLights)i, light);
		if (wheel != (uint32)dm.GetWheelStatus(i))
			dm.SetWheelStatus(i, wheel);
	}
	dm.SetEngineStatus(s.engine);
}

// (the players module's copies seated in a vehicle: taken off it before the vehicle goes, the game's destructor would
// destroy them)
static void
ReleasePlayerOccupants(CVehicle *veh)
{
	CPed *peds[1 + ARRAY_SIZE(veh->pPassengers)];
	int32 n = 0;
	if (veh->pDriver)
		peds[n++] = veh->pDriver;
	for (int32 i = 0; i < ARRAY_SIZE(veh->pPassengers); i++)
		if (veh->pPassengers[i])
			peds[n++] = veh->pPassengers[i];
	for (int32 k = 0; k < n; k++) {
		CPed *ped = peds[k];
		NetEntity *one = Find(ped);
		if (one == nil || one->player < 0)
			continue;
		if (veh->pDriver == ped)
			veh->pDriver = nil;
		for (int32 i = 0; i < ARRAY_SIZE(veh->pPassengers); i++)
			if (veh->pPassengers[i] == ped) {
				veh->pPassengers[i] = nil;
				if (veh->m_nNumPassengers > 0)
					veh->m_nNumPassengers--;
			}
		ped->bInVehicle = false;
		ped->m_pMyVehicle = nil;
		ped->bUsesCollision = true;
		ped->SetPedState(PED_IDLE);
	}
}

// (a copy of ours seated by the owner's state)
static bool
Seat(CPed *ped, CVehicle *veh, int32 seat)
{
	if (seat == 0) {
		if (veh->pDriver && veh->pDriver != ped)
			return false;
		if (veh->pDriver != ped)
			veh->SetDriver(ped);
	} else {
		const int32 n = seat - 1;
		if (n >= veh->m_nNumMaxPassengers || n >= ARRAY_SIZE(veh->pPassengers))
			return false;
		if (veh->pPassengers[n] && veh->pPassengers[n] != ped)
			return false;
		if (veh->pPassengers[n] != ped && !veh->AddPassenger(ped, n))
			return false;
	}
	ped->m_pMyVehicle = veh;
	veh->RegisterReference((CEntity **)&ped->m_pMyVehicle);
	ped->bInVehicle = true;
	ped->SetPedState(PED_DRIVING);
	ped->bUsesCollision = false;
	ped->bIsVisible = true;
	if (veh->bIsBus)
		ped->bRenderPedInCar = false;
	ped->AddInCarAnims(veh, seat == 0);
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType);
	if (info && info->m_nModelId > 0)
		ped->RemoveWeaponModel(info->m_nModelId);
	return true;
}

static void
Unseat(CPed *ped, const CVector &pos)
{
	CVehicle *veh = ped->m_pMyVehicle;
	if (veh) {
		if (veh->pDriver == ped)
			veh->pDriver = nil;
		else
			veh->RemovePassenger(ped);
		if (veh->bIsBus)
			ped->bRenderPedInCar = true;
	}
	ped->RemoveInCarAnims();
	ped->m_pVehicleAnim = nil;
	ped->bInVehicle = false;
	ped->m_pMyVehicle = nil;
	ped->bUsesCollision = true;
	ped->SetPedState(PED_IDLE);
	ped->m_nStoredMoveState = PEDMOVE_NONE;
	CAnimManager::BlendAnimation(ped->GetClump(), ped->m_animGroup, ANIM_STD_IDLE, 1000.0f);
	ped->Teleport(pos);
}

// ---------------------------------------------------------------- puppets: made, deleted
static void
DeleteEntity(CEntity *e)
{
	if (e->IsVehicle())
		ReleasePlayerOccupants((CVehicle *)e);
	CWorld::Remove(e);
	CWorld::RemoveReferencesToDeletedObject(e);
	delete e;
}

// (a copy deleted here: its owner keeps it)
static void
DeletePuppet(NetEntity *ne)
{
	CEntity *e = ne->e;
	EntData *d = Data(ne);
	if (d)
		SetCounted(e, d, true); // (the destructor takes it out of the counters)
	stats.deleted++;
	DeleteEntity(e);
}

static int32
PuppetStatus(const State &s)
{
	switch (s.status) {
	case STATUS_WRECKED: return STATUS_WRECKED;
	case STATUS_ABANDONED: return STATUS_ABANDONED;
	case STATUS_HELI: return s.kind == KIND_HELI ? STATUS_HELI : STATUS_PHYSICS;
	}
	return STATUS_PHYSICS; // (never PLAYER: pads and cheats would read it as the local player's; never SIMPLE: rails)
}

static uint8
PuppetCreatedBy(const State &s)
{
	if (s.createdBy == PARKED_VEHICLE)
		return PARKED_VEHICLE;
	if (s.createdBy == PERMANENT_VEHICLE)
		return PERMANENT_VEHICLE;
	return RANDOM_VEHICLE; // (the counters' traffic, deletable when it becomes ours)
}

// (may a copy of this state be made here: its model, its kind)
static bool
ValidModel(const State &s)
{
	if (s.model >= MODELINFOSIZE)
		return false;
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(s.model);
	if (mi == nil)
		return false;
	if (s.kind == KIND_PED) {
		if (mi->GetModelType() != MITYPE_PED || s.model == 0 || (s.model >= MI_SPECIAL01 && s.model <= MI_SPECIAL21))
			return false;
		switch (s.pedType) {
		case PEDTYPE_CIVMALE: case PEDTYPE_CIVFEMALE: case PEDTYPE_GANG1: case PEDTYPE_GANG2: case PEDTYPE_GANG3:
		case PEDTYPE_GANG4: case PEDTYPE_GANG5: case PEDTYPE_GANG6: case PEDTYPE_GANG7: case PEDTYPE_GANG8:
		case PEDTYPE_GANG9: case PEDTYPE_CRIMINAL: case PEDTYPE_PROSTITUTE:
			return true;
		// (these constructors pick their own model: the state's must be that one, so that the loaded check covers it)
		case PEDTYPE_EMERGENCY:
			return s.model == MI_MEDIC;
		case PEDTYPE_FIREMAN:
			return s.model == MI_FIREMAN;
		case PEDTYPE_COP:
			switch (s.copType) {
			case COP_STREET: return s.model == MI_COP;
			case COP_FBI: return s.model == MI_FBI;
			case COP_SWAT: case COP_HELI_SWAT: return s.model == MI_SWAT;
			case COP_ARMY: return s.model == MI_ARMY;
			case COP_MIAMIVICE: return s.model >= MI_VICE1 && s.model <= MI_VICE8;
			}
			return false;
		}
		return false;
	}
	if (mi->GetModelType() != MITYPE_VEHICLE)
		return false;
	switch (((CVehicleModelInfo *)mi)->m_vehicleType) {
	case VEHICLE_TYPE_CAR: return s.kind == KIND_AUTOMOBILE;
	case VEHICLE_TYPE_BIKE: return s.kind == KIND_BIKE;
	case VEHICLE_TYPE_BOAT: return s.kind == KIND_BOAT;
	case VEHICLE_TYPE_HELI: return s.kind == KIND_HELI;
	}
	return false;
}

static void SetPuppetFlags(NetEntity *ne);
static void BecomeReal(NetEntity *ne);

static CPed *
MakePed(const State &s)
{
	CPed *ped;
	switch (s.pedType) {
	case PEDTYPE_COP:
		ped = new CCopPed((eCopType)s.copType, s.copType == COP_MIAMIVICE ? s.model - MI_VICE1 : 0);
		break;
	case PEDTYPE_EMERGENCY:
	case PEDTYPE_FIREMAN:
		ped = new CEmergencyPed(s.pedType);
		break;
	default:
		ped = new CCivilianPed((ePedType)s.pedType, s.model);
		break;
	}
	if (ped == nil)
		return nil;
	ped->CharCreatedBy = UNK_CHAR; // (never deleted by the game's removers, its AI never run)
	ped->bAllowMedicsToReviveMe = false;
	ped->bRespondsToThreats = false;
	ped->m_fRotationCur = ped->m_fRotationDest = s.heading;
	ped->SetHeading(s.heading);
	ped->SetPosition(s.pos);
	ped->m_vecMoveSpeed = s.vel;
	CWorld::Add(ped);
	return ped;
}

static CVehicle *
MakeVehicle(const State &s)
{
	CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::GetModelInfo(s.model);
	if (mi->m_numComps != 0) {
		// (the state's extras index m_comps[]: kept only when they name one of this model's components)
		const int32 numComps = Min((int32)mi->m_numComps, 6);
		const int8 extra0 = s.extra0 >= 0 && s.extra0 < numComps ? s.extra0 : -1;
		const int8 extra1 = s.extra1 >= 0 && s.extra1 < numComps ? s.extra1 : -1;
		CVehicleModelInfo::SetComponentsToUse(extra0, extra1);
	}
	const uint8 createdBy = PuppetCreatedBy(s);
	CVehicle *veh;
	switch (s.kind) {
	case KIND_BIKE: veh = new CBike(s.model, createdBy); break;
	case KIND_BOAT: veh = new CBoat(s.model, createdBy); break;
	case KIND_HELI: veh = new CHeli(s.model, createdBy); break;
	default: veh = new CAutomobile(s.model, createdBy); break;
	}
	if (veh == nil)
		return nil;
	SetRotation(veh->GetMatrix(), s.q);
	veh->SetPosition(s.pos);
	veh->m_vecMoveSpeed = s.vel;
	veh->m_vecTurnSpeed = s.turn;
	veh->m_currentColour1 = s.colour1;
	veh->m_currentColour2 = s.colour2;
	veh->m_randomSeed = s.seed;
	veh->m_nDoorLock = (eCarLock)s.doorLock;
	veh->SetStatus(PuppetStatus(s));
	veh->bEngineOn = (s.vflags & VF_ENGINE) != 0;
	veh->bExtendedRange = (s.vflags & VF_EXTENDED) != 0;
	veh->bCreatedAsPoliceVehicle = (s.vflags & VF_POLICE) != 0;
	veh->bHasBeenOwnedByPlayer = (s.vflags & VF_OWNEDBYPLAYER) != 0;
	veh->m_fHealth = s.vhealth;
	veh->bRenderScorched = (s.vflags & VF_SCORCHED) != 0;
	if (veh->IsBike())
		((CBike *)veh)->bIsStanding = (s.vflags & VF_STANDING) != 0;
	veh->AutoPilot.m_nCarMission = MISSION_NONE;
	veh->bIsStatic = false;
	CWorld::Add(veh);
	ApplyDamage(veh, s, false);
	return veh;
}

static bool
InRange(const State &s)
{
	if (s.area != CGame::currArea)
		return false;
	return (s.pos - FindPlayerCoors()).Magnitude() <= CREATE_RANGE;
}

static int32
NumPuppets(bool peds)
{
	int32 n = 0;
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		if (ne && ne->puppet && ne->e->IsPed() == peds)
			n++;
	}
	return n;
}

// (may one more copy take a pool slot without starving the local population?)
static bool
PuppetRoom(bool ped)
{
	if (ped) {
		CPedPool *pool = CPools::GetPedPool();
		const int32 cap = CGame::IsInInterior() ? CPopulation::MaxNumberOfPedsInUseInterior : CPopulation::MaxNumberOfPedsInUse;
		const int32 toCome = Max(cap - (int32)CPopulation::ms_nTotalPeds, 0);
		return pool->GetNoOfFreeSpaces() > toCome + PED_MARGIN && NumPuppets(true) < pool->GetSize() / 2;
	}
	CVehiclePool *pool = CPools::GetVehiclePool();
	const int32 inUse = CCarCtrl::NumFiretrucksOnDuty + CCarCtrl::NumAmbulancesOnDuty + CCarCtrl::NumParkedCars +
	                    CCarCtrl::NumMissionCars + CCarCtrl::NumLawEnforcerCars + CCarCtrl::NumRandomCars;
	const int32 toCome = Max(CCarCtrl::MaxNumberOfCarsInUse - inUse, 0);
	return pool->GetNoOfFreeSpaces() > toCome + VEHICLE_MARGIN && NumPuppets(false) < pool->GetSize() / 2;
}

// (a pool nearly full: the farthest uncounted copy of that pool deleted, made again from a later state when there is
// room, so that the game's pool-full removers never delete the local player's own entities)
static void
RelievePool(bool peds)
{
	const int32 free = peds ? CPools::GetPedPool()->GetNoOfFreeSpaces() : CPools::GetVehiclePool()->GetNoOfFreeSpaces();
	if (free >= POOL_RELIEF)
		return;
	const CVector here = FindPlayerCoors();
	CVehicle *mine = FindPlayerVehicle();
	NetEntity *farthest = nil;
	float best = -1.0f;
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d == nil || !ne->puppet || d->counted || ne->e->IsPed() != peds || ne->e->bRemoveFromWorld || ne->e == mine)
			continue;
		if (peds && ((CPed *)ne->e)->InVehicle() && ((CPed *)ne->e)->m_pMyVehicle == mine)
			continue;
		const float dist = (ne->e->GetPosition() - here).MagnitudeSqr();
		if (dist > best) {
			best = dist;
			farthest = ne;
		}
	}
	if (farthest) {
		stats.relieved++;
		DeletePuppet(farthest);
	}
}

// (a copy made of a state; nil while it cannot be yet)
static NetEntity *
MakePuppet(int32 from, const State &s, bool *waiting)
{
	*waiting = false;
	if (!ValidModel(s))
		return nil;
	if (!CStreaming::HasModelLoaded(s.model)) {
		CStreaming::RequestModel(s.model, STREAMFLAGS_DEPENDENCY);
		*waiting = true;
		return nil;
	}
	CVehicle *vehicle = nil;
	if (s.kind == KIND_PED && s.vehicle != 0) {
		CEntity *v = EntityOf(s.vehicle);
		if (v == nil || !v->IsVehicle()) { // (its vehicle not here yet)
			*waiting = true;
			return nil;
		}
		vehicle = (CVehicle *)v;
	}
	// (room left for the local population)
	if (!PuppetRoom(s.kind == KIND_PED)) {
		*waiting = true;
		return nil;
	}
	CEntity *e = s.kind == KIND_PED ? (CEntity *)MakePed(s) : (CEntity *)MakeVehicle(s);
	if (e == nil)
		return nil;
	NetEntity *ne = Register(e, s.netid, from, true, s.kind);
	EntData *d = DataFor(e);
	if (ne == nil || d == nil) {
		DeleteEntity(e);
		return nil;
	}
	InitData(d, s.netid);
	ne->data = d;
	ne->flags = s.flags;
	d->area = s.area;
	d->cargen = s.kind == KIND_PED ? -1 : s.cargen;
	d->st = s;
	d->hasState = true;
	d->lastState = Now();
	PushSample(d, s);
	SetPuppetFlags(ne);
	if (vehicle)
		Seat((CPed *)e, vehicle, s.seat);
	stats.created++;
	return ne;
}

// ---------------------------------------------------------------- pending states
static Pending *
FindPending(uint32 netid)
{
	for (int32 i = 0; i < MAX_PENDING; i++)
		if (pending[i].used && pending[i].st.netid == netid)
			return &pending[i];
	return nil;
}

static void
StorePending(int32 from, const State &s)
{
	Pending *p = FindPending(s.netid);
	if (p == nil) {
		// (the table full: the oldest entry evicted, a far one first, one the server gave us last, told gone then)
		int32 victim = -1, victimRank = -1;
		for (int32 i = 0; i < MAX_PENDING; i++) {
			if (!pending[i].used) {
				p = &pending[i];
				break;
			}
			const int32 rank = pending[i].owned ? 0 : !InRange(pending[i].st) ? 2 : 1;
			if (rank > victimRank || (rank == victimRank && pending[i].at < pending[victim].at)) {
				victim = i;
				victimRank = rank;
			}
		}
		if (p == nil) {
			p = &pending[victim];
			if (p->owned)
				SendGone(p->st.netid, GONE_NOT_HERE);
			stats.evicted++;
		}
		p->owned = false;
	}
	p->used = true;
	p->from = from;
	p->at = Now();
	p->st = s;
}

// (the states waiting: made into copies when they can be, vehicles first so that their occupants find them)
static void
MakePending(double now)
{
	int32 made = 0;
	for (int32 pass = 0; pass < 2; pass++)
		for (int32 i = 0; i < MAX_PENDING && made < MAX_CREATE_PER_FRAME; i++) {
			Pending &p = pending[i];
			if (!p.used)
				continue;
			if (now - p.at > PENDING_LIFE) {
				if (p.owned) // (ours by the server, never made here: told gone)
					SendGone(p.st.netid, GONE_NOT_HERE);
				p.used = false;
				continue;
			}
			if ((pass == 0) != (p.st.kind != KIND_PED))
				continue;
			if (FindNet(p.st.netid)) {
				p.used = false;
				continue;
			}
			if (!InRange(p.st)) {
				if (p.owned) { // (ours by the server, but far from here: told gone)
					SendGone(p.st.netid, GONE_NOT_HERE);
					p.used = false;
				}
				continue;
			}
			bool waiting;
			NetEntity *ne = MakePuppet(p.from, p.st, &waiting);
			if (ne == nil) {
				if (!waiting)
					p.used = false;
				continue;
			}
			made++;
			const bool owned = p.owned;
			p.used = false;
			if (owned)
				BecomeReal(ne);
		}
}

// ---------------------------------------------------------------- conversions
// (a copy: no AI, no physics, kept by the game's removers)
static void
SetPuppetFlags(NetEntity *ne)
{
	CEntity *e = ne->e;
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		ped->CharCreatedBy = UNK_CHAR;
		ped->bAllowMedicsToReviveMe = false;
		ped->bRespondsToThreats = false;
		ped->m_objective = OBJECTIVE_NONE;
		ped->m_pedInObjective = nil;
		ped->m_carInObjective = nil;
		ped->m_threatEntity = nil;
		ped->m_pSeekTarget = nil;
		ped->bIsInTheAir = false;
		ped->bIsLanding = false;
		if (!ped->InVehicle() && !ped->DyingOrDead()) {
			ped->ClearAimFlag();
			ped->ClearLookFlag();
			ped->SetPedState(PED_IDLE);
			ped->m_nStoredMoveState = PEDMOVE_NONE;
		}
	} else if (e->IsVehicle()) {
		CVehicle *veh = (CVehicle *)e;
		veh->bIsLocked = true;
		veh->bInfiniteMass = true;
		veh->bCreateRoadBlockPeds = false;
		veh->AutoPilot.m_nCarMission = MISSION_NONE;
		veh->AutoPilot.m_nTempAction = TEMPACT_NONE;
		veh->ChangeLawEnforcerState(false);
		if (veh->GetStatus() == STATUS_SIMPLE || veh->GetStatus() == STATUS_PLAYER || veh->GetStatus() == STATUS_PLAYER_REMOTE)
			veh->SetStatus(STATUS_PHYSICS);
	}
	ne->puppet = true;
}

// (our real entity becomes a copy of the new owner's)
static void
BecomePuppet(NetEntity *ne, int32 owner)
{
	EntData *d = Data(ne);
	if (d == nil)
		return;
	CEntity *e = ne->e;
	ne->owner = (uint8)owner;
	if (ne->puppet)
		return;
	SetPuppetFlags(ne);
	d->counted = true;
	d->claiming = false;
	d->lastState = Now();
	d->ownerMissing = 0.0;
	d->rebuild = false;
	// (shown where it is until the new owner's states come)
	State &s = d->st;
	s.netid = ne->netid;
	s.kind = ne->kind;
	s.flags = IsDead(e) ? EF_DEAD : 0;
	s.pos = e->GetPosition();
	s.vel = ((CPhysical *)e)->m_vecMoveSpeed;
	s.area = d->area;
	s.model = e->GetModelIndex();
	s.at = Now();
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		s.heading = ped->m_fRotationCur;
		s.pedType = ped->m_nPedType;
		s.moveState = ped->m_nMoveState;
		s.pedState = ped->m_nPedState;
		s.health = (uint8)Clampf(ped->m_fHealth, 0.0f, 255.0f);
		s.vehicle = ped->InVehicle() ? NetIdOf(ped->m_pMyVehicle) : 0;
		s.weapon = ped->GetWeapon()->m_eWeaponType;
	} else {
		CVehicle *veh = (CVehicle *)e;
		s.q = QuatOf(veh->GetMatrix());
		s.turn = veh->m_vecTurnSpeed;
		s.status = veh->GetStatus();
		s.vhealth = (uint16)Clampf(veh->m_fHealth, 0.0f, 65535.0f);
		s.vflags = (veh->bEngineOn ? VF_ENGINE : 0) | (veh->bRenderScorched ? VF_SCORCHED : 0);
		s.cargen = d->cargen;
		uint8 engine, wheels, lights;
		uint16 doors, panels;
		DamageOf(veh, &engine, &wheels, &doors, &panels, &lights);
		s.engine = engine; s.wheels = wheels; s.doors = doors; s.panels = panels; s.lights = lights;
		for (int32 i = 0; i < 4; i++)
			s.susp[i] = veh->IsCar() ? (uint8)(Clampf(((CAutomobile *)veh)->m_aSuspensionSpringRatio[i], 0.0f, 1.0f) * 255.0f) : 255;
	}
	d->hasState = true;
	d->numSamples = 0;
	PushSample(d, s);
}

// (a copy becomes ours: the game's own behaviour from where it is — peds wander, cars join the roads, a parked car
// stays parked, a car whose driver was a player stays where it is)
static void
BecomeReal(NetEntity *ne)
{
	EntData *d = Data(ne);
	if (d == nil)
		return;
	CEntity *e = ne->e;
	ne->owner = (uint8)LocalSlot();
	if (!ne->puppet)
		return;
	ne->puppet = false;
	SetCounted(e, d, true);
	d->claiming = false;
	d->giving = false;
	d->announced = true; // (the others know it: its deletion is told)
	d->nextSend = 0.0;
	d->numSamples = 0;
	const uint32 now = CTimer::GetTimeInMilliseconds();
	if (e->IsPed()) {
		CPed *ped = (CPed *)e;
		ped->CharCreatedBy = RANDOM_CHAR;
		ped->bAllowMedicsToReviveMe = true;
		ped->bRespondsToThreats = true;
		ped->bIsInSafePosition = false;
		ped->bIsVisible = true;
		ped->m_bloodyFootprintCountOrDeathTime = now;
		if (ped->InVehicle()) {
			ped->SetPedState(PED_DRIVING);
		} else if (ped->m_nPedState == PED_DIE || ped->m_nPedState == PED_DEAD || ped->m_fHealth <= 0.0f) {
			ped->m_fHealth = 0.0f;
			ped->SetPedState(PED_DEAD);
			ped->bUsesCollision = false;
		} else {
			ped->bUsesCollision = true;
			ped->SetPedState(PED_IDLE);
			ped->SetMoveState(PEDMOVE_WALK);
			ped->SetWanderPath(CGeneral::GetRandomNumber() & 7);
		}
	} else if (e->IsVehicle()) {
		CVehicle *veh = (CVehicle *)e;
		veh->bIsLocked = false;
		veh->bInfiniteMass = false;
		veh->bIsInSafePosition = false;
		veh->AutoPilot.m_nTimeToStartMission = now;
		veh->AutoPilot.m_nAntiReverseTimer = now;
		CPed *driver = veh->pDriver;
		NetEntity *dn = driver ? Find(driver) : nil;
		const bool playerDriver = dn && dn->player >= 0;
		if (veh->GetStatus() == STATUS_WRECKED) {
			// (a wreck stays a wreck)
		} else if (driver && !playerDriver && !(d->st.flags & EF_DRIVEN) && !veh->IsHeli() && !veh->IsBoat()) {
			CCarCtrl::JoinCarWithRoadSystem(veh);
			veh->AutoPilot.m_nCarMission = MISSION_CRUISE;
			veh->AutoPilot.m_nDrivingStyle = DRIVINGSTYLE_STOP_FOR_CARS;
			veh->AutoPilot.m_nTempAction = TEMPACT_NONE;
			const float speed = veh->m_vecMoveSpeed.Magnitude() * 60.0f; // (GAME_SPEED_TO_CARAI_SPEED)
			veh->AutoPilot.m_nCruiseSpeed = (uint8)Clampf(speed, 9.0f, 30.0f);
			veh->AutoPilot.m_fMaxTrafficSpeed = veh->AutoPilot.m_nCruiseSpeed;
			veh->bEngineOn = true;
			veh->SetStatus(STATUS_PHYSICS);
			if (veh->bCreatedAsPoliceVehicle && veh->VehicleCreatedBy == RANDOM_VEHICLE)
				veh->ChangeLawEnforcerState(true);
		} else if (!veh->IsHeli()) {
			veh->SetStatus(STATUS_ABANDONED);
			if (driver == nil)
				veh->bEngineOn = false;
		}
		veh->SetIsStatic(false);
	}
	stats.received_owner++;
}

// (an entity of ours given to another player: the server told, the local copy a puppet at once)
static void
Give(NetEntity *ne, int32 slot)
{
	EntData *d = Data(ne);
	if (d == nil || ne->puppet)
		return;
	SendOwn(ne->netid, 2, slot);
	d->giving = true;
	BecomePuppet(ne, slot);
	stats.migrations++;
}

static void
Claim(NetEntity *ne)
{
	EntData *d = Data(ne);
	if (d == nil)
		return;
	SendOwn(ne->netid, 1, LocalSlot());
	d->claiming = true;
	d->claimAt = Now();
	stats.claims++;
}

// (a vehicle claimed with its ambient occupants, made ours at once)
static void
ClaimVehicle(NetEntity *ne)
{
	CVehicle *veh = (CVehicle *)ne->e;
	Claim(ne);
	BecomeReal(ne);
	CPed *peds[1 + ARRAY_SIZE(veh->pPassengers)];
	int32 n = 0;
	if (veh->pDriver)
		peds[n++] = veh->pDriver;
	for (int32 i = 0; i < ARRAY_SIZE(veh->pPassengers); i++)
		if (veh->pPassengers[i])
			peds[n++] = veh->pPassengers[i];
	for (int32 k = 0; k < n; k++) {
		NetEntity *one = Find(peds[k]);
		if (one && one->player < 0 && one->puppet) {
			Claim(one);
			BecomeReal(one);
		}
	}
}

// (the server's table gave a netid of ours to nobody: we keep the entity under a new netid)
static void
Renew(NetEntity *ne)
{
	CEntity *e = ne->e;
	const uint8 kind = ne->kind;
	Unregister(e);
	RegisterOwned(e, kind);
	stats.orphaned++;
}

// (the local player stops getting into a car that is not ours)
static void
StopEntering(CEntity *e)
{
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->bInVehicle)
		return;
	if (player->EnteringCar() && player->m_pMyVehicle == e)
		player->QuitEnteringCar();
	else if (player->m_carInObjective == e)
		player->ClearObjective();
}

// ---------------------------------------------------------------- puppets: every frame
static void
PuppetDie(CPed *ped)
{
	ped->ClearAll();
	ped->m_fHealth = 0.0f;
	ped->SetPedState(PED_DIE);
	CAnimBlendAssociation *assoc = CAnimManager::BlendAnimation(ped->GetClump(), ASSOCGRP_STD, ANIM_STD_KO_FRONT, 4.0f);
	if (assoc) {
		assoc->flags &= ~ASSOC_FADEOUTWHENDONE;
		assoc->SetFinishCallback(CPed::FinishDieAnimCB, ped);
		ped->bIsPedDieAnimPlaying = true;
	}
	ped->StopNonPartialAnims();
	ped->m_bloodyFootprintCountOrDeathTime = CTimer::GetTimeInMilliseconds();
}

static void
PuppetWeapon(CPed *ped, const State &s)
{
	if (ped->InVehicle() || ped->DyingOrDead() || s.weapon >= WEAPONTYPE_TOTALWEAPONS)
		return;
	if (ped->GetWeapon()->m_eWeaponType == s.weapon)
		return;
	CWeaponInfo *info = CWeaponInfo::GetWeaponInfo((eWeaponType)s.weapon);
	if (info == nil)
		return;
	if (info->m_nModelId > 0 && !CStreaming::HasModelLoaded(info->m_nModelId)) {
		CStreaming::RequestModel(info->m_nModelId, STREAMFLAGS_DEPENDENCY);
		return;
	}
	if (s.weapon != WEAPONTYPE_UNARMED)
		ped->GiveWeapon((eWeaponType)s.weapon, 1000, true);
	ped->SetCurrentWeapon((eWeaponType)s.weapon);
}

// (the frame's upkeep of the copies: out of range, silent, orphaned or revived ones deleted; counters; seats; weapons)
static void
UpkeepPuppets(double now)
{
	const CVector here = FindPlayerCoors();
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d == nil || !ne->puppet)
			continue;
		CEntity *e = ne->e;
		if (e == FindPlayerVehicle() || e->bRemoveFromWorld) // (claimed below, never deleted under the player; going)
			continue;
		if (Player(ne->owner) == nil) {
			if (d->ownerMissing == 0.0)
				d->ownerMissing = now;
		} else
			d->ownerMissing = 0.0;
		const bool far = d->area != CGame::currArea || (e->GetPosition() - here).Magnitude() > DELETE_RANGE;
		// (no states lately, its owner's player far from it: it went with them — a car they drove away, teleported)
		RemotePlayer *owner = Player(ne->owner);
		const bool silent = now - d->lastState > SILENCE ||
		                    (now - d->lastState > OWNER_FAR && owner && owner->hasState && (owner->pos - e->GetPosition()).Magnitude() > DELETE_RANGE);
		const bool orphan = d->ownerMissing != 0.0 && now - d->ownerMissing > ORPHAN;
		if (far || silent || orphan || d->rebuild) {
			// (made again from its last state when it comes back to life)
			if (d->rebuild && d->hasState && !far)
				StorePending(ne->owner, d->st);
			if (d->rebuild)
				stats.rebuilt++;
			// (a passenger copy goes with its vehicle's deletion, through the game's destructor)
			DeletePuppet(ne);
			continue;
		}
		SetCounted(e, d, InsideLocalPopulation(e));
		if (e->GetIsStatic()) {
			e->SetIsStatic(false);
			((CPhysical *)e)->AddToMovingList();
		}
		if (!e->IsPed() || !d->hasState)
			continue;
		CPed *ped = (CPed *)e;
		const State &s = d->st;
		// (its seat as its owner's state says)
		CVehicle *want = nil;
		if (s.vehicle != 0) {
			CEntity *v = EntityOf(s.vehicle);
			want = v && v->IsVehicle() ? (CVehicle *)v : nil;
		}
		if (ped->bInVehicle && ped->m_pMyVehicle != want)
			Unseat(ped, s.pos);
		if (want && !ped->bInVehicle && !ped->DyingOrDead())
			Seat(ped, want, s.seat);
		d->hidden = s.vehicle != 0 && want == nil;
		PuppetWeapon(ped, s);
	}
	RelievePool(true);
	RelievePool(false);
}

static void
PuppetAlpha(CPed *ped, bool fade)
{
	int32 alpha = CVisibilityPlugins::GetClumpAlpha(ped->GetClump());
	if (fade)
		alpha = Max(alpha - 8, 0);
	else if (alpha < 255)
		alpha = Min(alpha + 16, 255);
	CVisibilityPlugins::SetClumpAlpha(ped->GetClump(), alpha);
}

static void
SafeAfterControl(CPhysical *e)
{
	e->bIsInSafePosition = true; // (CWorld::Process: no collision, no shift for it)
	e->bWasPostponed = false;
	e->bIsStuck = false;
	e->bHasCollided = false;
	e->bHasContacted = false;
	e->bHasHitWall = false;
	e->m_nCollisionRecords = 0;
	e->m_fDamageImpulse = 0.0f;
	e->m_pDamageEntity = nil;
	e->m_nDamagePieceType = 0;
	e->m_vecMoveFriction = CVector(0.0f, 0.0f, 0.0f);
	e->m_vecTurnFriction = CVector(0.0f, 0.0f, 0.0f);
	e->SetIsStatic(false);
	e->RemoveAndAdd();
	e->GetMatrix().UpdateRW();
	e->UpdateRwFrame();
}

bool
Entities_PuppetPedControl(CPed *ped, NetEntity *ne)
{
	EntData *d = Data(ne);
	if (d == nil || !d->hasState) {
		SafeAfterControl(ped);
		return true;
	}
	const State &s = d->st;
	PuppetAlpha(ped, (s.pflags & PF_FADE) != 0);
	ped->bIsInTheAir = false;
	ped->bIsLanding = false;
	ped->m_nWaitState = WAITSTATE_FALSE;
	if (ped->InVehicle()) {
		if (ped->m_nPedState != PED_DRIVING && !ped->DyingOrDead())
			ped->SetPedState(PED_DRIVING);
		if (s.flags & EF_DEAD)
			ped->m_fHealth = 0.0f;
		ped->bIsInSafePosition = true;
		ped->bWasPostponed = false;
		return true; // (CWorld::Process seats it)
	}
	ped->bIsVisible = !d->hidden;
	ped->bUsesCollision = !d->hidden && !ped->DyingOrDead();
	Sample cur;
	if (Interpolate(d, cur)) {
		ped->m_fRotationCur = ped->m_fRotationDest = cur.heading;
		ped->SetHeading(cur.heading);
		ped->SetPosition(cur.pos);
		ped->m_vecMoveSpeed = cur.vel;
	}
	const bool dead = (s.flags & EF_DEAD) != 0;
	if (ped->DyingOrDead()) {
		if (d->deadAt == 0.0)
			d->deadAt = Now();
	} else
		d->deadAt = 0.0;
	if (dead && !ped->DyingOrDead())
		PuppetDie(ped);
	else if (!dead && ped->DyingOrDead() && Now() - d->deadAt > 2000.0)
		d->rebuild = true; // (revived by its owner — not a death told sooner by another way: made again)
	if (ped->m_nPedState == PED_DIE && !ped->bIsPedDieAnimPlaying && s.pedState == PED_DEAD) {
		ped->SetPedState(PED_DEAD);
		ped->bUsesCollision = false;
		CWeaponInfo *info = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType);
		if (info && info->m_nModelId > 0)
			ped->RemoveWeaponModel(info->m_nModelId);
	}
	if (!ped->DyingOrDead()) {
		ped->m_fHealth = Max(1.0f, (float)s.health);
		if (ped->m_nPedState != PED_IDLE)
			ped->SetPedState(PED_IDLE);
		eMoveState move = (eMoveState)(s.moveState <= PEDMOVE_SPRINT ? s.moveState : PEDMOVE_STILL);
		if (move == PEDMOVE_NONE || move == PEDMOVE_JOG)
			move = move == PEDMOVE_JOG ? PEDMOVE_RUN : PEDMOVE_STILL;
		ped->m_nMoveState = move;
		ped->SetMoveAnim();
		ped->bIsDucking = (s.pflags & PF_DUCKING) != 0;
		ped->PlayFootSteps();
	}
	ped->bIsStanding = true;
	SafeAfterControl(ped);
	return true;
}

// (wheels, suspension and the type's own parts of a car copy, from its owner's state)
static void
CarParts(CAutomobile *car, const State &s)
{
	CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::GetModelInfo(car->GetModelIndex());
	CColModel *col = car->GetColModel();
	int32 onGround = 0;
	for (int32 i = 0; i < 4; i++) {
		const float ratio = s.susp[i] / 255.0f;
		car->m_aSuspensionSpringRatio[i] = car->m_aSuspensionSpringRatioPrev[i] = ratio;
		car->m_aWheelTimer[i] = ratio < 1.0f ? 4.0f : 0.0f;
		if (ratio < 1.0f)
			onGround++;
		float wheelPos = col && col->numLines > i ? col->lines[i].p0.z - ratio * car->m_aSuspensionSpringLength[i] : car->m_aWheelPosition[i];
		car->m_aWheelPosition[i] += (wheelPos - car->m_aWheelPosition[i]) * 0.75f;
		car->m_aWheelState[i] = (tWheelState)((s.wheelStates >> (i * 2)) & 3);
		CVector p;
		mi->GetWheelPosn(i, p);
		p.z = car->m_aWheelPosition[i] - mi->m_wheelScale * 0.5f;
		car->m_aWheelColPoints[i].point = car->GetMatrix() * p;
		car->m_aWheelColPoints[i].surfaceB = SURFACE_DEFAULT;
	}
	car->m_nWheelsOnGround = onGround;
	car->m_nDriveWheelsOnGroundPrev = car->m_nDriveWheelsOnGround;
	car->m_nDriveWheelsOnGround = onGround;
	car->bTaxiLight = (s.vflags & VF_TAXILIGHT) != 0;
	car->m_fGasPedalAudio = s.gas;
	if (car->IsRealHeli())
		car->m_aWheelSpeed[1] = s.e1 / EXTRA_SCALE;
	else if (car->GetModelIndex() == MI_RHINO || car->GetModelIndex() == MI_FIRETRUCK) {
		car->m_fCarGunLR = s.e1 / EXTRA_SCALE;
		car->m_fCarGunUD = s.e2 / EXTRA_SCALE;
	}
}

static void
BikeParts(CBike *bike, const State &s)
{
	CColModel *col = bike->GetColModel();
	int32 onGround = 0;
	for (int32 i = 0; i < 4; i++) {
		const float ratio = s.susp[i] / 255.0f;
		bike->m_aSuspensionSpringRatio[i] = bike->m_aSuspensionSpringRatioPrev[i] = ratio;
		bike->m_aWheelTimer[i] = ratio < 1.0f ? 4.0f : 0.0f;
		if (ratio < 1.0f)
			onGround++;
	}
	if (col && col->numLines >= 4) {
		const int32 front = bike->m_aSuspensionSpringRatio[0] < bike->m_aSuspensionSpringRatio[1] ? 0 : 1;
		const int32 rear = bike->m_aSuspensionSpringRatio[2] < bike->m_aSuspensionSpringRatio[3] ? 2 : 3;
		const float frontPos = col->lines[front].p0.z - bike->m_aSuspensionSpringRatio[front] * bike->m_aSuspensionSpringLength[front];
		const float rearPos = col->lines[rear].p0.z - bike->m_aSuspensionSpringRatio[rear] * bike->m_aSuspensionSpringLength[rear];
		bike->m_aWheelPosition[0] += (frontPos - bike->m_aWheelPosition[0]) * 0.75f;
		bike->m_aWheelPosition[1] += (rearPos - bike->m_aWheelPosition[1]) * 0.75f;
	}
	bike->m_aWheelState[0] = (tWheelState)(s.wheelStates & 3);
	bike->m_aWheelState[1] = (tWheelState)((s.wheelStates >> 2) & 3);
	bike->m_nWheelsOnGround = onGround > 2 ? 2 : onGround;
	bike->m_nDriveWheelsOnGroundPrev = bike->m_nDriveWheelsOnGround;
	bike->m_nDriveWheelsOnGround = bike->m_nWheelsOnGround;
	bike->m_fLeanLRAngle = bike->m_fLeanLRAngle2 = s.e1 / EXTRA_SCALE;
	bike->m_fWheelAngle = s.e2 / EXTRA_SCALE;
	bike->bIsStanding = (s.vflags & VF_STANDING) != 0;
	bike->m_fGasPedalAudio = s.gas;
	bike->bLeanMatrixClean = false;
	bike->CalculateLeanMatrix(); // (its rider is seated on the leaning matrix)
}

static void
BoatParts(CBoat *boat, const State &s)
{
	boat->m_fMovingSpeed = s.e1 / EXTRA_SCALE;
	boat->bBoatInWater = true;
	boat->bPropellerInWater = true;
	boat->m_bIsAnchored = false;
	boat->PruneWakeTrail();
	if (boat->m_vecMoveSpeed.Magnitude2D() > 0.05f)
		boat->AddWakePoint(boat->GetPosition());
}

bool
Entities_PuppetVehicleControl(CVehicle *veh, NetEntity *ne)
{
	EntData *d = Data(ne);
	if (d == nil || !d->hasState) {
		SafeAfterControl(veh);
		return true;
	}
	const State &s = d->st;
	Sample cur;
	if (Interpolate(d, cur)) {
		CMatrix &m = veh->GetMatrix();
		SetRotation(m, cur.q);
		m.GetPosition() = cur.pos;
		veh->m_vecMoveSpeed = cur.vel;
		veh->m_vecTurnSpeed = cur.turn;
	}
	veh->m_fSteerAngle = s.steer;
	veh->m_fGasPedal = s.gas;
	veh->m_fBrakePedal = s.brake;
	veh->bIsHandbrakeOn = (s.vflags & VF_HANDBRAKE) != 0;
	veh->m_nCurrentGear = s.gear;
	veh->bEngineOn = (s.vflags & VF_ENGINE) != 0;
	veh->m_bSirenOrAlarm = (s.vflags & VF_SIREN) != 0;
	if (s.vflags & VF_HORN) {
		veh->m_nCarHornPattern = 1;
		veh->m_nCarHornTimer = 30;
	} else
		veh->m_nCarHornTimer = 0;
	veh->m_fHealth = s.vhealth;
	veh->bFadeOut = (s.vflags & VF_FADE) != 0;
	veh->bRenderScorched = (s.vflags & VF_SCORCHED) != 0;
	int32 status = veh->GetStatus() == STATUS_WRECKED ? STATUS_WRECKED : PuppetStatus(s);
	if (status == STATUS_WRECKED && veh->GetStatus() != STATUS_WRECKED) {
		// (wrecked by its owner, the copy alive until now: after a short wait for the owner's explosion event (which
		// wrecks it with its culprit), the combat module's wreck: BlowUpCar's own blast unless the state is old)
		const double now = Now();
		if (d->wreckSeen == 0.0)
			d->wreckSeen = now;
		if (now - d->wreckSeen < WRECK_WAIT)
			status = veh->GetStatus();
		else
			Combat_WreckPuppet(veh, now - s.at < WRECK_RECENT);
	} else
		d->wreckSeen = 0.0;
	// (the plain wreck when the combat module did not: its weak stub)
	if (veh->GetStatus() != status) {
		veh->SetStatus(status);
		if (status == STATUS_WRECKED) {
			veh->m_nTimeOfDeath = CTimer::GetTimeInMilliseconds();
			veh->bLightsOn = false;
			veh->bEngineOn = false;
			veh->m_bSirenOrAlarm = false;
			if (veh->IsCar())
				((CAutomobile *)veh)->bTaxiLight = false;
		}
	}
	veh->bIsLocked = true;
	veh->bInfiniteMass = true;
	veh->UpdateClumpAlpha();
	if (veh->IsCar())
		CarParts((CAutomobile *)veh, s);
	else if (veh->IsBike())
		BikeParts((CBike *)veh, s);
	else if (veh->IsBoat())
		BoatParts((CBoat *)veh, s);
	veh->bVehicleColProcessed = false;
	SafeAfterControl(veh);
	return true;
}

// ---------------------------------------------------------------- the game's hooks (mmo.h)
// (a remote player strictly nearer the spawn point populates it — ties: the more senior; the wanted player's police
// always comes)
bool
MayGenerateAt(const CVector &pos, int32 kind)
{
	if (!Live() || kind == GEN_LAW)
		return true;
	const float local = Dist2D(pos, FindPlayerCentreOfWorld(CWorld::PlayerInFocus));
	const uint32 rank = LocalRank();
	for (int32 i = 0; i < MAX_SLOTS; i++) {
		if (i == LocalSlot())
			continue;
		RemotePlayer *p = Player(i);
		if (p == nil || p->away || !p->hasState || p->area != CGame::currArea)
			continue;
		const float d = Dist2D(pos, p->pos);
		if (d > GEN_RANGE)
			continue;
		if (d < local || (d == local && p->rank < rank)) {
			stats.refusedSpawns++;
			return false;
		}
	}
	return true;
}

bool
MayGenerateParked(int32 generator, const CVector &pos)
{
	if (!Live())
		return true;
	// (its car already here as another client's)
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d && ne->e->IsVehicle() && d->cargen == generator && (ne->puppet || ne->owner != LocalSlot()))
			return false;
	}
	for (int32 i = 0; i < MAX_PENDING; i++)
		if (pending[i].used && pending[i].st.kind != KIND_PED && pending[i].st.cargen == generator)
			return false;
	if (!MayGenerateAt(pos, GEN_PARKED))
		return false;
	if (numGenSpawns < MAX_GEN_SPAWNS) {
		genSpawns[numGenSpawns].index = generator;
		genSpawns[numGenSpawns].pos = pos;
		numGenSpawns++;
	}
	return true;
}

// (removed for distance here, but a remote player is inside its removal radius: theirs now)
bool
MigrateInsteadOfRemove(CEntity *e)
{
	if (!Live() || e == nil)
		return false;
	NetEntity *ne = Find(e);
	EntData *d = Data(ne);
	if (d == nil || ne->puppet || ne->owner != LocalSlot())
		return false;
	if (e == FindPlayerVehicle() || IsLaw(e) || IsMission(e))
		return false;
	float radius;
	if (e->IsPed())
		radius = (((CPed *)e)->IsGangMember() ? 81.0f : 51.0f) * CPopulation::PedCreationDistMultiplier();
	else
		radius = 120.0f * (((CVehicle *)e)->bExtendedRange ? 1.5f : 1.0f);
	GatherPlayers();
	const int32 slot = NearestPlayer(e->GetPosition(), d->area, radius, true, true, nil);
	if (slot < 0)
		return false;
	Give(ne, slot);
	if (e->IsVehicle()) { // (its occupants go with it)
		CVehicle *veh = (CVehicle *)e;
		CPed *peds[1 + ARRAY_SIZE(veh->pPassengers)];
		int32 n = 0;
		if (veh->pDriver)
			peds[n++] = veh->pDriver;
		for (int32 i = 0; i < ARRAY_SIZE(veh->pPassengers); i++)
			if (veh->pPassengers[i])
				peds[n++] = veh->pPassengers[i];
		for (int32 k = 0; k < n; k++) {
			NetEntity *one = Find(peds[k]);
			if (one && one->player < 0 && !one->puppet && one->owner == LocalSlot())
				Give(one, slot);
		}
	}
	return true;
}

// (an off-screen car recycled, a wreck tidied up: kept while a remote player is near it)
bool
KeepForOthers(CEntity *e)
{
	if (!Live() || e == nil)
		return false;
	NetEntity *ne = Find(e);
	EntData *d = Data(ne);
	if (d == nil || ne->puppet)
		return false;
	GatherPlayers();
	if (NearestPlayer(e->GetPosition(), d->area, KEEP_RANGE, false, true, nil) < 0)
		return false;
	stats.keeps++;
	return true;
}

// (the local player starts getting into this car: a remote player's is refused; another client's empty or ambient car
// is claimed, ours at once)
bool
PlayerMayEnter(CVehicle *veh)
{
	if (!Live() || veh == nil)
		return true;
	NetEntity *ne = Find(veh);
	EntData *d = Data(ne);
	if (d == nil || !ne->puppet)
		return true;
	if (d->hasState && (d->st.flags & (EF_DRIVEN | EF_MISSION))) // (a remote player's, or a script's car)
		return false;
	if (veh->pDriver) {
		NetEntity *dn = Find(veh->pDriver);
		if (dn && dn->player >= 0)
			return false;
	}
	ClaimVehicle(ne);
	return true;
}

// ---------------------------------------------------------------- handlers (mmo_internal.h)
void
Entities_EntityDestroyed(CEntity *e, NetEntity *ne)
{
	EntData *d = Data(ne);
	if (e->IsVehicle())
		ReleasePlayerOccupants((CVehicle *)e);
	if (d == nil)
		return;
	if (ne->puppet) {
		SetCounted(e, d, true);
	} else if (d->announced && ne->owner == LocalSlot() && Live()) {
		SendGone(ne->netid, GONE_REMOVED);
	}
	d->netid = 0;
	ne->data = nil;
}

void
Entities_OnState(int32 from, In &in)
{
	State s;
	if (!ReadState(in, s) || !Live())
		return;
	if (from == LocalSlot() || Player(from) == nil)
		return;
	stats.received++;
	s.at = SampleTime(s.ts);
	NetEntity *ne = FindNet(s.netid);
	if (ne) {
		EntData *d = Data(ne);
		if (d == nil || !ne->puppet) // (a player's, or ours: a transfer in flight)
			return;
		if (ne->kind != s.kind || ne->e->GetModelIndex() != s.model) {
			d->rebuild = true;
			d->st = s;
			return;
		}
		ne->owner = (uint8)from;
		ne->flags = s.flags;
		d->lastState = Now();
		d->area = s.area;
		if (s.kind != KIND_PED) {
			CVehicle *veh = (CVehicle *)ne->e;
			veh->m_currentColour1 = s.colour1;
			veh->m_currentColour2 = s.colour2;
			veh->m_nDoorLock = (eCarLock)s.doorLock;
			ApplyDamage(veh, s, true);
			d->cargen = s.cargen;
		}
		d->st = s;
		d->hasState = true;
		PushSample(d, s);
		return;
	}
	// (a far state is never made into a copy here: not kept, unless the server gave us that entity: then MakePending
	// tells it gone)
	if (!InRange(s)) {
		Pending *p = FindPending(s.netid);
		if (p == nil)
			return;
		if (!p->owned) {
			p->used = false;
			return;
		}
	}
	StorePending(from, s);
}

void
Entities_OnGone(int32 from, uint32 netid, int32 reason)
{
	if (!Live())
		return;
	Pending *p = FindPending(netid);
	if (p)
		p->used = false;
	NetEntity *ne = FindNet(netid);
	if (Data(ne) && ne->puppet)
		DeletePuppet(ne);
}

void
Entities_OnOwner(uint32 netid, int32 owner, int32 previous)
{
	if (!Live())
		return;
	NetEntity *ne = FindNet(netid);
	EntData *d = Data(ne);
	if (d == nil) {
		if (ne) // (a player's)
			return;
		Pending *p = FindPending(netid);
		if (owner == LocalSlot()) {
			if (p) {
				p->owned = true;
				p->from = owner;
			} else { // (ours, but not here: told gone)
				SendGone(netid, GONE_NOT_HERE);
				stats.notHere++;
			}
		} else if (owner == NO_SLOT) {
			if (p)
				p->used = false;
		} else if (p)
			p->from = owner;
		return;
	}
	CEntity *e = ne->e;
	if (owner == LocalSlot()) {
		d->claiming = false;
		d->giving = false;
		if (ne->puppet) {
			// (a car its player drove, its player gone: it goes with them)
			if ((d->st.flags & EF_DRIVEN) && previous != NO_SLOT && previous != LocalSlot() && Player(previous) == nil &&
			    e != FindPlayerVehicle()) {
				ne->puppet = false;
				ne->owner = (uint8)LocalSlot();
				SetCounted(e, d, true);
				d->announced = false;
				SendGone(netid, GONE_DRIVER_LEFT);
				DeleteEntity(e);
				return;
			}
			// (the police heli of a wanted player gone: its AI would hunt ours)
			if (e->IsVehicle() && ((CVehicle *)e)->IsHeli()) {
				SetCounted(e, d, true);
				ne->puppet = false;
				ne->owner = (uint8)LocalSlot();
				d->announced = false;
				SendGone(netid, GONE_NOT_HERE);
				DeleteEntity(e);
				return;
			}
			BecomeReal(ne);
		}
		ne->owner = (uint8)LocalSlot();
		return;
	}
	if (owner == NO_SLOT) {
		if (ne->puppet)
			DeletePuppet(ne);
		else
			Renew(ne);
		return;
	}
	// (another player's now)
	if (!ne->puppet) {
		if (d->claiming && owner == previous)
			stats.claimsRefused++;
		d->claiming = false;
		if (e == FindPlayerVehicle() || DrivenByLocalPlayer(e)) {
			// (the local player sits in it: it stays ours here, claimed again until the server agrees)
			ne->owner = (uint8)owner;
			return;
		}
		if (e->IsPed() && ((CPed *)e)->InVehicle() && ((CPed *)e)->m_pMyVehicle == FindPlayerVehicle() && FindPlayerVehicle()) {
			ne->owner = (uint8)owner;
			return;
		}
		// (a script's entity is never made a copy here: it would be deleted under the script's handle; it stays ours,
		// unshared, and the new owner keeps theirs)
		if (IsMission(e)) {
			Unshare(ne, d);
			return;
		}
		if (LocalPlayerEntering(e))
			StopEntering(e);
		BecomePuppet(ne, owner);
		return;
	}
	ne->owner = (uint8)owner;
	d->ownerMissing = 0.0;
}

void
Entities_GameLoaded(void)
{
	memset(pending, 0, sizeof(pending));
	numGenSpawns = 0;
	carModel = -1;
	memset(&stats, 0, sizeof(stats));
}

// (the local player's car: ours, whatever the server's table says until our claim is granted)
static void
KeepPlayerVehicle(double now)
{
	CVehicle *veh = FindPlayerVehicle();
	if (veh == nil)
		return;
	NetEntity *ne = Find(veh);
	EntData *d = Data(ne);
	if (d == nil)
		return;
	if (ne->puppet) {
		ClaimVehicle(ne);
		return;
	}
	if (ne->owner != LocalSlot() && now - d->claimAt > CLAIM_RETRY)
		Claim(ne);
	if (d->cargen >= 0) // (a generator's car taken by its player: no longer the generator's, as the game does)
		d->cargen = -1;
}

// ("car <model>": the car made next to the local player once its model is loaded, the player put in as driver)
static void
MakeTestCar(double now)
{
	if (carModel < 0)
		return;
	if (now - carAt > CAR_WAIT) {
		Social_AddLine(LINE_ERROR, "car: the model did not load");
		carModel = -1;
		return;
	}
	if (!CStreaming::HasModelLoaded(carModel))
		return;
	const int32 model = carModel;
	carModel = -1;
	CPlayerPed *player = FindPlayerPed();
	if (player == nil || player->bInVehicle)
		return;
	CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::GetModelInfo(model);
	CVehicle *veh;
	switch (mi->m_vehicleType) {
	case VEHICLE_TYPE_BIKE: veh = new CBike(model, RANDOM_VEHICLE); if (veh) ((CBike *)veh)->bIsStanding = true; break;
	case VEHICLE_TYPE_BOAT: veh = new CBoat(model, RANDOM_VEHICLE); break;
	default: veh = new CAutomobile(model, RANDOM_VEHICLE); break;
	}
	if (veh == nil)
		return;
	CVector pos = player->GetPosition() + player->GetRight() * 3.0f;
	bool found = false;
	const float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	if (found)
		pos.z = ground;
	pos.z += veh->GetDistanceFromCentreOfMassToBaseOfModel();
	veh->SetPosition(pos);
	veh->SetHeading(player->m_fRotationCur);
	veh->SetStatus(STATUS_ABANDONED);
	veh->m_nDoorLock = CARLOCK_UNLOCKED;
	veh->bHasBeenOwnedByPlayer = true;
	CWorld::Add(veh);
	player->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, veh);
	player->WarpPedIntoCar(veh);
	if (Live() && !Find(veh))
		RegisterOwned(veh, KindOfVehicle(veh));
}

void
Entities_PreWorld(void)
{
	if (!Live())
		return;
	const double now = Now();
	GatherPlayers();
	Scan();
	MakeTestCar(now);
	MakePending(now);
	UpkeepPuppets(now);
}

void
Entities_PostWorld(void)
{
	if (!Live()) {
		numGenSpawns = 0;
		return;
	}
	const double now = Now();
	GatherPlayers();
	Scan();
	KeepPlayerVehicle(now);
	SendStates(now);
	numGenSpawns = 0;
}

// (the world left, or the link lost: the copies deleted; our entities the server handed to the others (it knew them
// near one) deleted too, the others kept to be shared again under new netids)
void
Entities_Reset(void)
{
	const double now = Now();
	GatherPlayers();
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d == nil)
			continue;
		CEntity *e = ne->e;
		if (ne->puppet) {
			if (e == FindPlayerVehicle()) {
				BecomeReal(ne);
				Unregister(e);
				continue;
			}
			DeletePuppet(ne);
			continue;
		}
		const bool handedOver = d->announced && now - d->lastSent < 15000.0 &&
		                        NearestPlayer(e->GetPosition(), d->area, CREATE_RANGE, true, false, nil) >= 0;
		const bool keep = e == FindPlayerVehicle() || DrivenByLocalPlayer(e) ||
		                  (e->IsPed() && ((CPed *)e)->InVehicle() && ((CPed *)e)->m_pMyVehicle == FindPlayerVehicle());
		d->netid = 0;
		ne->data = nil;
		if (handedOver && !keep && !(e->IsPed() && ((CPed *)e)->InVehicle()))
			DeleteEntity(e);
		else
			Unregister(e);
	}
	memset(pending, 0, sizeof(pending));
	numGenSpawns = 0;
	carModel = -1;
}

void
Entities_OnEvent(int32 from, int32 sub, In &in)
{
}

void
Entities_OnTargeted(int32 from, uint32 target, int32 sub, In &in)
{
}

static void
Census(int32 *ownedPeds, int32 *ownedVehicles, int32 *puppetPeds, int32 *puppetVehicles, int32 *counted)
{
	*ownedPeds = *ownedVehicles = *puppetPeds = *puppetVehicles = *counted = 0;
	for (int32 i = 0; i < RegistrySize(); i++) {
		NetEntity *ne = RegistryAt(i);
		EntData *d = Data(ne);
		if (d == nil)
			continue;
		const bool ped = ne->e->IsPed();
		if (ne->puppet) {
			ped ? (*puppetPeds)++ : (*puppetVehicles)++;
			if (d->counted)
				(*counted)++;
		} else
			ped ? (*ownedPeds)++ : (*ownedVehicles)++;
	}
}

bool
Entities_TestCommand(const char *cmd)
{
	if (strncmp(cmd, "car", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
		int32 model = cmd[3] ? atoi(cmd + 4) : MI_LANDSTAL;
		if (model <= 0 || model >= MODELINFOSIZE || CModelInfo::GetModelInfo(model) == nil ||
		    CModelInfo::GetModelInfo(model)->GetModelType() != MITYPE_VEHICLE) {
			Social_AddLine(LINE_ERROR, "car: not a vehicle model");
			return true;
		}
		const int32 type = ((CVehicleModelInfo *)CModelInfo::GetModelInfo(model))->m_vehicleType;
		if (type != VEHICLE_TYPE_CAR && type != VEHICLE_TYPE_BIKE && type != VEHICLE_TYPE_BOAT) {
			Social_AddLine(LINE_ERROR, "car: not a car, bike or boat");
			return true;
		}
		carModel = model;
		carAt = Now();
		CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
		return true;
	}
	if (strcmp(cmd, "population") == 0) {
		int32 op, ov, pp, pv, counted;
		Census(&op, &ov, &pp, &pv, &counted);
		char line[160];
		snprintf(line, sizeof(line), "owned %d peds %d vehicles, copies %d peds %d vehicles (%d counted), game %u peds %d cars",
		         op, ov, pp, pv, counted, CPopulation::ms_nTotalPeds, CCarCtrl::NumRandomCars + CCarCtrl::NumParkedCars);
		Social_AddLine(LINE_NOTICE, line);
		debug("Orthros MMO: %s\n", line);
		return true;
	}
	return false;
}

void
Entities_Debug(void)
{
	int32 op, ov, pp, pv, counted;
	Census(&op, &ov, &pp, &pv, &counted);
	int32 waiting = 0, models = 0;
	for (int32 i = 0; i < MAX_PENDING; i++)
		if (pending[i].used) {
			waiting++;
			if (pending[i].st.model < MODELINFOSIZE && !CStreaming::HasModelLoaded(pending[i].st.model))
				models++;
		}
	for (int32 pass = 0; pass < 2; pass++) {
		DebugAppend(pass == 0 ? ",\"owned\":{\"peds\":%d,\"vehicles\":%d,\"ids\":[" : ",\"puppets\":{\"peds\":%d,\"vehicles\":%d,\"ids\":[",
		            pass == 0 ? op : pp, pass == 0 ? ov : pv);
		int32 k = 0;
		for (int32 i = 0; i < RegistrySize() && k < MAX_IDS; i++) {
			NetEntity *ne = RegistryAt(i);
			EntData *d = Data(ne);
			if (d == nil || ne->puppet != (pass == 1))
				continue;
			if (pass == 0 && !d->announced) // (ours that no remote player was told of)
				continue;
			DebugAppend("%s%u", k++ ? "," : "", ne->netid);
		}
		DebugAppend("]}");
	}
	// (the networked entities nearest the local player: where each is here, owned or a copy, and by whom)
	DebugAppend(",\"near\":[");
	if (Live()) {
		const CVector here = FindPlayerCoors();
		int32 k = 0;
		for (int32 pass = 0; pass < 2; pass++) // (vehicles first)
			for (int32 i = 0; i < RegistrySize() && k < 40; i++) {
				NetEntity *ne = RegistryAt(i);
				EntData *d = Data(ne);
				if (d == nil || ne->e->IsVehicle() != (pass == 0))
					continue;
				const CVector p = ne->e->GetPosition();
				if ((p - here).Magnitude() > 120.0f)
					continue;
				DebugAppend("%s{\"id\":%u,\"k\":%d,\"m\":%d,\"p\":[%.1f,%.1f,%.1f],\"o\":%d,\"copy\":%s}", k++ ? "," : "", ne->netid,
				            ne->kind, ne->e->GetModelIndex(), p.x, p.y, p.z, ne->owner, ne->puppet ? "true" : "false");
			}
	}
	DebugAppend("],\"playerVehicle\":%u", Live() && FindPlayerVehicle() ? NetIdOf(FindPlayerVehicle()) : 0u);
	DebugAppend(",\"entities\":{\"live\":%s,\"sent\":%u,\"received\":%u,\"created\":%u,\"deleted\":%u,\"gone\":%u,"
	            "\"migrations\":%u,\"ownerIn\":%u,\"claims\":%u,\"claimsRefused\":%u,\"keeps\":%u,\"refusedSpawns\":%u,"
	            "\"parkedLinked\":%u,\"orphaned\":%u,\"notHere\":%u,\"rebuilt\":%u,\"relieved\":%u,\"evicted\":%u,"
	            "\"pending\":%d,\"pendingModels\":%d,\"counted\":%d,\"gamePeds\":%u,\"gameCars\":%d}",
	            Live() ? "true" : "false", stats.sent, stats.received, stats.created, stats.deleted, stats.gone,
	            stats.migrations, stats.received_owner, stats.claims, stats.claimsRefused, stats.keeps, stats.refusedSpawns,
	            stats.parkedLinked, stats.orphaned, stats.notHere, stats.rebuilt, stats.relieved, stats.evicted, waiting, models, counted,
	            CPopulation::ms_nTotalPeds, CCarCtrl::NumRandomCars + CCarCtrl::NumParkedCars);
}
}
#endif
