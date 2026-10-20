// The shared world's entity registry (core, docs/mmo.md section 7): every networked ped, vehicle and object, owned here
// or a puppet of another client's, in side tables by pool slot; its network id hashed back to it. An entry remembers
// its entity's pool handle: a slot freed or given to another entity since is told apart (and forgotten) at once.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "Pools.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"

namespace OrthrosMmo
{
// (registry indices: the ped pool's slots, then the vehicle pool's, then the object pool's)
enum { REG_PEDS = 0, REG_VEHICLES = REG_PEDS + NUMPEDS, REG_OBJECTS = REG_VEHICLES + NUMVEHICLES, REG_SIZE = REG_OBJECTS + NUMOBJECTS };
// (netid -> registry index + 1, 0 empty: open addressing, linear probing; a power of two over twice REG_SIZE)
enum { HASH_BITS = 11, HASH_SIZE = 1 << HASH_BITS, HASH_MASK = HASH_SIZE - 1 };

static NetEntity entries[REG_SIZE];
static int32 handles[REG_SIZE]; // (the pool's handle of the entity when it was registered)
static int16 hashSlots[HASH_SIZE];

// ---------------------------------------------------------------- pool slots
template<typename T, typename U>
static int32
PoolSlot(CPool<T, U> *pool, const CEntity *e, int32 limit, int32 *handle)
{
	if (pool == nil)
		return -1;
	T *t = (T *)e;
	const int32 i = pool->GetJustIndex_NoFreeAssert(t);
	if (i < 0 || i >= pool->GetSize() || i >= limit || pool->GetSlot(i) != t)
		return -1;
	*handle = pool->GetIndex(t);
	return i;
}

// (the entity's registry index and pool handle; -1 if it lives in no pool the registry covers)
static int32
IndexOf(const CEntity *e, int32 *handle)
{
	if (e == nil)
		return -1;
	int32 i;
	switch (e->GetType()) {
	case ENTITY_TYPE_PED:
		i = PoolSlot(CPools::GetPedPool(), e, NUMPEDS, handle);
		return i < 0 ? -1 : REG_PEDS + i;
	case ENTITY_TYPE_VEHICLE:
		i = PoolSlot(CPools::GetVehiclePool(), e, NUMVEHICLES, handle);
		return i < 0 ? -1 : REG_VEHICLES + i;
	case ENTITY_TYPE_OBJECT:
		i = PoolSlot(CPools::GetObjectPool(), e, NUMOBJECTS, handle);
		return i < 0 ? -1 : REG_OBJECTS + i;
	}
	return -1;
}

template<typename T, typename U>
static int32
SlotHandle(CPool<T, U> *pool, int32 slot, const CEntity *e)
{
	if (pool == nil || slot >= pool->GetSize())
		return -1;
	T *t = pool->GetSlot(slot);
	return t != nil && (const CEntity *)t == e ? pool->GetIndex(t) : -1;
}

// (an entry still its entity's: the slot neither freed nor used again; read from the pool, not from the entity)
static bool
StillThere(int32 i)
{
	const NetEntity &ne = entries[i];
	if (ne.e == nil)
		return false;
	int32 handle;
	if (i < REG_VEHICLES)
		handle = SlotHandle(CPools::GetPedPool(), i - REG_PEDS, ne.e);
	else if (i < REG_OBJECTS)
		handle = SlotHandle(CPools::GetVehiclePool(), i - REG_VEHICLES, ne.e);
	else
		handle = SlotHandle(CPools::GetObjectPool(), i - REG_OBJECTS, ne.e);
	return handle >= 0 && handle == handles[i];
}

// ---------------------------------------------------------------- netid hash
static inline uint32
HashOf(uint32 netid)
{
	return (netid * 2654435761u) >> (32 - HASH_BITS);
}

// (the hash position holding netid; -1 if none)
static int32
HashFind(uint32 netid)
{
	uint32 h = HashOf(netid);
	for (int32 n = 0; n < HASH_SIZE; n++, h = (h + 1) & HASH_MASK) {
		const int32 k = hashSlots[h];
		if (k == 0)
			return -1;
		if (entries[k - 1].netid == netid)
			return h;
	}
	return -1;
}

static void
HashInsert(uint32 netid, int32 index)
{
	uint32 h = HashOf(netid);
	while (hashSlots[h] != 0)
		h = (h + 1) & HASH_MASK;
	hashSlots[h] = index + 1;
}

// (the hole filled back by the entries after it that may move into it: no tombstones)
static void
HashRemove(uint32 pos)
{
	hashSlots[pos] = 0;
	for (uint32 j = (pos + 1) & HASH_MASK; hashSlots[j] != 0; j = (j + 1) & HASH_MASK) {
		const int32 k = hashSlots[j];
		const uint32 home = HashOf(entries[k - 1].netid);
		if (((j - home) & HASH_MASK) >= ((j - pos) & HASH_MASK)) {
			hashSlots[pos] = k;
			hashSlots[j] = 0;
			pos = j;
		}
	}
}

static void
Unhash(int32 i)
{
	if (entries[i].netid == 0)
		return;
	const int32 pos = HashFind(entries[i].netid);
	if (pos >= 0 && hashSlots[pos] == i + 1)
		HashRemove(pos);
}

static void
Clear(int32 i)
{
	Unhash(i);
	NetEntity &ne = entries[i];
	ne.e = nil;
	ne.netid = 0;
	ne.owner = NO_SLOT;
	ne.kind = 0;
	ne.flags = 0;
	ne.puppet = false;
	ne.player = -1;
	ne.data = nil;
	handles[i] = -1;
}

// ---------------------------------------------------------------- the registry
NetEntity *
Find(const CEntity *e)
{
	int32 handle;
	const int32 i = IndexOf(e, &handle);
	if (i < 0 || entries[i].e != e)
		return nil;
	if (handles[i] != handle) { // (a stale entry: another entity in the same slot)
		Clear(i);
		return nil;
	}
	return &entries[i];
}

NetEntity *
FindNet(uint32 netid)
{
	if (netid == 0)
		return nil;
	const int32 pos = HashFind(netid);
	if (pos < 0)
		return nil;
	const int32 i = hashSlots[pos] - 1;
	if (!StillThere(i)) {
		Clear(i);
		return nil;
	}
	return &entries[i];
}

// (an entity registered again keeps its module's data, flags and player; a netid another entity had moves to this one,
// whose entry is removed — its data pointer is its module's to free)
NetEntity *
Register(CEntity *e, uint32 netid, int32 owner, bool puppet, uint8 kind)
{
	int32 handle;
	const int32 i = IndexOf(e, &handle);
	if (i < 0)
		return nil;
	NetEntity &ne = entries[i];
	const bool same = ne.e == e && handles[i] == handle;
	if (ne.e != nil && !same)
		Clear(i);
	if (same && ne.netid != netid)
		Unhash(i);
	if (netid != 0 && !(same && ne.netid == netid)) {
		const int32 pos = HashFind(netid);
		if (pos >= 0)
			Clear(hashSlots[pos] - 1);
		HashInsert(netid, i);
	}
	if (!same) {
		ne.flags = 0;
		ne.player = -1;
		ne.data = nil;
	}
	ne.e = e;
	ne.netid = netid;
	ne.owner = (uint8)owner;
	ne.puppet = puppet;
	ne.kind = kind;
	handles[i] = handle;
	return &ne;
}

void
Unregister(const CEntity *e)
{
	int32 handle;
	const int32 i = IndexOf(e, &handle);
	if (i >= 0 && entries[i].e == e)
		Clear(i);
}

int32
RegistrySize(void)
{
	return REG_SIZE;
}

NetEntity *
RegistryAt(int32 i)
{
	if (i < 0 || i >= REG_SIZE || entries[i].e == nil)
		return nil;
	if (!StillThere(i)) {
		Clear(i);
		return nil;
	}
	return &entries[i];
}

// ---------------------------------------------------------------- network ids and ownership
uint32
NetIdOf(const CEntity *e)
{
	if (e == nil)
		return 0;
	if (e == FindPlayerPed())
		return LocalSlot() != NO_SLOT ? PlayerNetId(LocalSlot()) : 0;
	const NetEntity *ne = Find(e);
	return ne ? ne->netid : 0;
}

CEntity *
EntityOf(uint32 netid)
{
	if (netid == 0)
		return nil;
	if (IsPlayerNetId(netid)) {
		const int32 slot = SlotOfNetId(netid);
		if (slot != NO_SLOT && slot == LocalSlot())
			return FindPlayerPed();
		const RemotePlayer *p = Player(slot);
		if (p && p->puppet)
			return (CEntity *)p->puppet;
	}
	const NetEntity *ne = FindNet(netid);
	return ne ? ne->e : nil;
}

// ---------------------------------------------------------------- puppets: the game's hooks, to their module
bool
IsPuppet(const CEntity *e)
{
	if (e == nil || Mode() == MODE_OFF)
		return false;
	const NetEntity *ne = Find(e);
	return ne && ne->puppet;
}

bool
PuppetPedControl(CPed *ped)
{
	if (Mode() == MODE_OFF)
		return false;
	NetEntity *ne = Find(ped);
	if (ne == nil || !ne->puppet)
		return false;
	return ne->player >= 0 ? Players_PuppetControl(ped, ne) : Entities_PuppetPedControl(ped, ne);
}

bool
PuppetVehicleControl(CVehicle *veh)
{
	if (Mode() == MODE_OFF)
		return false;
	NetEntity *ne = Find(veh);
	if (ne == nil || !ne->puppet)
		return false;
	return Entities_PuppetVehicleControl(veh, ne);
}

// (a ped or vehicle being deleted, whatever deletes it: its module told while its entry still holds, then the entry
// gone)
void
EntityDestroyed(CEntity *e)
{
	if (e == nil || Mode() == MODE_OFF)
		return;
	NetEntity *ne = Find(e);
	if (ne == nil)
		return;
	if (ne->player >= 0)
		Players_EntityDestroyed(e, ne);
	else
		Entities_EntityDestroyed(e, ne);
	Unregister(e);
}

bool
OwnedHere(const CEntity *e)
{
	if (e == nil)
		return false;
	if (e == FindPlayerPed() || e == FindPlayerVehicle())
		return true;
	const NetEntity *ne = Find(e);
	return ne == nil || !ne->puppet;
}
}
#endif
