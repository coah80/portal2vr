#pragma once
#include "vector.h"

class Game;
class VR;
class CViewSetup;

namespace Cinematic
{
	void Install(Game* game, VR* vr);
	void SetEnabled(bool enabled);
	bool Update(void* localPlayer, CViewSetup& setup);
	bool Active();
	QAngle GameAngles();
}
