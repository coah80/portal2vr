#pragma once
#include "rig.h"

class Game;
class VR;

namespace Body
{
	struct HandInput
	{
		bool tracked = false;
		bool skeletal = false;
		rig::V3 position;
		rig::V3 forward;
		rig::V3 right;
		rig::V3 up;
		float curl[rig::FingerCount] = {};
	};

	struct Input
	{
		bool valid = false;
		rig::V3 eyeOffset;
		rig::V3 hmdForward;
		rig::V3 hmdRight;
		rig::V3 hmdUp;
		float trigger = 0;
		HandInput hand[2];
	};

	void Install(Game* game, VR* vr);
	void Publish(const Input& input);
	void SetEnabled(bool enabled);
	void SetHidden(bool hidden);
	bool Visible();
	int ViewRecursionLevel();
}
