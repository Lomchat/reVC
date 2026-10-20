#include "common.h"

#ifdef ORTHROS_EXTRAS
#include "orthros_extras.h"
#include "crossplatform.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Frontend.h"
#include "Pad.h"
#include "Timer.h"
#include "FileMgr.h"
#include "Script.h"
#include "GenericGameStorage.h"
#include "PCSave.h"
#include "frontendoption.h"
#include "Streaming.h"
#include "mmo.h"

const char *_psGetUserFilesFolder();

namespace OrthrosExtras
{
bool bFreeMode;

// ---------------------------------------------------------------- the free mode's saves
// (the save files the game's lists and its saves use now: the free mode's when its page or its game asked for them)
static bool freeSaves;

bool
UsingFreeSaves()
{
	return freeSaves;
}

const char *
SaveFilePrefix()
{
	return freeSaves ? "GTAVCfree" : "GTAVCsf";
}

static void
UseFreeSaves(bool on)
{
	freeSaves = on;
	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());
	PcSaveHelper.PopulateSlotInfo();
}

// ---------------------------------------------------------------- the game's text
// The game's fonts hold the French accented letters at the codes its French text uses (TEXT/french.gxt); an
// accented capital they have not: its letter without the accent.
void
GameText(const char *utf8, wchar *out, int size)
{
	int n = 0;
	for (const uint8 *s = (const uint8 *)utf8; *s && n < size - 1; ) {
		uint32 c = *s++;
		// (a letter in two, three or four bytes; a byte that starts none is taken as Latin-1)
		const int more = c >= 0xC0 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF8 ? 3 : 0;
		int k = 0;
		while (k < more && (s[k] & 0xC0) == 0x80)
			k++;
		if (more > 0 && k == more) {
			c &= 0x3F >> more;
			for (int i = 0; i < more; i++)
				c = (c << 6) | (*s++ & 0x3F);
		}
		switch (c) {
		case 0xE0: c = 0x97; break; // à
		case 0xE2: c = 0x99; break; // â
		case 0xE7: c = 0x9C; break; // ç
		case 0xE8: c = 0x9D; break; // è
		case 0xE9: c = 0x9E; break; // é
		case 0xEA: c = 0x9F; break; // ê
		case 0xEE: c = 0xA3; break; // î
		case 0xEF: c = 0xA4; break; // ï
		case 0xF4: c = 0xA7; break; // ô
		case 0xF9: c = 0xA9; break; // ù
		case 0xFB: c = 0xAB; break; // û
		case 0xC0: case 0xC2: c = 'A'; break;
		case 0xC7: c = 'C'; break;
		case 0xC8: case 0xC9: case 0xCA: c = 'E'; break;
		case 0xCE: c = 'I'; break;
		case 0xD4: c = 'O'; break;
		default: if (c >= 0x80) c = '?';
		}
		out[n++] = (wchar)c;
	}
	out[n] = 0;
}

static bool
French()
{
	return FrontEndMenuManager.m_PrefsLanguage == CMenuManager::LANGUAGE_FRENCH;
}

// (the texts the game's own text files lack: keys "ORT_*", asked for as any of theirs — CText::Get)
wchar *
Text(const char *key)
{
	static const struct { const char *key, *en, *fr; } texts[] = {
		{ "ORT_FRE", "Free Mode", "Mode libre" },
		{ "ORT_LMP", "Leave Multiplayer", "Quitter le multijoueur" },
		{ "ORT_CON", "Continue", "Continuer" },
		{ "ORT_NEW", "New Free Game", "Nouvelle partie libre" },
	};
	static wchar buffers[ARRAY_SIZE(texts)][2][48];
	if (strncmp(key, "ORT_", 4) != 0)
		return nil;
	const int lang = French() ? 1 : 0;
	for (int i = 0; i < (int)ARRAY_SIZE(texts); i++)
		if (strcmp(key, texts[i].key) == 0) {
			if (buffers[i][lang][0] == 0)
				GameText(lang ? texts[i].fr : texts[i].en, buffers[i][lang], ARRAY_SIZE(buffers[i][lang]));
			return buffers[i][lang];
		}
	return nil;
}

