#include "StartNewGameCommand.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "internal/UIMinimal.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"

#include "internal/globals.h"

extern const _ExtractArgs ExtractArgs;

using UIMinimal::Tile;

namespace
{
	//return to the main menu (0x703DA0 closes menus, 0x7D0A70 tears down + shows title)
	void ReturnToMainMenu() { CdeclCall<void>(0x703DA0); CdeclCall<void>(0x7D0A70); }

	//StartMenu option callbacks (option+8), used to identify list rows to click
	constexpr UInt32 kCallback_NewGame = 0x7D0490;     //title "New Game", raises confirm
	constexpr UInt32 kCallback_NewGameYes = 0x7D32B0;  //confirmed new-game worker

	constexpr UInt32 kListBox_MainOptions = 0x84;
	constexpr UInt32 kListBox_Confirm = 0xE4;
	constexpr UInt32 kOffset_Selected = 0x10;   //OptionListBox selected tile (vfunc1 returns *(lb+0x10))
	constexpr UInt32 kOffset_HandleClick = 0x0C; //Menu vtable slot 3

	enum Phase { kIdle, kStart, kWaitTitle, kWaitConfirm, kWatchClose, kCleanup };

	constexpr UInt32 kReadySettle = 30;
	constexpr UInt32 kConfirmDelay = 40;   //StartMenu::HandleClick debounces clicks <500ms apart
	constexpr UInt32 kConfirmTimeout = 200;
	constexpr UInt32 kTimeoutFrames = 1800;
	constexpr UInt32 kLoadedSettle = 30;
	constexpr UInt32 kCleanupInterval = 15;
	constexpr UInt32 kCleanupTimeout = 600;

	void* const* const g_startMenu = reinterpret_cast<void* const*>(0x11DAAC0);
	void* const* const g_loadingMenu = reinterpret_cast<void* const*>(0x11DA0C0);
	void* const* const g_messageMenu = reinterpret_cast<void* const*>(0x11DA4F0);
	UInt8* const g_messageMenuMode = reinterpret_cast<UInt8*>(0x11DA4EC);

	Phase s_phase = kIdle;
	UInt32 s_idleFrames = 0;
	UInt32 s_waitFrames = 0;
	UInt32 s_confirmFrames = 0;
	UInt32 s_watchFrames = 0;
	UInt32 s_cleanupFrames = 0;

	void* PlayerCell()
	{
		auto* player = *g_thePlayerPtr;
		return player ? player->parentCell : nullptr;
	}

	bool IsMenuTransitioning()
	{
		void* menu = *g_startMenu;
		return menu && ThisCall<bool>(0x7CE8F0, menu);
	}

	bool IsStartMenuActive()
	{
		return CdeclCall<char>(0x702450, 1013) != 0;
	}

	UInt32 MenuVisState()
	{
		void* menu = *g_startMenu;
		return menu ? *reinterpret_cast<UInt32*>(static_cast<char*>(menu) + 0x24) : 0;
	}

	UInt32 PlayerControlFlags()
	{
		auto* player = *g_thePlayerPtr;
		return player ? *reinterpret_cast<UInt32*>(reinterpret_cast<char*>(player) + 0x680) : 0;
	}

	UInt32 HudMode()
	{
		void* hud = *g_hudMainMenuPtr;
		return hud ? *reinterpret_cast<UInt32*>(static_cast<char*>(hud) + 0x1C4) : 0;
	}

	UInt32 LoadingFlags()
	{
		void* menu = *g_loadingMenu;
		return menu ? *reinterpret_cast<UInt16*>(static_cast<char*>(menu) + 0x222) : 0;
	}

	UInt32 InterfaceState()
	{
		void* im = GetInterfaceManager();
		return im ? *reinterpret_cast<UInt32*>(static_cast<char*>(im) + 0x0C) : 0;
	}

	UInt32 MenuStack(UInt32 idx)
	{
		void* im = GetInterfaceManager();
		return im && idx < 10 ? *reinterpret_cast<UInt32*>(static_cast<char*>(im) + 0x114 + idx * 4) : 0;
	}

	UInt32 TopMenu()
	{
		return CdeclCall<UInt32>(0x7023C0);
	}

	UInt32 IsMenuMode()
	{
		return CdeclCall<char>(0x702360) != 0;
	}

	void LogState(const char* label, UInt32 frame)
	{
		Log("%s f=%u cell=%p sm=%p lm=%p lflags=%04X msg=%p msgmode=%u ctrl=%08X hud=%u mm=%u istate=%u top=%u stack=%u,%u,%u",
			label, frame, PlayerCell(), *g_startMenu, *g_loadingMenu, LoadingFlags(), *g_messageMenu, *g_messageMenuMode,
			PlayerControlFlags(), HudMode(), IsMenuMode(), InterfaceState(), TopMenu(), MenuStack(0), MenuStack(1), MenuStack(2));
	}

