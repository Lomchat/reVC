// The shared world's social side (docs/mmo.md, section 7; the owner's decisions 4, 8, 9 and 10):
//   - the chat: T (the game's NETWORK_TALK) opens a line at the bottom left, above the radar; Enter sends it, Esc
//     cancels; the last 8 lines stay 15 s (all of them while typing), in the game's fonts, names coloured by player;
//     the server's notices, the kill feed and refusals among them (Social_AddLine for the other modules);
//   - the teleport list: "teleport" typed (as HELP) or sent in the chat opens the game's 17 save points, in HELP's own
//     list (orthros_extras.cpp: its rendering, mouse and keys); the one chosen is reached behind a fade, in the
//     player's vehicle when there is one, whatever the wanted level;
//   - the cheats' gate: only administrators cheat in a shared world; their world cheats go through the world module,
//     slow and fast motion are refused to everyone; HELP keeps working, its list never pausing a shared world.
#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "mmo_internal.h"
#include "orthros_extras.h"
#include "skeleton.h"
#include "Pad.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Text.h"
#include "Radar.h"
#include "Camera.h"
#include "Timer.h"
#include "Game.h"
#include "World.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Vehicle.h"
#include "Streaming.h"
#include "Timecycle.h"
#include "CarCtrl.h"
#include "Rubbish.h"
#include "Script.h"

