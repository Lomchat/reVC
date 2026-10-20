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

namespace OrthrosExtras
{
bool bFreeMode;

bool
CommandLine(const char *arg)
{
	if (strcmp(arg, "-freemode") != 0)
		return false;
	bFreeMode = true;
	// (the menus' own additions were made before the command line was read: the free mode's one now)
	FreeModeAddMenuEntries();
	return true;
}

const char *
SaveFilePrefix()
{
	return bFreeMode ? "GTAVCfree" : "GTAVCsf";
}

// ---------------------------------------------------------------- the game's text
// The game's fonts hold the French accented letters at the codes its French text uses (TEXT/french.gxt); an
// accented capital they have not: its letter without the accent.
static void
GameText(const char *utf8, wchar *out, int size)
{
	int n = 0;
	for (const uint8 *s = (const uint8 *)utf8; *s && n < size - 1; ) {
		uint32 c = *s++;
		if (c >= 0xC0 && (*s & 0xC0) == 0x80) {
			c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
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
	GameText(fr ? "Tapez un code en jouant pour l'activer  -  HELP, Entrée ou Echap : fermer"
	            : "Type a code while playing to use it  -  HELP, Enter or Esc: close", text, ARRAY_SIZE(text));
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
// (the game completed at 100 %, among the game's files, copied to the free mode's first slot when it has no save)
static const char *FREE_MODE_TEMPLATE = "orthros\\freemode.b";

static bool
CopyFreeModeTemplate()
{
	static uint8 buffer[256 * 1024];
	CFileMgr::ChangeDir("\\");
	int in = CFileMgr::OpenFile(FREE_MODE_TEMPLATE, "rb");
	if (in == 0) {
		debug("Orthros: no %s, the free mode starts on the main menu\n", FREE_MODE_TEMPLATE);
		return false;
	}
	int size = CFileMgr::Read(in, (char *)buffer, sizeof(buffer));
	CFileMgr::CloseFile(in);
	if (size <= 0 || size >= (int)sizeof(buffer))
		return false;
	MakeValidSaveName(0);
	int out = CFileMgr::OpenFile(ValidSaveName, "wb");
	if (out == 0)
		return false;
	bool ok = CFileMgr::Write(out, (char *)buffer, size) == size;
	CFileMgr::CloseFile(out);
	return ok;
}

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
	PcSaveHelper.PopulateSlotInfo();
	int best = -1;
	SYSTEMTIME bestTime, time;
	for (int i = 0; i < SLOT_COUNT; i++)
		if (Slots[i] == SLOT_OK && SlotSaveTime(i, &time) && (best < 0 || Later(time, bestTime))) {
			best = i;
			bestTime = time;
		}
	return best;
}

void
FreeModeBoot()
{
	int slot = MostRecentSlot();
	if (slot < 0 && CopyFreeModeTemplate())
		slot = MostRecentSlot();
	if (slot < 0)
		return;
	// (as the menu's "load" does: its slot checked, then the game started on it)
	FrontEndMenuManager.m_nCurrSaveSlot = slot;
	if (CheckSlotDataValid(slot)) {
		FrontEndMenuManager.DoSettingsBeforeStartingAGame();
		FrontEndMenuManager.m_bWantToLoad = true;
	}
}

// (the pause menu's "Save Game": the save screen of a safe house, anywhere — not during a mission, whose script would
// be saved half way)
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

void
FreeModeAddMenuEntries()
{
	if (!bFreeMode)
		return;
	FrontendOptionSetCursor(MENUPAGE_PAUSE_MENU, 1);
	FrontendOptionAddDynamic("GMSAVE", 0, 0, MENUALIGN_CENTER, FreeModeSaveDraw, nil, FreeModeSavePress);
}
}
#endif
