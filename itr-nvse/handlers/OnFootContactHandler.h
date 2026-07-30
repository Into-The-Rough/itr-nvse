#pragma once

namespace OnFootContactHandler
{
	bool Init(void* nvse);
	void InstallListenerProbes();
	void Update();
	void ClearState();
}