namespace OrthrosMmo
{
// The player's controls while the chat is open or a teleport is under way (the HELP list's are
// OrthrosExtras::PLAYERCONTROL_ORTHROS_LIST).
enum { PLAYERCONTROL_MMO_SOCIAL = 0x2000 };
enum { NUM_LINES = 8, LINE_BYTES = 256, MAX_CHAT = 120 };
static const double LINE_MS = 15000.0, FADE_MS = 1000.0;

static void OpenTeleportList(void);

// ---------------------------------------------------------------- texts
static const char *
Say(const char *en, const char *fr)
{
	return LangFrench() ? fr : en;
}

// (a code point from UTF-8, the pointer moved past it; a stray byte stands for itself, a broken sequence for '?')
static uint32
NextCodePoint(const uint8 *&s)
{
	uint32 c = *s++;
	if (c < 0xC0)
		return c;
	const int32 more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
	uint32 cp = c & (0x3F >> more);
	for (int32 i = 0; i < more; i++) {
		if ((*s & 0xC0) != 0x80)
			return '?';
		cp = cp << 6 | (*s++ & 0x3F);
	}
	return cp;
}

// (Latin-1, the chat's bytes on the wire, to UTF-8)
static void
Latin1ToUtf8(const char *in, char *out, int32 size)
{
	int32 n = 0;
	for (const uint8 *s = (const uint8 *)in; *s; s++) {
		if (*s < 0x80) {
			if (n + 1 >= size) break;
			out[n++] = *s;
		} else {
			if (n + 2 >= size) break;
			out[n++] = 0xC0 | *s >> 6;
			out[n++] = 0x80 | (*s & 0x3F);
		}
	}
	out[n] = 0;
}

// (UTF-8 to Latin-1, what cannot be sent left out)
static void
Utf8ToLatin1(const char *in, char *out, int32 size)
{
	int32 n = 0;
	for (const uint8 *s = (const uint8 *)in; *s && n < size - 1; ) {
		const uint32 cp = NextCodePoint(s);
		if (cp >= 0x20 && cp <= 0xFF && cp != 0x7F && (cp < 0x80 || cp >= 0xA0))
			out[n++] = (char)cp;
	}
	out[n] = 0;
}

// (a Latin-1 character as the game's fonts print it: GameText's mapping, one character for one)
static wchar
FontChar(uint8 c)
{
	const char utf8[3] = { (char)(c < 0x80 ? c : 0xC0 | c >> 6), (char)(c < 0x80 ? 0 : 0x80 | (c & 0x3F)), 0 };
	wchar out[4];
	GameText(utf8, out, ARRAY_SIZE(out));
	return out[0];
}

// A character the chat accepts: printable ASCII but '~' (the fonts' colour tokens: an unclosed one reads past the
// string), Latin-1 the game's fonts draw (GameText's accents).
static bool
Drawable(uint32 cp)
{
	if (cp < 0x20 || cp == '~' || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp > 0xFF)
		return false;
	if (cp < 0x80)
		return true;
	const wchar c = FontChar((uint8)cp);
	return c != 0 && c != '?';
}

// (a text as the fonts may print it: no '~', no control character; cut whole at size)
static void
Clean(const char *in, char *out, int32 size)
{
	int32 n = 0;
	for (const uint8 *s = (const uint8 *)in; *s; ) {
		const uint8 *from = s;
		uint32 cp = NextCodePoint(s);
		const int32 k = (int32)(s - from);
		if (n + k >= size)
			break;
		if (cp == '~' || cp < 0x20 || cp == 0x7F) {
			out[n++] = cp == '~' ? '-' : ' ';
			continue;
		}
		memcpy(out + n, from, k);
		n += k;
	}
	out[n] = 0;
}

// ---------------------------------------------------------------- the chat's lines
struct Line
{
	int32 kind, slot;
	double time;
	char name[24];
	char text[LINE_BYTES]; // UTF-8
};
static Line lines[NUM_LINES]; // (oldest first)
static int32 numLines;
static char lastNotice[LINE_BYTES];

static void
AddLine(int32 kind, int32 slot, const char *name, const char *utf8)
{
	if (numLines == NUM_LINES) {
		memmove(&lines[0], &lines[1], (NUM_LINES - 1) * sizeof(Line));
		numLines--;
	}
	Line &line = lines[numLines++];
	line.kind = kind;
	line.slot = slot;
	line.time = Now();
	Clean(name ? name : "", line.name, sizeof(line.name));
	Clean(utf8 ? utf8 : "", line.text, sizeof(line.text));
	if (kind != LINE_CHAT)
		strcpy(lastNotice, line.text);
}

static void
Notice(int32 kind, const char *en, const char *fr)
{
	AddLine(kind, NO_SLOT, "", Say(en, fr));
}

void
Social_AddLine(int32 kind, const char *text)
{
	AddLine(kind, NO_SLOT, "", text);
}

void
Social_OnChat(int32 from, int32 kind, const char *name, const char *text)
{
	char utf8[LINE_BYTES];
	Latin1ToUtf8(text, utf8, sizeof(utf8));
	AddLine(kind == 0 ? LINE_CHAT : LINE_NOTICE, from, name, utf8);
}

void
Social_OnDenied(int32 type, int32 reason)
{
	if (reason == 1)
		Notice(LINE_ERROR, "Cheats are reserved to administrators", "Codes réservés aux administrateurs");
	else if (type == C_CHAT && reason == 2)
		Notice(LINE_ERROR, "Not so fast: one message a second", "Pas si vite : un message par seconde");
	else if (type == C_CHAT)
		Notice(LINE_ERROR, "Message refused", "Message refusé");
	else if (type == C_WORLD_SET || type == C_WORLD_EVENT)
		Notice(LINE_ERROR, "Refused by the server", "Refusé par le serveur");
}

// ---------------------------------------------------------------- the cheats' gate
// (the code typed last, in the game's reversed string — its newest character first)
static bool
Typed(const char *reversed, const char *code)
{
	const int32 n = (int32)strlen(code);
	if (n > (int32)ARRAY_SIZE(CPad::KeyBoardCheatString) - 1)
		return false;
	for (int32 i = 0; i < n; i++)
		if (reversed[i] != code[n - 1 - i])
			return false;
	return true;
}

// (the longest of HELP's codes just typed; nil if none)
static const char *
TypedCheat(const char *reversed)
{
	const char *found = nil;
	for (int32 i = 0; i < OrthrosExtras::NumCheats(); i++) {
		const char *code = OrthrosExtras::CheatCode(i);
		if (Typed(reversed, code) && (found == nil || strlen(code) > strlen(found)))
			found = code;
	}
	return found;
}

static bool
IsCheatCode(const char *word)
{
	for (int32 i = 0; i < OrthrosExtras::NumCheats(); i++)
		if (strcmp(OrthrosExtras::CheatCode(i), word) == 0)
			return true;
	return false;
}

// (a code given to the cheats' reader letter by letter, as typed: through the gate below)
static void
FeedCheat(const char *code)
{
	for (const char *c = code; *c; c++)
		CPad::GetPad(0)->AddToPCCheatString(*c);
}

// In a shared world, every character typed comes here first: "teleport" opens the list for everyone, HELP its list;
// the other players' cheats go no further — not one reaches the game's reader, a code of HELP's list refused aloud —;
// an administrator's slow or fast motion is refused, a world cheat sent to the server (the world module), a personal
// one left to the game.
bool
CheatString(char *reversed)
{
	// (the free mode has "teleport" too: the same list, the same way there)
	if (!IsActive() && OrthrosExtras::bFreeMode && Typed(reversed, "TELEPORT")) {
		reversed[0] = ' ';
		OpenTeleportList();
		return true;
	}
	if (!IsActive())
		return false;
	if (Typed(reversed, "TELEPORT")) {
		reversed[0] = ' ';
		OpenTeleportList();
		return true;
	}
	const char *code = TypedCheat(reversed);
	if (code && strcmp(code, "HELP") == 0) {
		reversed[0] = ' ';
		OrthrosExtras::ToggleCheatList();
		return true;
	}
	if (!IsAdmin()) {
		if (code) {
			reversed[0] = ' ';
			Notice(LINE_ERROR, "Cheats are reserved to administrators", "Codes réservés aux administrateurs");
		}
		return true;
	}
	if (code == nil)
		return false;
	if (strcmp(code, "ONSPEED") == 0 || strcmp(code, "BOOOOOORING") == 0) {
		reversed[0] = ' ';
		Notice(LINE_ERROR, "No slow or fast motion in a shared world", "Ni ralenti ni accéléré dans un monde partagé");
		return true;
	}
	if (World_CheatRequested(code)) {
		reversed[0] = ' ';
		return true;
	}
	return false;
}

bool
PadCheatsBlocked(void)
{
	return IsActive();
}

bool
ListsPause(void)
{
	return !IsActive();
}

// ---------------------------------------------------------------- the teleport list
// The game's 17 save points, where the game puts its player after a save (main.scm's save threads), the Ocean View
// and the Vercetti Estate outside their doors (their save points are in interiors); names and districts are the game's
// own texts.
struct Place { const char *name, *district; float x, y, z, heading; };
static const Place safehouses[] = {
	{ "HOTEL", "BEACH1", 233.5f, -1278.45f, 11.0f, 270.0f },
	{ "TMANS", "STARI", -377.06f, -545.43f, 16.0f, 345.0f },
	{ "STPR_11", "BEACH3", 428.4f, 609.1f, 12.0f, 0.0f },
	{ "STPR_10", "BEACH3", 306.573f, 376.293f, 12.186f, 276.316f },
	{ "STPR_9", "BEACH3", 529.663f, 1272.155f, 16.822f, 131.528f },
	{ "STPR_15", "DTOWN", -833.811f, 1304.07f, 10.513f, 200.446f },
	{ "STPR_14", "DTOWN", -559.9f, 705.4f, 19.8f, 243.0f },
	{ "STPR_12", "BEACH2", 90.781f, -806.121f, 10.335f, 238.861f },
	{ "STPR_13", "BEACH1", 14.457f, -1498.594f, 12.197f, 353.829f },
};
static const Place businesses[] = {
	{ "STPR_1", "BEACH3", 492.0f, -80.6f, 10.7f, 224.5f },
	{ "STPR_8", "BEACH1", 100.93f, -1470.622f, 9.387f, 218.027f },
	{ "STPR_7", "DOCKS", -660.0f, -1477.8f, 13.0f, 248.0f },
	{ "STPR_5", "HAVANA", -1028.3f, -851.9f, 12.3f, 226.0f },
	{ "STPR_4", "HAVANA", -872.0f, -573.4f, 10.4f, 277.0f },
	{ "STPR_2", "HAVANA", -1056.7f, -279.7f, 10.6f, 273.0f },
	{ "STPR_6", "HAITI", -996.5f, 194.1f, 10.6f, 80.0f },
	{ "STPR_3", "PORNI", 1.4f, 960.7f, 10.2f, 274.3f },
};
static const struct { const char *en, *fr; const Place *places; int32 count; } placeGroups[] = {
	{ "Safehouses", "Planques", safehouses, ARRAY_SIZE(safehouses) },
	{ "Businesses", "Commerces", businesses, ARRAY_SIZE(businesses) },
};

// (a place by its index: the list's rows counted in order — "teleport <index>")
static const Place *
PlaceAt(int32 index)
{
	for (int32 g = 0; g < (int32)ARRAY_SIZE(placeGroups); g++) {
		if (index >= 0 && index < placeGroups[g].count)
			return &placeGroups[g].places[index];
		index -= placeGroups[g].count;
	}
	return nil;
}

static void
CopyText(const wchar *from, wchar *to, int32 size)
{
	int32 n = 0;
	for (; from && from[n] && n < size - 1; n++)
		to[n] = from[n];
	to[n] = 0;
}

static int
TeleportGroup(int group, wchar *heading, int size)
{
	GameText(Say(placeGroups[group].en, placeGroups[group].fr), heading, size);
	return placeGroups[group].count;
}

static void
TeleportRow(int group, int row, wchar *name, wchar *desc, int size)
{
	const Place &place = placeGroups[group].places[row];
	CopyText(TheText.Get(place.name), name, size);
	CopyText(TheText.Get(place.district), desc, size);
}

static void StartTeleport(const Place *place);

static void
TeleportChosen(int group, int row)
{
	StartTeleport(&placeGroups[group].places[row]);
}

static const OrthrosExtras::List teleportList = {
	{ "TELEPORT", "TÉLÉPORTATION" },
	{ "Click a place, or choose it with the arrows and Enter  -  Esc: close",
	  "Cliquez sur un lieu, ou choisissez-le avec les flèches et Entrée  -  Echap : fermer" },
	ARRAY_SIZE(placeGroups), true, 1.4f, 300.0f, TeleportGroup, TeleportRow, TeleportChosen,
};

// ---------------------------------------------------------------- the teleport
enum { TP_NONE, TP_FADE_OUT, TP_FADE_IN };
static int32 teleportState;
static const Place *teleportTo;
static double teleportSince;
static const float TELEPORT_FADE = 0.5f;
static const double TELEPORT_WAIT_MS = 1500.0; // (a fade that never ends: the teleport goes on all the same)

static bool
Wasted(void)
{
	CPlayerPed *ped = FindPlayerPed();
	return ped == nil || ped->DyingOrDead() || CWorld::Players[0].m_WBState != WBSTATE_PLAYING;
}

// (the player can be moved now; else why not, told)
static bool
MayTeleport(bool tell)
{
	CPlayerPed *ped = FindPlayerPed();
	const char *en = nil, *fr = nil;
	if (Wasted()) {
		en = "Not while wasted or busted";
		fr = "Pas pendant une mort ou une arrestation";
	} else if (ped->EnteringCar() || ped->GetPedState() == PED_EXIT_CAR || ped->GetPedState() == PED_DRAG_FROM_CAR) {
		en = "Not while getting in or out of a vehicle";
		fr = "Pas en montant ou en descendant d'un véhicule";
	} else if (ped->InVehicle() && !OwnedHere(ped->m_pMyVehicle)) {
		en = "Get out of this vehicle first";
		fr = "Descendez d'abord de ce véhicule";
	}
	if (en && tell)
		Notice(LINE_ERROR, en, fr);
	return en == nil;
}

static void
OpenTeleportList(void)
{
	if (teleportState != TP_NONE)
		return;
	OrthrosExtras::OpenList(&teleportList);
}

static void
StartTeleport(const Place *place)
{
	if (place == nil || teleportState != TP_NONE || !MayTeleport(true))
		return;
	teleportTo = place;
	teleportState = TP_FADE_OUT;
	teleportSince = Now();
	TheCamera.SetFadeColour(0, 0, 0);
	TheCamera.Fade(TELEPORT_FADE, FADE_OUT);
}

// The player out of any interior, as the interiors thread ("SHIT") takes them out (its subroutines at main.scm 132277
// and 132298): no interior colours, the rubbish back, the main map shown; and every interior's "inside" flag cleared,
// else the thread believes the player is still in and that interior's door stays shut (this main.scm's variables).
void
Social_LeaveInterior(void)
{
	static const int32 insideFlags[] = { 985, 986, 987, 988, 989, 990, 991, 1002, 1003, 1088 };
	CTimeCycle::StopExtraColour(false);
	CCarCtrl::CarDensityMultiplier = 1.0f;
	CRubbish::SetVisibility(true);
	CGame::currArea = AREA_MAIN_MAP;
	CStreaming::RemoveBuildingsNotInArea(AREA_MAIN_MAP);
	if (CTheScripts::MainScriptSize == MAIN_SCRIPT_SIZE)
		for (int32 i = 0; i < (int32)ARRAY_SIZE(insideFlags); i++)
			*(int32 *)&CTheScripts::ScriptSpace[insideFlags[i] * 4] = 0;
}

// (behind the black: the scene loaded there — the clock held meanwhile, once —, then the player or its vehicle put
// down on it, facing the game's way, the camera behind)
static void
MovePlayer(const Place &place)
{
	CPlayerPed *ped = FindPlayerPed();
	CVector pos(place.x, place.y, place.z);
	const float heading = DEGTORAD(place.heading);
	Social_LeaveInterior();
	CTimer::Suspend();
	CStreaming::LoadScene(pos);
	CStreaming::LoadSceneCollision(pos);
	CTimer::Resume();
	if (ped->InVehicle()) {
		CVehicle *veh = ped->m_pMyVehicle;
		pos.z += veh->GetDistanceFromCentreOfMassToBaseOfModel();
		veh->Teleport(pos);
		veh->SetHeading(heading);
		veh->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		veh->SetTurnSpeed(0.0f, 0.0f, 0.0f);
		CTheScripts::ClearSpaceForMissionEntity(pos, veh);
	} else {
		pos.z += ped->GetDistanceFromCentreOfMassToBaseOfModel();
		ped->Teleport(pos);
		CTheScripts::ClearSpaceForMissionEntity(pos, ped);
		ped->m_fRotationCur = ped->m_fRotationDest = heading;
		ped->SetHeading(heading);
	}
	TheCamera.RestoreWithJumpCut();
	if (!ped->InVehicle())
		TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
}

static void
ProcessTeleport(void)
{
	switch (teleportState) {
	case TP_FADE_OUT:
		if (TheCamera.GetScreenFadeStatus() != FADE_2 && Now() - teleportSince < TELEPORT_WAIT_MS)
			return;
		if (Wasted()) {
			// (wasted or busted meanwhile: the game's own fades take over)
			teleportState = TP_NONE;
			return;
		}
		if (MayTeleport(false))
			MovePlayer(*teleportTo);
		teleportState = TP_FADE_IN;
		teleportSince = Now();
		TheCamera.Fade(TELEPORT_FADE, FADE_IN);
		return;
	case TP_FADE_IN:
		if (TheCamera.GetScreenFadeStatus() != FADE_0 && Now() - teleportSince < TELEPORT_WAIT_MS)
			return;
		teleportState = TP_NONE;
		return;
	}
}

// ---------------------------------------------------------------- the chat's input
static bool chatOpen;
static char input[MAX_CHAT + 1]; // Latin-1, as sent
static int32 inputLength, caret;
static int32 openKey; // (the letter that opened it: its key's repeats, while still held, are not text)
static bool controlsOff;

static void
OpenChat(void)
{
	chatOpen = true;
	input[0] = 0;
	inputLength = caret = 0;
	const int32 key = ControlsManager.GetControllerKeyAssociatedWithAction(NETWORK_TALK, KEYBOARD);
	openKey = key >= 'A' && key <= 'Z' ? key : 0;
}

static void
CloseChat(void)
{
	chatOpen = false;
}

// (controls off while typing or teleporting; again each frame: a respawn gives every one of them back)
static void
UpdateControls(void)
{
	const bool off = chatOpen || teleportState != TP_NONE;
	CPad *pad = CPad::GetPad(0);
	if (off)
		pad->SetDisablePlayerControls(PLAYERCONTROL_MMO_SOCIAL);
	else if (controlsOff)
		pad->SetEnablePlayerControls(PLAYERCONTROL_MMO_SOCIAL);
	controlsOff = off;
}

// (a line typed or a test's "say": "teleport" opens the list; an administrator's cheat code is typed for the cheats' reader — in a
// shared world T opens the chat, so a code with a T in it is typed here; one begun with the chat's own letter lost that
// letter to it, put back —; anything else is said to everyone)
static void
Submit(const char *latin1)
{
	char text[MAX_CHAT + 1];
	int32 n = 0;
	const char *s = latin1;
	while (*s == ' ')
		s++;
	for (; *s && n < MAX_CHAT; s++)
		text[n++] = *s;
	while (n > 0 && text[n - 1] == ' ')
		n--;
	text[n] = 0;
	if (n == 0)
		return;

	char word[32];
	int32 k = 0;
	bool letters = true, spaces = false;
	for (s = text; *s && letters; s++) {
		if (*s == ' ') {
			spaces = true;
			continue;
		}
		if (k >= (int32)sizeof(word) - 2 || !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')))
			letters = false;
		else
			word[k++] = toupper(*s);
	}
	word[k] = 0;
	if (letters && k > 0) {
		// (the chat's letter put back on one word only, as a code typed from the game is: "hugs tools" stays a line)
		char prefixed[34];
		snprintf(prefixed, sizeof(prefixed), "%c%s", openKey ? openKey : ' ', word);
		const char *candidates[2] = { word, openKey && !spaces ? prefixed : nil };
		for (int32 i = 0; i < 2; i++) {
			if (candidates[i] == nil)
				continue;
			if (strcmp(candidates[i], "TELEPORT") == 0) {
				OpenTeleportList();
				return;
			}
			// (codes are the administrators' only: anyone else's line that spells one is said, not refused and lost)
			if (IsAdmin() && IsCheatCode(candidates[i])) {
				FeedCheat(candidates[i]);
				return;
			}
		}
	}
	Out out(C_CHAT);
	out.Str(text);
}

void
Social_TextInput(const char *utf8)
{
	if (!chatOpen)
		return;
	for (const uint8 *s = (const uint8 *)utf8; *s; ) {
		const uint32 cp = NextCodePoint(s);
		if (openKey && cp < 0x80 && toupper(cp) == openKey && CPad::TempKeyState.VK_KEYS[openKey])
			continue;
		if (!Drawable(cp) || inputLength >= MAX_CHAT)
			continue;
		memmove(input + caret + 1, input + caret, inputLength - caret + 1);
		input[caret++] = (char)cp;
		inputLength++;
	}
}

bool
Social_KeyDown(int32 key)
{
	if (!chatOpen)
		return false;
	switch (key) {
	case rsESC:
		CloseChat();
		break;
	case rsENTER:
	case rsPADENTER: {
		char line[MAX_CHAT + 1];
		strcpy(line, input);
		CloseChat();
		Submit(line);
		break;
	}
	case rsBACKSP:
		if (caret > 0) {
			memmove(input + caret - 1, input + caret, inputLength - caret + 1);
			caret--;
			inputLength--;
		}
		break;
	case rsDEL:
		if (caret < inputLength) {
			memmove(input + caret, input + caret + 1, inputLength - caret);
			inputLength--;
		}
		break;
	case rsLEFT: if (caret > 0) caret--; break;
	case rsRIGHT: if (caret < inputLength) caret++; break;
	case rsHOME: caret = 0; break;
	case rsEND: caret = inputLength; break;
	}
	return true;
}

void
Social_BeginProcess(void)
{
	if (!IsActive()) {
		CloseChat();
		// (a free game's teleport goes on: its fades, its scene)
		if (!OrthrosExtras::bFreeMode)
			teleportState = TP_NONE;
		ProcessTeleport();
		UpdateControls();
		return;
	}
	CPad *pad = CPad::GetPad(0);
	if (FrontEndMenuManager.m_bMenuActive)
		CloseChat();
	else if (!chatOpen && pad->NewState.NetworkTalk && !pad->OldState.NetworkTalk && !OrthrosExtras::IsCheatListOpen() &&
	         teleportState == TP_NONE)
		OpenChat();
	ProcessTeleport();
	UpdateControls();
}

void
Social_Reset(void)
{
	CloseChat();
	numLines = 0;
	lastNotice[0] = 0;
	if (OrthrosExtras::IsListOpen(&teleportList))
		OrthrosExtras::CloseList();
	if (teleportState == TP_FADE_OUT)
		TheCamera.Fade(TELEPORT_FADE, FADE_IN);
	teleportState = TP_NONE;
	UpdateControls();
}

// ---------------------------------------------------------------- the chat drawn
// (sizes in the game's 640 × 448 screen units: the chat's left edge on the radar's, its bottom above it)
static const float CHAT_SCALE_X = 0.36f, CHAT_SCALE_Y = 0.76f, CHAT_WIDTH = 300.0f, CHAT_INDENT = 8.0f, CHAT_ABOVE_RADAR = 10.0f;
enum { MAX_ROWS = 6 };

static CRGBA
NameColour(int32 slot)
{
	static const CRGBA colours[] = {
		CRGBA(255, 150, 225, 255), CRGBA(97, 194, 247, 255), CRGBA(0, 207, 133, 255), CRGBA(255, 180, 70, 255),
		CRGBA(190, 160, 255, 255), CRGBA(255, 236, 110, 255), CRGBA(110, 225, 215, 255), CRGBA(255, 125, 110, 255),
	};
	return colours[(slot & 0xFF) % ARRAY_SIZE(colours)];
}

static CRGBA
TextColour(int32 kind)
{
	switch (kind) {
	case LINE_NOTICE: return CRGBA(190, 190, 190, 255);
	case LINE_KILL: return CRGBA(255, 160, 60, 255);
	case LINE_ERROR: return CRGBA(240, 80, 80, 255);
	default: return CRGBA(255, 255, 255, 255);
	}
}

static float
CharWidth(wchar c)
{
	wchar s[2] = { c, 0 };
	return CFont::GetStringWidth(s, true);
}

// (a text cut into the rows it takes — the first one narrower, after the name —, at spaces; a word longer than a row
// cut where it reaches the edge; each row's start and end)
static int32
WrapRows(const wchar *text, const float *widths, int32 length, float firstWidth, float width, int32 *starts, int32 *ends)
{
	int32 n = 0, start = 0;
	while (start < length && n < MAX_ROWS) {
		const float room = n == 0 ? firstWidth : width;
		float x = 0.0f;
		int32 end = start, space = -1;
		while (end < length && (end == start || x + widths[end] <= room)) {
			if (text[end] == ' ')
				space = end;
			x += widths[end++];
		}
		if (end < length && space > start)
			end = space;
		starts[n] = start;
		ends[n++] = end;
		for (start = end; start < length && text[start] == ' '; start++);
	}
	if (n == 0) {
		starts[0] = ends[0] = 0;
		n = 1;
	}
	return n;
}

// (a part of a text printed; one starting with '*' would print nothing — the fonts' rule —: a space before it)
static void
PrintPart(float x, float y, const wchar *text, int32 from, int32 to)
{
	wchar part[LINE_BYTES + 2];
	int32 n = 0;
	if (from < to && text[from] == '*')
		part[n++] = ' ';
	for (int32 i = from; i < to && text[i] && n < (int32)ARRAY_SIZE(part) - 1; i++)
		part[n++] = text[i];
	part[n] = 0;
	CFont::PrintString(x, y, part);
}

struct LineLayout
{
	wchar name[32], text[LINE_BYTES];
	float nameWidth;
	int32 rows, starts[MAX_ROWS], ends[MAX_ROWS];
};

static void
Layout(const Line &line, float width, LineLayout &out)
{
	char name[40];
	out.name[0] = 0;
	out.nameWidth = 0.0f;
	if (line.name[0]) {
		snprintf(name, sizeof(name), "%s: ", line.name);
		GameText(name, out.name, ARRAY_SIZE(out.name));
		out.nameWidth = CFont::GetStringWidth(out.name, true);
	}
	GameText(line.text, out.text, ARRAY_SIZE(out.text));
	float widths[LINE_BYTES];
	int32 length = 0;
	for (; out.text[length]; length++)
		widths[length] = CharWidth(out.text[length]);
	out.rows = WrapRows(out.text, widths, length, width - out.nameWidth, width - SCREEN_SCALE_X(CHAT_INDENT), out.starts, out.ends);
}

void
Social_RenderOverlay(void)
{
	if (!IsActive() || FrontEndMenuManager.m_bMenuActive)
		return;
	const double now = Now();
	int32 first = numLines;
	while (first > 0 && (chatOpen || now - lines[first - 1].time < LINE_MS))
		first--;
	if (first == numLines && !chatOpen)
		return;

	// (what the frame printed so far drawn now: the chat's background goes over it, the chat's text over that)
	CFont::DrawFonts();
	CFont::SetBackgroundOff();
	CFont::SetPropOn();
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetWrapx(SCREEN_WIDTH * 4.0f);
	CFont::SetAlphaFade(255.0f);
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(SCREEN_SCALE_X(CHAT_SCALE_X), SCREEN_SCALE_Y(CHAT_SCALE_Y));
	CFont::SetDropShadowPosition(1);

	const float left = SCREEN_SCALE_X(RADAR_LEFT), width = SCREEN_SCALE_X(CHAT_WIDTH);
	const float rowHeight = 18.0f * SCREEN_SCALE_Y(CHAT_SCALE_Y); // (the fonts' own line feed)
	const float bottom = SCREEN_SCALE_FROM_BOTTOM(RADAR_BOTTOM + RADAR_HEIGHT + CHAT_ABOVE_RADAR);
	const float pad = SCREEN_SCALE_X(4.0f);
	static LineLayout layouts[NUM_LINES];
	int32 rows = 0;
	for (int32 i = first; i < numLines; i++) {
		Layout(lines[i], width, layouts[i]);
		rows += layouts[i].rows;
	}
	// (while typing: the input line on a darker bar at the bottom, the history on a lighter one above it)
	const float inputTop = bottom - rowHeight;
	float y = (chatOpen ? inputTop - pad * 2.0f : bottom) - rows * rowHeight;
	if (chatOpen) {
		if (rows > 0)
			CSprite2d::DrawRect(CRect(left - pad, y - pad, left + width + pad, inputTop - pad), CRGBA(0, 0, 0, 110));
		CSprite2d::DrawRect(CRect(left - pad, inputTop - pad, left + width + pad, bottom + pad), CRGBA(0, 0, 0, 170));
	}

	for (int32 i = first; i < numLines; i++) {
		const Line &line = lines[i];
		const LineLayout &l = layouts[i];
		// (fading out over its last second, unless the chat is open)
		const double remaining = LINE_MS - (now - line.time);
		const uint8 alpha = chatOpen || remaining >= FADE_MS ? 255 : (uint8)Max(0.0, 255.0 * remaining / FADE_MS);
		CFont::SetDropColor(CRGBA(0, 0, 0, alpha));
		CRGBA colour = line.kind == LINE_CHAT ? NameColour(line.slot) : TextColour(line.kind);
		colour.a = alpha;
		if (l.name[0]) {
			CFont::SetColor(colour);
			PrintPart(left, y, l.name, 0, 1024);
		}
		colour = TextColour(line.kind);
		colour.a = alpha;
		CFont::SetColor(colour);
		for (int32 r = 0; r < l.rows; r++)
			PrintPart(r == 0 ? left + l.nameWidth : left + SCREEN_SCALE_X(CHAT_INDENT), y + r * rowHeight, l.text, l.starts[r], l.ends[r]);
		y += l.rows * rowHeight;
	}

	if (chatOpen) {
		// (the prompt, then as much of the line as fits around the caret)
		wchar prompt[16], text[MAX_CHAT + 1];
		GameText(Say("Say:", "Dire :"), prompt, ARRAY_SIZE(prompt));
		CFont::SetDropColor(CRGBA(0, 0, 0, 255));
		CFont::SetColor(CRGBA(97, 194, 247, 255));
		CFont::PrintString(left, inputTop, prompt);
		const float textLeft = left + CFont::GetStringWidth(prompt, true) + SCREEN_SCALE_X(4.0f), room = left + width - textLeft;
		float widths[MAX_CHAT + 1];
		for (int32 i = 0; i < inputLength; i++) {
			text[i] = FontChar((uint8)input[i]);
			widths[i] = CharWidth(text[i]);
		}
		text[inputLength] = 0;
		int32 from = 0, to = caret;
		float before = 0.0f;
		for (int32 i = 0; i < caret; i++)
			before += widths[i];
		while (from < caret && before > room) // (the caret kept in view)
			before -= widths[from++];
		float shown = before;
		while (to < inputLength && shown + widths[to] <= room)
			shown += widths[to++];
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		PrintPart(textLeft, inputTop, text, from, to);
		if (fmod(now, 1000.0) < 600.0)
			CSprite2d::DrawRect(CRect(textLeft + before, inputTop, textLeft + before + Max(1.0f, SCREEN_SCALE_X(1.0f)), inputTop + rowHeight * 0.9f),
			                    CRGBA(255, 255, 255, 220));
	}
	CFont::DrawFonts();
	CFont::SetDropShadowPosition(0);
}

// ---------------------------------------------------------------- tests and debug
bool
Social_TestCommand(const char *cmd)
{
	if (strncmp(cmd, "say ", 4) == 0) {
		char latin1[MAX_CHAT + 1];
		Utf8ToLatin1(cmd + 4, latin1, sizeof(latin1));
		Submit(latin1);
		return true;
	}
	if (strcmp(cmd, "teleport") == 0) {
		OpenTeleportList();
		return true;
	}
	if (strncmp(cmd, "teleport ", 9) == 0) {
		const Place *place = PlaceAt(atoi(cmd + 9));
		if (place)
			StartTeleport(place);
		else
			Notice(LINE_ERROR, "No such place", "Lieu inconnu");
		return true;
	}
	if (strncmp(cmd, "cheat ", 6) == 0) {
		char code[32];
		int32 n = 0;
		for (const char *s = cmd + 6; *s && n < (int32)sizeof(code) - 1; s++)
			if (*s != ' ')
				code[n++] = toupper((uint8)*s);
		code[n] = 0;
		FeedCheat(code);
		return true;
	}
	if (strcmp(cmd, "chatopen") == 0) {
		if (IsActive() && !chatOpen)
			OpenChat();
		return true;
	}
	return false;
}

// (a JSON string's inside: quotes, backslashes and control characters escaped; UTF-8 kept)
static int32
JsonEscape(char *out, int32 size, const char *s)
{
	int32 n = 0;
	for (; *s && n < size - 7; s++) {
		const uint8 c = *s;
		if (c == '"' || c == '\\') {
			out[n++] = '\\';
			out[n++] = c;
		} else if (c < 0x20)
			n += sprintf(out + n, "\\u%04x", c);
		else
			out[n++] = c;
	}
	out[n] = 0;
	return n;
}

void
Social_Debug(void)
{
	static char chat[NUM_LINES * ((LINE_BYTES + 32) * 2 + 8) + 16];
	char line[LINE_BYTES + 32], notice[LINE_BYTES * 2];
	int32 n = 0;
	chat[n++] = '[';
	for (int32 i = 0; i < numLines; i++) {
		if (lines[i].name[0])
			snprintf(line, sizeof(line), "%s: %s", lines[i].name, lines[i].text);
		else
			snprintf(line, sizeof(line), "%s", lines[i].text);
		if (i > 0)
			chat[n++] = ',';
		chat[n++] = '"';
		n += JsonEscape(chat + n, sizeof(chat) - n - 4, line);
		chat[n++] = '"';
	}
	chat[n++] = ']';
	chat[n] = 0;
	JsonEscape(notice, sizeof(notice), lastNotice);
	DebugAppend(",\"chat\":%s,\"chatOpen\":%s,\"teleportList\":%s,\"teleporting\":%s,\"notice\":\"%s\"", chat,
	            chatOpen ? "true" : "false", OrthrosExtras::IsListOpen(&teleportList) ? "true" : "false",
	            teleportState != TP_NONE ? "true" : "false", notice);
	// (a free game's player, for its teleport's checks: the shared world's session tells its own)
	if (!IsActive() && OrthrosExtras::bFreeMode && FindPlayerPed()) {
		const CVector &p = FindPlayerPed()->GetPosition();
		DebugAppend(",\"freeMode\":{\"pos\":[%.2f,%.2f,%.2f],\"health\":%.1f}", p.x, p.y, p.z, FindPlayerPed()->m_fHealth);
	}
}
}
#endif