// ---------------------------------------------------------------- the cheats, by kind
struct Cheat { const char *code, *en, *fr; };
struct CheatGroup { const char *en, *fr; const Cheat *cheats; int count; };

static const Cheat weapons[] = {
	{ "ASPIRINE", "Full health", "Santé au maximum" },
	{ "ICANTTAKEITANYMORE", "Suicide", "Suicide" },
	{ "NUTTERTOOLS", "Weapons, set 3", "Armes, lot 3" },
	{ "PRECIOUSPROTECTION", "Full armour", "Gilet pare-balles" },
	{ "PROFESSIONALTOOLS", "Weapons, set 2", "Armes, lot 2" },
	{ "THUGSTOOLS", "Weapons, set 1", "Armes, lot 1" },
};
static const Cheat police[] = {
	{ "LEAVEMEALONE", "Wanted level cleared", "Plus recherché" },
	{ "YOUWONTTAKEMEALIVE", "Two more wanted stars", "Deux étoiles de plus" },
};
static const Cheat spawn[] = {
	{ "BETTERTHANWALKING", "Caddy", "Caddy" },
	{ "GETTHEREAMAZINGLYFAST", "Hotring Racer (2)", "Hotring Racer (2)" },
	{ "GETTHEREFAST", "Sabre Turbo", "Sabre Turbo" },
	{ "GETTHEREQUICKLY", "Bloodring Banger (2)", "Bloodring Banger (2)" },
	{ "GETTHEREVERYFASTINDEED", "Hotring Racer", "Hotring Racer" },
	{ "PANZER", "Rhino tank", "Char Rhino" },
	{ "ROCKANDROLLCAR", "Love Fist limousine", "Limousine Love Fist" },
	{ "RUBBISHCAR", "Trashmaster", "Camion poubelle" },
	{ "THELASTRIDE", "Romero's hearse", "Corbillard de Romero" },
	{ "TRAVELINSTYLE", "Bloodring Banger", "Bloodring Banger" },
};
static const Cheat traffic[] = {
	{ "AHAIRDRESSERSCAR", "Pink traffic", "Circulation rose" },
	{ "AIRSHIP", "Boats fly", "Bateaux volants" },
	{ "BIGBANG", "Nearby vehicles blow up", "Véhicules qui explosent" },
	{ "CARSAREHELI", "Cars fly like helicopters", "Voitures hélicoptères" },
	{ "COMEFLYWITHME", "Cars fly", "Voitures volantes" },
	{ "GREENLIGHT", "Lights stay green", "Feux toujours verts" },
	{ "GRIPISEVERYTHING", "Perfect handling", "Tenue de route parfaite" },
	{ "IWANTITPAINTEDBLACK", "Black traffic", "Circulation noire" },
	{ "LOADSOFLITTLETHINGS", "Big wheels, more power", "Grosses roues, plus de puissance" },
	{ "MIAMITRAFFIC", "Aggressive drivers", "Conducteurs agressifs" },
	{ "SEAWAYS", "Cars drive on water", "Voitures sur l'eau" },
	{ "SPIDERCAR", "Cars climb walls", "Voitures grimpeuses" },
	{ "WHEELSAREALLINEED", "Only the wheels show", "Seulement les roues" },
};
static const Cheat weather[] = {
	{ "ABITDRIEG", "Cloudy", "Nuageux" },
	{ "ALOVELYDAY", "Very sunny", "Grand soleil" },
	{ "APLEASANTDAY", "Sunny", "Ensoleillé" },
	{ "BOOOOOORING", "Slow motion", "Ralenti" },
	{ "CANTSEEATHING", "Fog", "Brouillard" },
	{ "CATSANDDOGS", "Rain", "Pluie" },
	{ "LIFEISPASSINGMEBY", "Faster clock", "Horloge accélérée" },
	{ "ONSPEED", "Faster game", "Jeu accéléré" },
};
static const Cheat peds[] = {
	{ "CHICKSWITHGUNS", "Armed girls in bikinis", "Filles en bikini armées" },
	{ "FANNYMAGNET", "Women follow you", "Les femmes vous suivent" },
	{ "FIGHTFIGHTFIGHT", "Riot", "Bagarre générale" },
	{ "HOPINGIRL", "A girl gets in your car", "Une fille monte en voiture" },
	{ "NOBODYLIKESME", "Everyone attacks you", "Tout le monde vous attaque" },
	{ "OURGODGIVENRIGHTTOBEARARMS", "Everyone is armed", "Tout le monde est armé" },
};
static const Cheat tommy[] = {
	{ "CERTAINDEATH", "Cigar", "Cigare" },
	{ "CHEATSHAVEBEENCRACKED", "Ricardo Diaz", "Ricardo Diaz" },
	{ "DEEPFRIEDMARSBARS", "Fat Tommy", "Tommy gros" },
	{ "FOXYLITTLETHING", "Mercedes", "Mercedes" },
	{ "IDONTHAVETHEMONEYSONNY", "Sonny Forelli", "Sonny Forelli" },
	{ "ILOOKLIKEHILARY", "Hilary King", "Hilary King" },
	{ "IWANTBIGTITS", "Candy Suxxx", "Candy Suxxx" },
	{ "LOOKLIKELANCE", "Lance Vance", "Lance Vance" },
	{ "MYSONISALAWYER", "Ken Rosenberg", "Ken Rosenberg" },
	{ "ONEARMEDBANDIT", "Phil Cassidy", "Phil Cassidy" },
	{ "PROGRAMMER", "Skinny Tommy", "Tommy maigre" },
	{ "ROCKANDROLLMAN", "Jezz Torrent", "Jezz Torrent" },
	{ "STILLLIKEDRESSINGUP", "Random look", "Tenue au hasard" },
	{ "WELOVEOURDICK", "Dick (Love Fist)", "Dick (Love Fist)" },
};
static const Cheat other[] = {
	{ "CHASESTAT", "Chase statistics", "Statistiques de poursuite" },
	{ "HELP", "This list", "Cette liste" },
	{ "KANGAROO", "Super jump", "Super saut" },
};
static const CheatGroup groups[] = {
	{ "Weapons and health", "Armes et santé", weapons, ARRAY_SIZE(weapons) },
	{ "Police", "Police", police, ARRAY_SIZE(police) },
	{ "Vehicles", "Véhicules", spawn, ARRAY_SIZE(spawn) },
	{ "Traffic and driving", "Circulation et conduite", traffic, ARRAY_SIZE(traffic) },
	{ "Weather and time", "Météo et temps", weather, ARRAY_SIZE(weather) },
	{ "Pedestrians", "Piétons", peds, ARRAY_SIZE(peds) },
	{ "Tommy", "Tommy", tommy, ARRAY_SIZE(tommy) },
	{ "Other", "Divers", other, ARRAY_SIZE(other) },
};