	//StartMenu is present, active and fully shown (not mid-fade)
	bool IsMenuShown()
	{
		return *g_startMenu && !IsMenuTransitioning() && IsStartMenuActive() && MenuVisState() == 1;
	}

	Tile::Value* GetTileValue(const Tile* tile, UInt32 typeID)
	{
		if (!tile) return nullptr;
		Tile::Value** data = tile->values.data;
		UInt32 count = tile->values.size;
		UInt32 lo = 0, hi = count;
		while (lo < hi)
		{
			UInt32 mid = (lo + hi) / 2;
			Tile::Value* v = data[mid];
			if (v->id < typeID) lo = mid + 1;
			else if (v->id == typeID) return v;
			else hi = mid;
		}
		return nullptr;
	}

	UInt32 GetTileId(const Tile* tile)
	{
		Tile::Value* v = GetTileValue(tile, UIMinimal::kTileValue_id);
		return v ? static_cast<UInt32>(v->num) : 0;
	}

	//row layout matches StartMenu::HandleClick: node{data,next} at listBox+4,
	//list item{tile@0, option@4}, option callback at option+8
	struct Node { void* data; Node* next; };

	Tile* FindRowByCallback(void* menu, UInt32 listBoxOffset, UInt32 targetCallback, bool verbose)
	{
		if (!menu) return nullptr;
		Node* node = reinterpret_cast<Node*>(static_cast<char*>(menu) + listBoxOffset + 4);
		int i = 0;
		for (; node && node->data && i < 64; node = node->next, ++i)
		{
			void* li = node->data;
			Tile* tile = *reinterpret_cast<Tile**>(li);
			void* option = *reinterpret_cast<void**>(static_cast<char*>(li) + 4);
			UInt32 cb = option ? *reinterpret_cast<UInt32*>(static_cast<char*>(option) + 8) : 0;
			if (verbose)
				Log("  row %d tile=%p option=%p cb=%08X", i, tile, option, cb);
			if (cb == targetCallback)
				return tile;
		}
		return nullptr;
	}

	void HandleClick(void* menu, UInt32 tileID, Tile* tile)
	{
		void** vtbl = *reinterpret_cast<void***>(menu);
		auto fn = reinterpret_cast<void(__thiscall*)(void*, int, Tile*)>(vtbl[kOffset_HandleClick / 4]);
		fn(menu, static_cast<int>(tileID), tile);
	}

	bool ClickRow(UInt32 listBoxOffset, UInt32 callback, const char* label)
	{
		void* menu = *g_startMenu;
		if (!menu) { Log("%s: no startMenu", label); return false; }
		Tile* tile = FindRowByCallback(menu, listBoxOffset, callback, true);
		if (!tile) { Log("%s: row not found", label); return false; }
		//a real click selects the row on hover first, the confirm positions against
		//listbox->selected (OptionListBox::vfunc1 returns *(listbox+0x10))
		*reinterpret_cast<Tile**>(static_cast<char*>(menu) + listBoxOffset + kOffset_Selected) = tile;
		UInt32 id = GetTileId(tile);
		Log("%s: clicking tile=%p id=%u", label, tile, id);
		HandleClick(menu, id, tile);
		return true;
	}

	//poll the confirm list-box for the given Yes row, respecting the click debounce, and click it
	bool TryClickConfirm(UInt32 yesCallback, const char* label)
	{
		if (s_confirmFrames < kConfirmDelay) return false;
		if (!FindRowByCallback(*g_startMenu, kListBox_Confirm, yesCallback, false)) return false;
		Log("%s confirm f=%u -> click Yes", label, s_confirmFrames);
		ClickRow(kListBox_Confirm, yesCallback, label);
		return true;
	}

	void ClearPresentationState()
	{
		LogState("clear pre", s_cleanupFrames);
		if (*g_messageMenu)
			CdeclCall<void>(0x7A8DF0);
		CdeclCall<void>(0x78CFC0);
		void* tes = GetTES();
		if (tes)
		{
			void* target = nullptr;
			char ret = ThisCall<char>(0x457D70, tes, 0, target, 0);
			Log("clear 457D70 ret=%d", ret);
		}
		else
		{
			Log("clear: no TES");
		}

		auto* player = *g_thePlayerPtr;
		if (player)
			ThisCall<int>(0x95F530, player, 0, 0x7F);
		void* im = GetInterfaceManager();
		if (im)
		{
			for (UInt32 i = 0; i < 10; ++i)
			{
				UInt32 top = TopMenu();
				if (!top)
					break;
				int ret = ThisCall<int>(0x714FD0, im, top, static_cast<char>(1));
				Log("clear stack top=%u ret=%d", top, ret);
			}
			CdeclCall<void>(0x702840);
			if (!TopMenu() && IsMenuMode() && InterfaceState() != 4)
			{
				UInt32 oldState = InterfaceState();
				*reinterpret_cast<UInt32*>(static_cast<char*>(im) + 0x0C) = 4;
				Log("clear istate %u -> 4", oldState);
				CdeclCall<void>(0x702840);
			}
		}
		LogState("clear post", s_cleanupFrames);
	}
}

