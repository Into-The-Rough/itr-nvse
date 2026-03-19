//mutes all audio when the game window loses focus

#include "AltTabMute.h"
#include "internal/globals.h"
#include <Windows.h>

namespace AltTabMute
{
	constexpr UInt32 kNumVolumeChannels = 12;
	#define INI_MUSIC_VOLUME_ADDR 0x11F6E44

	struct BSAudioManager
	{
		void* vtable;
		UInt8 pad004[0x13C];
		float volumes[kNumVolumeChannels];

		static BSAudioManager* Get() { return (BSAudioManager*)0x11F6EF0; }
	};

	static HWND g_gameWindow = nullptr;
	static bool g_wasInFocus = true;
	static float g_savedVolumes[kNumVolumeChannels] = {0};
	static float g_savedIniMusicVolume = 0.0f;
	static bool g_volumesSaved = false;

	static void OnFocusLost()
	{
		BSAudioManager* audioMgr = BSAudioManager::Get();
		for (UInt32 i = 0; i < kNumVolumeChannels; i++)
		{
			g_savedVolumes[i] = audioMgr->volumes[i];
			audioMgr->volumes[i] = 0.0f;
		}
		float* iniMusicVolume = (float*)INI_MUSIC_VOLUME_ADDR;
		g_savedIniMusicVolume = *iniMusicVolume;
		*iniMusicVolume = 0.0f;
		g_volumesSaved = true;
	}

	static void OnFocusGained()
	{
		if (!g_volumesSaved) return;
		BSAudioManager* audioMgr = BSAudioManager::Get();
		for (UInt32 i = 0; i < kNumVolumeChannels; i++)
		{
			audioMgr->volumes[i] = g_savedVolumes[i];
		}
		float* iniMusicVolume = (float*)INI_MUSIC_VOLUME_ADDR;
		*iniMusicVolume = g_savedIniMusicVolume;
	}

	void Update()
	{
		if (!g_gameWindow)
			g_gameWindow = FindWindowA(nullptr, "Fallout: New Vegas");

		if (g_gameWindow)
		{
			bool currentlyInFocus = (GetForegroundWindow() == g_gameWindow);
			if (currentlyInFocus != g_wasInFocus)
			{
				if (!currentlyInFocus)
					OnFocusLost();
				else
					OnFocusGained();
				g_wasInFocus = currentlyInFocus;
			}
		}
	}
}