// ---------------------------------------------------------------- the list over the paused game
static bool listOpen;
// (the mouse over the list — free while it is open — and each code's place as last drawn, for a click)
static float mouseX = -1.0f, mouseY = -1.0f;
static struct { float left, top, right, bottom; const Cheat *cheat; } places[80];
static int numPlaces;
static bool swallowLeft; // (the click that chose a code, not the game's until released)

bool
SwallowLeftButton(bool down)
{
	if (!down)
		swallowLeft = false;
	return swallowLeft;
}

void
SwallowLeftClick()
{
	swallowLeft = true;
}

void
CheatListMouse(float x, float y)
{
	mouseX = x;
	mouseY = y;
}

// (a code clicked: as if typed — the list closed, the game going on again, then its letters given to the cheats'
// reader one by one, the keyboard's way; HELP itself only closes the list)
bool
CheatListClick(float x, float y)
{
	if (!listOpen)
		return false;
	mouseX = x;
	mouseY = y;
	for (int i = 0; i < numPlaces; i++) {
		if (x < places[i].left || x > places[i].right || y < places[i].top || y > places[i].bottom)
			continue;
		const char *code = places[i].cheat->code;
		swallowLeft = true;
		ToggleCheatList();
		if (strcmp(code, "HELP") != 0)
			for (const char *c = code; *c; c++)
				CPad::GetPad(0)->AddToPCCheatString(*c);
		return true;
	}
	return true;
}

