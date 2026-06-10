#include "CenterOnCellAltCommand.h"
#include "internal/CallTemplates.h"
#include "internal/GameGlobals.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

#include <cstring>

extern const _ExtractArgs ExtractArgs;
extern bool IsConsoleMode();
extern void Console_Print(const char* fmt, ...);
extern void Log(const char* fmt, ...);
extern NVSEMessagingInterface* g_msgInterface;

namespace
{
	constexpr UInt32 kCenterDelayFrames = 2;
	constexpr UInt32 kSessionDelayFrames = 2;
	constexpr UInt32 kPendingTimeoutFrames = 1800;
	constexpr size_t kCellNameSize = 512;

	using CenterOnCell_t = bool(__thiscall*)(TESObjectREFR*, char*, void*);

	const auto CenterOnCell = reinterpret_cast<CenterOnCell_t>(0x93DB60);

	bool s_pending = false;
	bool s_sessionQueued = false;
	bool s_seenNewGame = false;
	UInt32 s_sessionDelayFrames = 0;
	UInt32 s_framesAfterNewGame = 0;
	UInt32 s_waitFrames = 0;
	char s_pendingCellName[kCellNameSize] = {};

	PlayerCharacter* GetPlayer()
	{
		return *g_thePlayerPtr;
	}

	bool IsMainMenuCOC()
	{
		auto* player = GetPlayer();
		return player && !player->parentCell;
	}

	void CopyCellName(char* dest, size_t destSize, const char* src)
	{
		if (!dest || !destSize)
			return;

		dest[0] = '\0';
		if (src && src[0])
			strncpy_s(dest, destSize, src, _TRUNCATE);
	}

	void RunCenterOnCell(const char* cellName)
	{
		auto* player = GetPlayer();
		if (!player || !cellName || !cellName[0])
			return;

		char cellNameCopy[kCellNameSize] = {};
		CopyCellName(cellNameCopy, sizeof(cellNameCopy), cellName);

		CdeclCall<void>(0x703DA0);
		CenterOnCell(player, cellNameCopy, nullptr);
	}

	void DispatchNewSession()
	{
		if (!g_msgInterface)
		{
			Log("CenterOnCellAlt could not dispatch new session: messaging interface unavailable");
			s_seenNewGame = true;
			s_framesAfterNewGame = 0;
			return;
		}

		Log("CenterOnCellAlt dispatching synthetic kMessage_NewGame for %s", s_pendingCellName);
		if (!g_msgInterface->Dispatch(0, NVSEMessagingInterface::kMessage_NewGame, nullptr, 0, nullptr))
		{
			Log("CenterOnCellAlt synthetic kMessage_NewGame had no listeners for %s", s_pendingCellName);
			s_seenNewGame = true;
			s_framesAfterNewGame = 0;
		}
	}

	void QueueNewSessionCenter(const char* cellName)
	{
		CopyCellName(s_pendingCellName, sizeof(s_pendingCellName), cellName);
		s_pending = true;
		s_sessionQueued = true;
		s_seenNewGame = false;
		s_sessionDelayFrames = 0;
		s_framesAfterNewGame = 0;
		s_waitFrames = 0;

		if (IsConsoleMode())
			Console_Print("CenterOnCellAlt >> queued new session, then COC %s", s_pendingCellName);

		Log("CenterOnCellAlt queued COC %s through synthetic new session", s_pendingCellName);
		CdeclCall<void>(0x703DA0);
	}
}

static ParamInfo kParams_CenterOnCellAlt[1] =
{
	{ "cellName", kParamType_String, 0 },
};

extern bool Cmd_CenterOnCellAlt_Execute(COMMAND_ARGS);
static CommandInfo kCommandInfo_CenterOnCellAlt =
{
	"CenterOnCellAlt", "COCA", 0,
	"CenterOnCell after dispatching a new session when used from the main menu",
	0, 1, kParams_CenterOnCellAlt,
	Cmd_CenterOnCellAlt_Execute, nullptr, nullptr, 0
};

bool Cmd_CenterOnCellAlt_Execute(COMMAND_ARGS)
{
	*result = 0;

	char cellName[kCellNameSize] = {};
	if (!ExtractArgs(EXTRACT_ARGS, cellName) || !cellName[0])
		return true;

	if (IsMainMenuCOC())
	{
		QueueNewSessionCenter(cellName);
		*result = 1;
		return true;
	}

	RunCenterOnCell(cellName);
	*result = 1;
	return true;
}

namespace CenterOnCellAltCommand
{
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_CenterOnCellAlt);
	}

	void OnNewGame()
	{
		if (s_pending)
		{
			s_seenNewGame = true;
			s_framesAfterNewGame = 0;
			Log("CenterOnCellAlt saw kMessage_NewGame for %s", s_pendingCellName);
		}
	}

	void ClearPending()
	{
		s_pending = false;
		s_sessionQueued = false;
		s_seenNewGame = false;
		s_sessionDelayFrames = 0;
		s_framesAfterNewGame = 0;
		s_waitFrames = 0;
		s_pendingCellName[0] = '\0';
	}

	void Update()
	{
		if (!s_pending)
			return;

		if (++s_waitFrames > kPendingTimeoutFrames)
		{
			Log("CenterOnCellAlt timed out waiting to COC %s", s_pendingCellName);
			ClearPending();
			return;
		}

		if (s_sessionQueued)
		{
			if (++s_sessionDelayFrames < kSessionDelayFrames)
				return;

			s_sessionQueued = false;
			DispatchNewSession();
			return;
		}

		if (!s_seenNewGame)
			return;

		if (++s_framesAfterNewGame < kCenterDelayFrames)
			return;

		char cellName[kCellNameSize] = {};
		CopyCellName(cellName, sizeof(cellName), s_pendingCellName);
		ClearPending();
		Log("CenterOnCellAlt running COC %s after synthetic new session", cellName);
		RunCenterOnCell(cellName);
	}
}
