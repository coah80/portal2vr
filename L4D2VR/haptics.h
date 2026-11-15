#pragma once

class Game;
class VR;

namespace Haptics
{
	enum Target { LeftHand, RightHand, GunHand, OffHand, BothHands };

	void Install(Game* game, VR* vr);
	void SetStrength(float strength);
	void Pulse(Target target, float seconds, float frequency, float amplitude, float delay = 0.0f);
	void Think(void* localPlayer);
	void Frame(float seconds);
	void OnPortalled();
}