bool
IsCheatListOpen()
{
	return listOpen;
}

void
ToggleCheatList()
{
	if (FrontEndMenuManager.m_bMenuActive)
		return;
	listOpen = !listOpen;
	if (listOpen)
		CTimer::StartUserPause();
	else
		CTimer::EndUserPause();
}

void
ProcessCheatList()
{
	if (!listOpen)
		return;
	CPad *pad = CPad::GetPad(0);
	if (pad->GetEscapeJustDown() || pad->GetEnterJustDown() || pad->GetBackspaceJustDown()) {
		// (the key is the list's: the frontend does not open its pause menu on that Esc)
		CPad::OldKeyState.ESC = CPad::NewKeyState.ESC;
		ToggleCheatList();
	}
}

// (sizes in the game's 640 × 448 screen units, as its menus' and HUD's: SCREEN_SCALE_X/Y)
static const float HEAD_SCALE_X = 0.62f, HEAD_SCALE_Y = 1.1f, CODE_SCALE_X = 0.36f, CODE_SCALE_Y = 0.72f, DESC_SCALE_X = 0.32f, DESC_SCALE_Y = 0.66f;
static const float HEAD_ROW = 21.0f, CHEAT_ROW = 13.5f, GROUP_GAP = 7.0f, TOP = 64.0f, BOTTOM = 30.0f;

static float
GroupHeight(const CheatGroup &g)
{
	return HEAD_ROW + g.count * CHEAT_ROW;
}

