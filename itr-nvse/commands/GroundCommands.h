#pragma once

namespace GroundCommands {
	bool Init(void* nvse);
	void RegisterCommands(void* nvse);
	void Update();
	void ClearState();
}

