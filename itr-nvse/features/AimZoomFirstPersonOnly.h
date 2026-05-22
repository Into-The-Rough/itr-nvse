#pragma once

namespace AimZoomFirstPersonOnly
{
	void Init(bool enabled);
	void SetEnabled(bool enabled);
	bool IsEnabled();
	void RegisterCommands(void* nvse);
}