void
RenderCheatList()
{
	if (!listOpen)
		return;
	if (FrontEndMenuManager.m_bMenuActive) { // (the pause menu opened over it: the list goes)
		listOpen = false;
		CTimer::EndUserPause();
		return;
	}
	const bool fr = French();
	wchar text[128];
	const CRGBA pink(255, 150, 225, 255), cyan(97, 194, 247, 255), white(255, 255, 255, 255), grey(185, 185, 185, 255), black(0, 0, 0, 255);
	numPlaces = 0;

	CSprite2d::DrawRect(CRect(0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT), CRGBA(0, 0, 0, 205));
	CFont::SetBackgroundOff();
	CFont::SetPropOn();
	CFont::SetJustifyOff();
	CFont::SetCentreOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetRightJustifyWrap(0.0f);

	// (the title as the menus' pages have theirs: top right, the heading font, its shadow behind)
	GameText("CHEATS", text, ARRAY_SIZE(text));
	CFont::SetRightJustifyOn();
	CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
	CFont::SetScale(SCREEN_SCALE_X(1.0f), SCREEN_SCALE_Y(2.0f));
	CFont::SetDropShadowPosition(0);
	CFont::SetColor(CRGBA(30, 30, 30, 255));
	CFont::PrintString(SCREEN_STRETCH_FROM_RIGHT(10.0f) - SCREEN_SCALE_X(7.0f), SCREEN_SCALE_Y(17.0f), text);
	CFont::SetColor(pink);
	CFont::PrintString(SCREEN_STRETCH_FROM_RIGHT(10.0f), SCREEN_SCALE_Y(10.0f), text);
	CFont::SetRightJustifyOff();

	// (the groups in columns, in order, the tallest column as short as can be; everything scaled down to fit)
	const int numGroups = ARRAY_SIZE(groups);
	const int columns = SCREEN_WIDTH >= SCREEN_HEIGHT * 1.45f ? 3 : 2;
	int split[2] = { numGroups, numGroups };
	float best = 1e9f;
	for (int a = 1; a < numGroups; a++)
		for (int b = columns == 3 ? a + 1 : numGroups; b <= numGroups; b++) {
			if (columns == 3 && b >= numGroups) continue;
			float h[3] = { 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < numGroups; i++) {
				int c = i < a ? 0 : i < b ? 1 : 2;
				h[c] += GroupHeight(groups[i]) + (h[c] > 0.0f ? GROUP_GAP : 0.0f);
			}
			float tallest = Max(h[0], Max(h[1], h[2]));
			if (tallest < best) { best = tallest; split[0] = a; split[1] = b; }
		}
	const float available = DEFAULT_SCREEN_HEIGHT - TOP - BOTTOM;
	const float f = best > available ? available / best : 1.0f;

	const float margin = SCREEN_STRETCH_X(28.0f), gutter = SCREEN_STRETCH_X(18.0f);
	const float columnWidth = (SCREEN_WIDTH - 2.0f * margin - (columns - 1) * gutter) / columns;
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(black);
	for (int c = 0; c < columns; c++) {
		const float left = margin + c * (columnWidth + gutter), right = left + columnWidth;
		float y = TOP;
		const int from = c == 0 ? 0 : split[c - 1], to = c == columns - 1 ? numGroups : split[c];
		for (int i = from; i < to; i++) {
			const CheatGroup &g = groups[i];
			GameText(fr ? g.fr : g.en, text, ARRAY_SIZE(text));
			CFont::SetFontStyle(FONT_LOCALE(FONT_HEADING));
			CFont::SetScale(SCREEN_SCALE_X(HEAD_SCALE_X * f), SCREEN_SCALE_Y(HEAD_SCALE_Y * f));
			// (a title wider than its column narrowed to it)
			const float headWidth = CFont::GetStringWidth(text, true);
			if (headWidth > columnWidth)
				CFont::SetScale(SCREEN_SCALE_X(HEAD_SCALE_X * f) * columnWidth / headWidth, SCREEN_SCALE_Y(HEAD_SCALE_Y * f));
			CFont::SetColor(cyan);
			CFont::PrintString(left, SCREEN_SCALE_Y(y), text);
			CSprite2d::DrawRect(CRect(left, SCREEN_SCALE_Y(y + HEAD_ROW * f - 3.0f * f), right, SCREEN_SCALE_Y(y + HEAD_ROW * f - 3.0f * f) + Max(1.0f, SCREEN_SCALE_Y(1.0f))), CRGBA(97, 194, 247, 110));
			y += HEAD_ROW * f;
			for (int k = 0; k < g.count; k++) {
				const Cheat &cheat = g.cheats[k];
				const float rowY = SCREEN_SCALE_Y(y), space = SCREEN_STRETCH_X(8.0f);
				wchar desc[128];
				GameText(fr ? cheat.fr : cheat.en, desc, ARRAY_SIZE(desc));
				const float descX = SCREEN_SCALE_X(DESC_SCALE_X * f), descY = SCREEN_SCALE_Y(DESC_SCALE_Y * f);
				CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
				CFont::SetScale(descX, descY);
				const float descWidth = CFont::GetStringWidth(desc, true);
				// (a code and its description on one line: the description narrower if they would meet — down to 70 % —,
				// then the code too, down to 80 %)
				float codeX = SCREEN_SCALE_X(CODE_SCALE_X * f);
				const float codeY = SCREEN_SCALE_Y(CODE_SCALE_Y * f);
				GameText(cheat.code, text, ARRAY_SIZE(text));
				CFont::SetScale(codeX, codeY);
				float codeWidth = CFont::GetStringWidth(text, true);
				float squeeze = Min(1.0f, (columnWidth - codeWidth - space) / descWidth);
				if (squeeze < 0.7f) {
					codeX *= Max(0.8f, (columnWidth - space - descWidth * 0.7f) / codeWidth);
					CFont::SetScale(codeX, codeY);
					codeWidth = CFont::GetStringWidth(text, true);
					squeeze = Max(0.5f, Min(1.0f, (columnWidth - codeWidth - space) / descWidth));
				}
				// (its place, for a click; under the mouse, the menus' selection behind it)
				const float pad = SCREEN_STRETCH_X(4.0f), top = rowY - SCREEN_SCALE_Y(1.0f), bottom = rowY + SCREEN_SCALE_Y(CHEAT_ROW * f) - SCREEN_SCALE_Y(1.0f);
				if (numPlaces < (int)ARRAY_SIZE(places)) {
					places[numPlaces].left = left - pad; places[numPlaces].right = right + pad;
					places[numPlaces].top = top; places[numPlaces].bottom = bottom;
					places[numPlaces].cheat = &cheat;
					numPlaces++;
				}
				if (mouseX >= left - pad && mouseX <= right + pad && mouseY >= top && mouseY <= bottom)
					CSprite2d::DrawRect(CRect(left - pad, top, right + pad, bottom), CRGBA(25, 130, 70, 255));
				CFont::SetColor(white);
				CFont::PrintString(left, rowY, text);
				// (its description right-aligned)
				memcpy(text, desc, sizeof(desc));
				CFont::SetScale(descX * squeeze, descY);
				CFont::SetRightJustifyOn();
				CFont::SetColor(pink);
				CFont::PrintString(right, rowY + SCREEN_SCALE_Y((CODE_SCALE_Y - DESC_SCALE_Y) * f * 9.0f), text);
				CFont::SetRightJustifyOff();
				y += CHEAT_ROW * f;
			}
			y += GROUP_GAP * f;
		}
	}

	// (how to use it, at the bottom)
	GameText(fr ? "Cliquez sur un code, ou tapez-le en jouant  -  HELP, Entrée ou Echap : fermer"
	            : "Click a code, or type it while playing  -  HELP, Enter or Esc: close", text, ARRAY_SIZE(text));
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	CFont::SetScale(SCREEN_SCALE_X(0.34f), SCREEN_SCALE_Y(0.7f));
	CFont::SetCentreOn();
	CFont::SetCentreSize(SCREEN_WIDTH);
	CFont::SetColor(grey);
	CFont::PrintString(SCREEN_WIDTH / 2.0f, SCREEN_SCALE_FROM_BOTTOM(22.0f), text);
	CFont::SetCentreOff();
	CFont::SetDropShadowPosition(0);
}