extern bool Cmd_StartNewGame_Execute(COMMAND_ARGS);
static CommandInfo kCommandInfo_StartNewGame =
{
	"StartNewGame", "", 0,
	"starts a new game from anywhere, returning to the main menu first if a game is loaded",
	0, 0, nullptr,
	Cmd_StartNewGame_Execute, nullptr, nullptr, 0
};

bool Cmd_StartNewGame_Execute(COMMAND_ARGS)
{
	*result = 0;
	s_phase = kStart;
	s_idleFrames = 0;
	s_waitFrames = 0;
	s_confirmFrames = 0;
	s_watchFrames = 0;
	s_cleanupFrames = 0;
	LogState("REQUEST", 0);
	*result = 1;
	return true;
}

namespace StartNewGameCommand
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_StartNewGame);
	}

	//a manual save load fires PreLoadGame, never the command's own new-game path,
	//without this the leftover kWatchClose/kCleanup phases would run
	//ClearPresentationState against the freshly loaded game
	void Abort()
	{
		if (s_phase != kIdle)
			Log("ABORT phase=%d (save load)", s_phase);
		s_phase = kIdle;
		s_idleFrames = 0;
		s_waitFrames = 0;
		s_confirmFrames = 0;
		s_watchFrames = 0;
		s_cleanupFrames = 0;
	}

	void Update()
	{
		switch (s_phase)
		{
		case kIdle:
			return;

		case kStart:
			Log("kStart  cell=%p startMenu=%p", PlayerCell(), *g_startMenu);
			s_idleFrames = 0;
			s_waitFrames = 0;
			if (PlayerCell())
				ReturnToMainMenu();
			s_phase = kWaitTitle;
			return;

		case kWaitTitle:
			if (++s_waitFrames > kTimeoutFrames)
			{
				Log("TIMEOUT waiting for title f=%u", s_waitFrames);
				s_phase = kIdle;
				return;
			}
			if ((s_waitFrames % 10) == 0)
				Log("title f=%u cell=%p sm=%p active=%d vis=%X", s_waitFrames, PlayerCell(), *g_startMenu, IsStartMenuActive(), MenuVisState());
			//title = game unloaded AND StartMenu fully shown
			if (PlayerCell() || !IsMenuShown())
			{
				s_idleFrames = 0;
				return;
			}
			if (++s_idleFrames < kReadySettle)
				return;
			Log("TITLE ready f=%u -> click New Game", s_waitFrames);
			ClickRow(kListBox_MainOptions, kCallback_NewGame, "newgame");
			s_phase = kWaitConfirm;
			s_confirmFrames = 0;
			return;

		case kWaitConfirm:
			if (++s_confirmFrames > kConfirmTimeout)
			{
				Log("TIMEOUT waiting for new-game confirm f=%u", s_confirmFrames);
				s_phase = kIdle;
				return;
			}
			if (TryClickConfirm(kCallback_NewGameYes, "newgame"))
			{
				s_phase = kWatchClose;
				s_watchFrames = 0;
				s_idleFrames = 0;
				s_cleanupFrames = 0;
			}
			return;

		case kWatchClose:
			++s_watchFrames;
			if ((s_watchFrames % 20) == 0)
				LogState("watch", s_watchFrames);
			if (s_watchFrames > kTimeoutFrames)
			{
				LogState("TIMEOUT waiting for loaded new game", s_watchFrames);
				s_phase = kIdle;
				return;
			}
			if (!PlayerCell() || *g_startMenu)
			{
				s_idleFrames = 0;
				return;
			}
			if (++s_idleFrames < kLoadedSettle)
				return;
			LogState("loaded stable", s_watchFrames);
			ClearPresentationState();
			s_phase = kCleanup;
			s_cleanupFrames = 0;
			s_watchFrames = 0;
			return;

		case kCleanup:
			++s_cleanupFrames;
			if ((s_cleanupFrames % kCleanupInterval) == 0)
				ClearPresentationState();
			if ((s_cleanupFrames % 20) == 0)
				LogState("cleanup", s_cleanupFrames);
			if (s_cleanupFrames >= kLoadedSettle && !*g_loadingMenu)
			{
				LogState("cleanup done", s_cleanupFrames);
				s_phase = kIdle;
				return;
			}
			if (s_cleanupFrames > kCleanupTimeout)
			{
				LogState("TIMEOUT cleanup", s_cleanupFrames);
				s_phase = kIdle;
			}
			return;
		}
	}
}
