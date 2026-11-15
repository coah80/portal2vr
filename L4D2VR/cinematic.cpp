#include "cinematic.h"
#include "body.h"
#include "game.h"
#include "netvars.h"
#include "sdk.h"
#include "vr.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
	constexpr float kCutDistance = 64.0f;
	constexpr float kCutYaw = 50.0f;
	constexpr float kFollowDeadzone = 25.0f;
	constexpr float kFollowSettle = 3.0f;
	constexpr float kFollowRate = 90.0f;
	constexpr float kPositionAlpha = 0.35f;
	constexpr float kPositionBeta = 0.05f;
	constexpr float kYawSmoothing = 6.0f;

	Game* g_Game_ = nullptr;
	VR* g_VR_ = nullptr;
	bool g_Enabled = true;

	struct State
	{
		bool active = false;
		bool anchorPending = false;
		bool following = false;
		bool vehicle = false;
		int settleFrames = 0;
		QAngle gameAngles = { 0, 0, 0 };
		Vector position = { 0, 0, 0 };
		Vector velocity = { 0, 0, 0 };
		float cameraYaw = 0;
		float entryOffset = 0;
		float anchorCameraYaw = 0;
		float anchorOffset = 0;
		float offset = 0;
		Vector lastCamera = { 0, 0, 0 };
		float lastCameraYaw = 0;
		double lastTime = 0;
	};

	State g_State;

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	float Wrap(float degrees)
	{
		degrees = std::fmod(degrees + 180.0f, 360.0f);
		if (degrees < 0)
			degrees += 360.0f;
		return degrees - 180.0f;
	}

	bool ValidHandle(void* player, const char* prop, void** entity)
	{
		int offset = Netvars::Find("class:CPortal_Player", prop);
		if (!offset)
			return false;
		int handle = Netvars::At<int>(player, offset);
		if (handle == -1 || handle == 0)
			return false;
		*entity = g_Game_->GetClientEntityFromHandle(handle);
		return *entity != nullptr;
	}

	bool Wanted(void* player, bool* vehicle)
	{
		void* view = nullptr;
		void* ride = nullptr;
		bool hasView = ValidHandle(player, "m_hViewEntity", &view) && view != player;
		bool hasRide = ValidHandle(player, "m_hVehicle", &ride);
		*vehicle = hasRide;
		return hasView || hasRide;
	}

	float HeadYaw()
	{
		return g_VR_->m_HmdPose.TrackedDeviceAng.y;
	}

	void Anchor(const Vector& camera, float cameraYaw)
	{
		State& s = g_State;
		s.anchorPending = false;
		s.following = false;
		s.position = camera;
		s.velocity = { 0, 0, 0 };
		s.cameraYaw = cameraYaw;
		s.anchorCameraYaw = cameraYaw;
		s.anchorOffset = Wrap(cameraYaw - HeadYaw());
		s.offset = s.anchorOffset;
		g_VR_->ResetPosition();
	}

	void Enter(void* player, bool vehicle)
	{
		State& s = g_State;
		s.active = true;
		s.anchorPending = true;
		s.vehicle = vehicle;
		s.settleFrames = vehicle ? 1 : 0;
		s.offset = g_VR_->m_RotationOffset.y;
		s.entryOffset = s.offset;
		if (vehicle)
			s.gameAngles = { 0, 0, 0 };
		else
			g_Game_->m_EngineClient->GetViewAngles(s.gameAngles);
		s.lastTime = Now();
		Body::SetHidden(true);
	}

	void Leave()
	{
		State& s = g_State;
		s.active = false;
		g_VR_->m_RotationOffset.y = s.entryOffset;
		g_VR_->UpdateHMDAngles();
		g_VR_->ResetPosition();
		Body::SetHidden(false);
	}

	void Filter(const Vector& camera, float cameraYaw, float dt)
	{
		State& s = g_State;

		Vector predicted = s.position + s.velocity * dt;
		Vector residual = camera - predicted;
		s.position = predicted + residual * kPositionAlpha;
		s.velocity = s.velocity + residual * (kPositionBeta / dt);

		float blend = 1.0f - std::exp(-dt * kYawSmoothing);
		s.cameraYaw = Wrap(s.cameraYaw + Wrap(cameraYaw - s.cameraYaw) * blend);

		float desired = Wrap(s.anchorOffset + Wrap(s.cameraYaw - s.anchorCameraYaw));
		float gap = Wrap(desired - s.offset);
		if (std::fabs(gap) > kFollowDeadzone)
			s.following = true;
		if (s.following)
		{
			float step = std::min(std::fabs(gap), kFollowRate * dt);
			s.offset = Wrap(s.offset + (gap > 0 ? step : -step));
			if (std::fabs(Wrap(desired - s.offset)) < kFollowSettle)
				s.following = false;
		}
	}
}

namespace Cinematic
{
	void Install(Game* game, VR* vr)
	{
		g_Game_ = game;
		g_VR_ = vr;
	}

	void SetEnabled(bool enabled)
	{
		g_Enabled = enabled;
		if (!enabled && g_State.active)
			Leave();
	}

	bool Update(void* localPlayer, CViewSetup& setup)
	{
		State& s = g_State;
		if (!g_Enabled || !localPlayer || !Netvars::Ready())
		{
			if (s.active)
				Leave();
			return false;
		}

		bool vehicle = false;
		bool wanted = Wanted(localPlayer, &vehicle);

		if (!s.active && wanted)
			Enter(localPlayer, vehicle);
		else if (s.active && !wanted)
		{
			Leave();
			return false;
		}

		if (!s.active)
			return false;

		Vector camera = setup.origin;
		float cameraYaw = setup.angles.y;
		double now = Now();
		float dt = std::clamp(static_cast<float>(now - s.lastTime), 0.001f, 0.1f);
		s.lastTime = now;

		Vector jump = camera - s.lastCamera;
		bool cut = std::sqrt(jump.x * jump.x + jump.y * jump.y + jump.z * jump.z) > kCutDistance
			|| std::fabs(Wrap(cameraYaw - s.lastCameraYaw)) > kCutYaw;

		if (s.anchorPending && s.settleFrames > 0)
		{
			--s.settleFrames;
			s.position = camera;
		}
		else if (s.anchorPending || cut)
			Anchor(camera, cameraYaw);
		else
			Filter(camera, cameraYaw, dt);

		s.lastCamera = camera;
		s.lastCameraYaw = cameraYaw;

		g_VR_->m_RotationOffset.y = s.offset;
		g_VR_->UpdateHMDAngles();
		setup.origin = s.position;
		return true;
	}

	bool Active()
	{
		return g_State.active;
	}

	QAngle GameAngles()
	{
		return g_State.gameAngles;
	}
}