// ---------------------------------------------------------------- the free mode
// A game of its own, chosen in the main menu ("Free Mode": its page, MENUPAGE_FREE_MODE): "Continue" starts it on the
// most recent of its saves, "New Free Game" on a game completed at 100 % found among the game's files
// (orthros/freemode.b, loaded as it is), "Load Game" and "Delete Game" are the game's own pages on its saves. Its games
// save anywhere from the pause menu.
static const char *FREE_MODE_START = "orthros\\freemode.b";
static bool freeModeStart; // (the next load: that game, not a slot)
// (that game as the shared world's base — OrthrosMmo's session: the next load is it; the game being played is it)
static bool sharedStart, sharedGame;

// (when a slot's game was saved: its file's header, as PopulateSlotInfo reads it)
static bool
SlotSaveTime(int slot, SYSTEMTIME *time)
{
	struct {
		int size;
		wchar FileName[24];
		SYSTEMTIME SaveDateTime;
	} header;
	MakeValidSaveName(slot);
	int file = CFileMgr::OpenFile(ValidSaveName, "rb");
	if (file == 0)
		return false;
	bool ok = CFileMgr::Read(file, (char *)&header, sizeof(header)) == sizeof(header);
	CFileMgr::CloseFile(file);
	if (ok)
		*time = header.SaveDateTime;
	return ok;
}

static bool
Later(const SYSTEMTIME &a, const SYSTEMTIME &b)
{
	const uint16 x[] = { a.wYear, a.wMonth, a.wDay, a.wHour, a.wMinute, a.wSecond, a.wMilliseconds };
	const uint16 y[] = { b.wYear, b.wMonth, b.wDay, b.wHour, b.wMinute, b.wSecond, b.wMilliseconds };
	for (int i = 0; i < 7; i++)
		if (x[i] != y[i])
			return x[i] > y[i];
	return false;
}

static int
MostRecentSlot()
{
	int best = -1;
	SYSTEMTIME bestTime, time;
	PcSaveHelper.PopulateSlotInfo();
	for (int i = 0; i < SLOT_COUNT; i++)
		if (Slots[i] == SLOT_OK && SlotSaveTime(i, &time) && (best < 0 || Later(time, bestTime))) {
			best = i;
			bestTime = time;
		}
	return best;
}

