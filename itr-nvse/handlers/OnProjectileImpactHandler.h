#pragma once

namespace ProjectileLogic { struct Config; }

namespace OnProjectileImpactHandler {
	bool Init(void* nvseInterface);
	void InstallHook();
	void Update();
	void ClearState();
	void UpdateSettings(const ProjectileLogic::Config& cfg, bool masterEnabled);
	//seeds cfg.hardMaterials/thinMaterials with canonical material ids
	void FillDefaultMaterials(ProjectileLogic::Config& cfg);
}
