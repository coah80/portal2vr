#pragma once
#include "vector.h"

class Game;
class VR;

namespace Interact
{
	enum Hand { Left = 0, Right = 1 };

	struct HandState
	{
		bool tracked = false;
		bool grip = false;
		Vector position = { 0, 0, 0 };
		Vector forward = { 1, 0, 0 };
	};

	struct Input
	{
		bool valid = false;
		HandState hand[2];
		Vector gunTip = { 0, 0, 0 };
	};

	void Install(Game* game, VR* vr);
	void SetEnabled(bool enabled);
	void Publish(const Input& input);
	void ServerTick(void* serverPlayer);
	bool Physical();
	Hand HoldHand();
	bool WantsUse(bool classicPressed);
}