// (the game completed at 100 % ready to load, as CheckSlotDataValid readies a slot's: a PC save whose bytes before its
// last four add up to them, its level in its first block)
static bool
FreeModeStartReady()
{
	static uint8 buffer[256 * 1024];
	snprintf(LoadFileName, sizeof(LoadFileName), "%s%s", CFileMgr::GetRootDirName(), FREE_MODE_START);
	int file = CFileMgr::OpenFile(LoadFileName, "rb");
	if (file == 0) {
		debug("Orthros: no %s, no new free game\n", FREE_MODE_START);
		return false;
	}
	int size = CFileMgr::Read(file, (char *)buffer, sizeof(buffer));
	CFileMgr::CloseFile(file);
	if (size < 16 || size >= (int)sizeof(buffer))
		return false;
	uint32 sum = 0, stored;
	for (int i = 0; i < size - 4; i++)
		sum += buffer[i];
	memcpy(&stored, buffer + size - 4, sizeof(stored));
	if (sum != stored)
		return false;
	memcpy(&m_LevelToLoad, buffer + 8, sizeof(m_LevelToLoad));
	CStreaming::DeleteAllRwObjects();
	return true;
}

// (the pause menu's "Save Game", in a free game only: the save screen of a safe house, anywhere — not during a
// mission, whose script would be saved half way)
static wchar *
FreeModeSaveDraw(bool *disabled, bool)
{
	*disabled = CTheScripts::IsPlayerOnAMission();
	return nil;
}

static void
FreeModeSavePress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT || CTheScripts::IsPlayerOnAMission())
		return;
	FrontEndMenuManager.RequestFrontEndShutDown();
	FrontEndMenuManager.m_bActivateSaveMenu = true;
}

static void
SetPauseSaveEntry(bool on)
{
	static CMenuScreenCustom::CMenuEntry entry;
	static bool in;
	if (on == in)
		return;
	CMenuScreenCustom::CMenuEntry *entries = aScreens[MENUPAGE_PAUSE_MENU].m_aEntries;
	int count = 0;
	while (count < NUM_MENUROWS && entries[count].m_Action != MENUACTION_NOTHING)
		count++;
	if (on) {
		if (count < 1 || count >= NUM_MENUROWS)
			return;
		if (entry.m_Action == MENUACTION_NOTHING) {
			entry.m_Action = MENUACTION_CFO_DYNAMIC;
			strncpy(entry.m_EntryName, "GMSAVE", sizeof(entry.m_EntryName));
			entry.m_CFODynamic = new CCFODynamic(nil, nil, nil, FreeModeSaveDraw, FreeModeSavePress);
			entry.m_Align = MENUALIGN_CENTER;
		}
		// (after "Resume")
		memmove(&entries[2], &entries[1], (count - 1) * sizeof(entries[0]));
		entries[1] = entry;
	} else {
		if (count < 2 || entries[1].m_Action != MENUACTION_CFO_DYNAMIC || strcmp(entries[1].m_EntryName, "GMSAVE") != 0)
			return;
		memmove(&entries[1], &entries[2], (count - 2) * sizeof(entries[0]));
		memset(&entries[count - 1], 0, sizeof(entries[0]));
	}
	in = on;
}

// (the free mode's game completed at 100 %: the phone calls its player never answered, made — else the phone rings for
// them again and again, conversations of the story and nothing more. main.scm's variables — the PC script whose main
// part is 204,596 bytes: its CELL thread marks each call made once its conversation has been heard)
void
GameLoaded()
{
	static const int callsMade[] = { 1139, 1140, 1158, 1162, 1164 }; // (MOB_17A, MOB_18A, MOB_63A, MOB_71A, MOB_73A)
	// (the shared world's base: prepared by its session, the free mode's ways not)
	if (sharedGame) {
		OrthrosMmo::GameLoaded();
		return;
	}
	if (!bFreeMode || CTheScripts::MainScriptSize != 204596)
		return;
	for (int i = 0; i < (int)ARRAY_SIZE(callsMade); i++)
		*(int32 *)&CTheScripts::ScriptSpace[callsMade[i] * 4] = 1;
}

// (a game starts: a free one or not, the shared world's or not)
static void
StartingGame(bool free, bool shared = false)
{
	bFreeMode = free;
	sharedGame = shared;
	SetPauseSaveEntry(free);
	OrthrosMmo::GameStarting(shared);
}

void
SaveMenuOpening()
{
	if (freeSaves != bFreeMode)
		UseFreeSaves(bFreeMode);
}

void
StartingNewGame()
{
	StartingGame(false);
}

bool
CheckLoad(int32 slot)
{
	// (the shared world's base: the free mode's start, a game of the shared world and not a free one)
	if (sharedStart) {
		sharedStart = false;
		if (!FreeModeStartReady())
			return false;
		StartingGame(false, true);
		return true;
	}
	// (a mission's retry: the quick save its start made, in its game's files — the game goes on as it was)
	if (slot >= SLOT_COUNT) {
		SaveMenuOpening();
		if (!CheckSlotDataValid(slot))
			return false;
		StartingGame(bFreeMode);
		return true;
	}
	if (freeModeStart) {
		freeModeStart = false;
		if (!FreeModeStartReady())
			return false;
		StartingGame(true);
		return true;
	}
	if (!CheckSlotDataValid(slot))
		return false;
	StartingGame(freeSaves);
	return true;
}

// (the game's load and delete pages, theirs again once the player is back on the story's start page)
static void
ListsFor(bool free)
{
	const int page = free ? MENUPAGE_FREE_MODE : MENUPAGE_NEW_GAME;
	aScreens[MENUPAGE_CHOOSE_LOAD_SLOT].m_PreviousPage = page;
	aScreens[MENUPAGE_CHOOSE_DELETE_SLOT].m_PreviousPage = page;
	aScreens[MENUPAGE_DELETE_SUCCESSFUL].m_PreviousPage = page;
	aScreens[MENUPAGE_DELETE_SUCCESSFUL].m_aEntries[1].m_TargetMenu = page;
	if (freeSaves != free)
		UseFreeSaves(free);
}

void
ScreenChanged(int screen)
{
	if (screen == MENUPAGE_NEW_GAME && freeSaves)
		ListsFor(false);
}

void
FreeModeContinuePress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	ListsFor(true);
	const int slot = MostRecentSlot();
	if (slot < 0)
		freeModeStart = true;
	else
		FrontEndMenuManager.m_nCurrSaveSlot = slot;
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_LOADING_IN_PROGRESS);
}

void
FreeModeNewPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	ListsFor(true);
	freeModeStart = true;
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_LOADING_IN_PROGRESS);
}

void
FreeModeLoadPress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	ListsFor(true);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_CHOOSE_LOAD_SLOT);
}

void
FreeModeDeletePress(int8 action)
{
	if (action != FEOPTION_ACTION_SELECT)
		return;
	ListsFor(true);
	FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_CHOOSE_DELETE_SLOT);
}

// ---------------------------------------------------------------- the shared world's base
// (loaded from a menu as "New Free Game" loads its game; with no menu open — a game being played —, as the pause menu's
// load page would)
void
StartSharedWorld()
{
	sharedStart = true;
	if (FrontEndMenuManager.m_bMenuActive) {
		FrontEndMenuManager.m_nCurrSaveSlot = 0;
		FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_LOADING_IN_PROGRESS);
		return;
	}
	if (CheckLoad(0)) {
		FrontEndMenuManager.DoSettingsBeforeStartingAGame();
		FrontEndMenuManager.m_bWantToLoad = true;
	}
}

void
CancelSharedWorld()
{
	if (!sharedStart)
		return;
	sharedStart = false;
	if (FrontEndMenuManager.m_bMenuActive && FrontEndMenuManager.m_nCurrScreen == MENUPAGE_LOADING_IN_PROGRESS)
		FrontEndMenuManager.SwitchToNewScreen(MENUPAGE_START_MENU);
}
}
#endif
