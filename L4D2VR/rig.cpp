#include "rig.h"
#include <algorithm>

namespace rig
{
	M34 Mul(const M34& a, const M34& b)
	{
		M34 r{};
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 4; ++j)
			{
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
			}
			r.m[i][3] += a.m[i][3];
		}
		return r;
	}

	M34 InverseRigid(const M34& a)
	{
		M34 r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r.m[i][j] = a.m[j][i];
		V3 o = a.Origin();
		r.SetOrigin(-(r.Rotate(o)));
		return r;
	}

	M34 AxisAngle(const V3& axisIn, float radians)
	{
		V3 a = Normalized(axisIn);
		float c = std::cos(radians), s = std::sin(radians), t = 1 - c;
		M34 r{};
		r.m[0][0] = t * a.x * a.x + c;       r.m[0][1] = t * a.x * a.y - s * a.z; r.m[0][2] = t * a.x * a.z + s * a.y;
		r.m[1][0] = t * a.x * a.y + s * a.z; r.m[1][1] = t * a.y * a.y + c;       r.m[1][2] = t * a.y * a.z - s * a.x;
		r.m[2][0] = t * a.x * a.z - s * a.y; r.m[2][1] = t * a.y * a.z + s * a.x; r.m[2][2] = t * a.z * a.z + c;
		return r;
	}

	M34 RotationBetween(const V3& fromIn, const V3& toIn)
	{
		V3 from = Normalized(fromIn), to = Normalized(toIn);
		float d = std::clamp(Dot(from, to), -1.0f, 1.0f);
		V3 axis = Cross(from, to);
		if (Length(axis) < 1e-6f)
		{
			if (d > 0)
				return M34::Identity();
			V3 perp = std::fabs(from.x) < 0.9f ? Cross(from, { 1, 0, 0 }) : Cross(from, { 0, 1, 0 });
			return AxisAngle(perp, 3.14159265f);
		}
		return AxisAngle(axis, std::acos(d));
	}

	M34 RotateAbout(const M34& bone, const M34& rotation, const V3& pivot)
	{
		M34 r = Mul(rotation, bone);
		r.SetOrigin(rotation.Rotate(bone.Origin() - pivot) + pivot);
		return r;
	}

	M34 Orthonormalized(const M34& a)
	{
		V3 x = Normalized(a.Axis(0));
		V3 y = Normalized(Reject(a.Axis(1), x), { 0, 1, 0 });
		V3 z = Cross(x, y);
		return M34::FromAxes(x, y, z, a.Origin());
	}

	M34 Slerp(const M34& a, const M34& b, float t)
	{
		M34 rel = Mul(b, InverseRigid(a));
		float trace = rel.m[0][0] + rel.m[1][1] + rel.m[2][2];
		float angle = std::acos(std::clamp((trace - 1) * 0.5f, -1.0f, 1.0f));
		V3 axis = { rel.m[2][1] - rel.m[1][2], rel.m[0][2] - rel.m[2][0], rel.m[1][0] - rel.m[0][1] };
		M34 out = a;
		if (Length(axis) > 1e-6f && angle > 1e-5f)
		{
			M34 step = AxisAngle(axis, angle * t);
			out = Mul(step, a);
		}
		out.SetOrigin(Lerp(a.Origin(), b.Origin(), t));
		return out;
	}

	V3 SolveElbow(const V3& shoulder, float upper, float lower, const V3& target, const V3& pole, V3* reached)
	{
		V3 toTarget = target - shoulder;
		float dist = Length(toTarget);
		V3 dir = Normalized(toTarget, { 0, 0, -1 });
		float minReach = std::fabs(upper - lower) + 0.01f;
		float maxReach = upper + lower - 0.01f;
		float d = std::clamp(dist, minReach, maxReach);
		if (reached)
			*reached = shoulder + dir * d;

		float cosA = std::clamp((upper * upper + d * d - lower * lower) / (2 * upper * d), -1.0f, 1.0f);
		float sinA = std::sqrt(std::max(0.0f, 1 - cosA * cosA));
		V3 bend = Normalized(Reject(pole - shoulder, dir), Normalized(Reject({ 0, 0, -1 }, dir), { 1, 0, 0 }));
		return shoulder + dir * (upper * cosA) + bend * (upper * sinA);
	}

	void Pose::Reset(const M34* world, const int* parents, int count)
	{
		m_World.assign(world, world + count);
		m_Parent.assign(parents, parents + count);
		m_Local.resize(count);
		for (int i = 0; i < count; ++i)
		{
			int p = m_Parent[i];
			m_Local[i] = (p >= 0 && p < i) ? Mul(InverseRigid(m_World[p]), m_World[i]) : m_World[i];
		}
	}

	void Pose::PropagateFrom(int bone)
	{
		std::vector<char> dirty(m_World.size(), 0);
		dirty[bone] = 1;
		for (int i = bone + 1; i < Count(); ++i)
		{
			int p = m_Parent[i];
			if (p >= 0 && dirty[p])
			{
				m_World[i] = Mul(m_World[p], m_Local[i]);
				dirty[i] = 1;
			}
		}
	}

	void Pose::SetWorld(int bone, const M34& world)
	{
		if (!Valid(bone))
			return;
		m_World[bone] = world;
		int p = m_Parent[bone];
		m_Local[bone] = (p >= 0) ? Mul(InverseRigid(m_World[p]), world) : world;
		PropagateFrom(bone);
	}

	void Pose::RotateBone(int bone, const M34& rotation)
	{
		if (!Valid(bone))
			return;
		SetWorld(bone, RotateAbout(m_World[bone], rotation, m_World[bone].Origin()));
	}

	void Pose::TranslateAll(const V3& delta)
	{
		for (int i = 0; i < Count(); ++i)
		{
			m_World[i].SetOrigin(m_World[i].Origin() + delta);
			if (m_Parent[i] < 0)
				m_Local[i] = m_World[i];
		}
	}

	void Pose::AimBone(int bone, int child, const V3& target)
	{
		if (!Valid(bone) || !Valid(child))
			return;
		V3 origin = m_World[bone].Origin();
		RotateBone(bone, RotationBetween(m_World[child].Origin() - origin, target - origin));
	}

	void Pose::CopyTo(M34* out) const
	{
		std::copy(m_World.begin(), m_World.end(), out);
	}

	namespace
	{
		float SignedAngleAbout(const V3& from, const V3& to, const V3& axis)
		{
			V3 a = Normalized(Reject(from, axis), {}), b = Normalized(Reject(to, axis), {});
			return std::atan2(Dot(Cross(a, b), axis), Dot(a, b));
		}

		V3 AnyPerpendicular(const V3& v)
		{
			return Normalized(std::fabs(v.z) < 0.9f ? Cross(v, { 0, 0, 1 }) : Cross(v, { 1, 0, 0 }));
		}

		struct ForearmProbe
		{
			V3 elbowLocal, wristLocal;
		};

		ForearmProbe CaptureProbe(const Pose& pose, const ArmBones& arm)
		{
			const M34& elbow = pose.World(arm.elbow);
			const M34& wrist = pose.World(arm.wrist);
			V3 axis = Normalized(wrist.Origin() - elbow.Origin());
			V3 world = AnyPerpendicular(axis);
			return { elbow.InverseRotate(world), wrist.InverseRotate(world) };
		}

		float ForearmRoll(const Pose& pose, const ArmBones& arm, const ForearmProbe& probe)
		{
			const M34& elbow = pose.World(arm.elbow);
			const M34& wrist = pose.World(arm.wrist);
			V3 axis = Normalized(wrist.Origin() - elbow.Origin());
			return SignedAngleAbout(elbow.Rotate(probe.elbowLocal), wrist.Rotate(probe.wristLocal), axis);
		}

		struct HandFrame
		{
			V3 finger, across, normal;
		};

		HandFrame LocalHandFrame(const Pose& pose, const ArmBones& arm)
		{
			const M34& wrist = pose.World(arm.wrist);
			V3 knuckles = pose.World(arm.finger[Middle][0]).Origin();
			V3 indexBase = pose.World(arm.finger[Index][0]).Origin();
			V3 pinkyBase = pose.World(arm.finger[Pinky][0]).Origin();
			V3 f = Normalized(wrist.InverseRotate(knuckles - wrist.Origin()));
			V3 a = Normalized(Reject(wrist.InverseRotate(indexBase - pinkyBase), f), AnyPerpendicular(f));
			return { f, a, Cross(f, a) };
		}

		M34 BasisFromFrame(const HandFrame& h)
		{
			return M34::FromAxes(h.finger, h.across, h.normal, {});
		}

		M34 TransposeRotation(const M34& m)
		{
			M34 r{};
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r.m[i][j] = m.m[j][i];
			return r;
		}

		bool ArmComplete(const ArmBones& arm)
		{
			return arm.upper >= 0 && arm.elbow >= 0 && arm.wrist >= 0;
		}

		void SolveLeg(Pose& pose, const LegBones& leg, const M34& ankleTarget, const V3& forward)
		{
			if (leg.thigh < 0 || leg.knee < 0 || leg.ankle < 0)
				return;
			V3 hip = pose.World(leg.thigh).Origin();
			float upper = Length(pose.World(leg.knee).Origin() - hip);
			float lower = Length(pose.World(leg.ankle).Origin() - pose.World(leg.knee).Origin());
			V3 pole = pose.World(leg.knee).Origin() + forward * 20.0f;
			V3 reached;
			V3 knee = SolveElbow(hip, upper, lower, ankleTarget.Origin(), pole, &reached);
			pose.AimBone(leg.thigh, leg.knee, knee);
			pose.AimBone(leg.knee, leg.ankle, reached);
			M34 ankle = ankleTarget;
			ankle.SetOrigin(pose.World(leg.ankle).Origin());
			pose.SetWorld(leg.ankle, ankle);
		}
	}

	void CurlFingers(Pose& pose, const ArmBones& arm, const HandTarget& hand, Hand side)
	{
		static const float maxBend[FingerCount][3] = {
			{ 0.35f, 0.55f, 0.75f },
			{ 1.20f, 1.50f, 1.10f },
			{ 1.25f, 1.55f, 1.10f },
			{ 1.30f, 1.55f, 1.10f },
			{ 1.35f, 1.50f, 1.10f } };
		const float relaxed = 0.15f;

		if (arm.wrist < 0)
			return;

		V3 palm = Normalized(hand.palmNormal, { 0, 0, -1 });

		for (int f = 0; f < FingerCount; ++f)
		{
			float amount = std::clamp(hand.curl[f], 0.0f, 1.0f) - relaxed;
			int base = arm.finger[f][0], next = arm.finger[f][1];
			if (base < 0 || next < 0)
				continue;
			V3 axis = Normalized(Cross(pose.World(next).Origin() - pose.World(base).Origin(), palm), {});
			if (Length(axis) < 0.5f)
				continue;
			for (int j = 0; j < 3; ++j)
				if (arm.finger[f][j] >= 0)
					pose.RotateBone(arm.finger[f][j], AxisAngle(axis, amount * maxBend[f][j]));
		}
		(void)side;
	}

	SolveReport SolveBody(Pose& pose, const BodyBones& bones, const BodyTarget& target)
	{
		SolveReport report;
		if (!pose.Valid(bones.neck))
			return report;

		M34 ankleBefore[2];
		for (int l = 0; l < 2; ++l)
			if (pose.Valid(bones.leg[l].ankle))
				ankleBefore[l] = pose.World(bones.leg[l].ankle);

		ForearmProbe probe[2];
		HandFrame localHand[2];
		bool armOk[2] = {};
		for (int s = 0; s < 2; ++s)
		{
			const ArmBones& arm = bones.arm[s];
			armOk[s] = ArmComplete(arm) && arm.finger[Middle][0] >= 0 && arm.finger[Index][0] >= 0 && arm.finger[Pinky][0] >= 0;
			if (!armOk[s])
				continue;
			probe[s] = CaptureProbe(pose, arm);
			localHand[s] = LocalHandFrame(pose, arm);
		}

		V3 delta = target.neckPosition - pose.World(bones.neck).Origin();
		V3 horizontal = { delta.x, delta.y, 0 };
		float lean = Length(horizontal);
		if (lean > target.maxLean)
			horizontal = horizontal * (target.maxLean / lean);
		float drop = std::clamp(delta.z, -target.maxCrouch, 0.0f);
		pose.TranslateAll(horizontal + V3{ 0, 0, drop });

		for (int l = 0; l < 2; ++l)
			if (pose.Valid(bones.leg[l].ankle))
				SolveLeg(pose, bones.leg[l], ankleBefore[l], target.bodyForward);

		V3 up = Normalized(target.bodyUp, { 0, 0, 1 });
		V3 forward = Normalized(Reject(target.bodyForward, up), { 1, 0, 0 });
		V3 left = Cross(up, forward);

		for (int s = 0; s < 2; ++s)
		{
			const ArmBones& arm = bones.arm[s];
			const HandTarget& hand = target.hand[s];
			if (!armOk[s] || !hand.valid)
				continue;

			V3 shoulder = pose.World(arm.upper).Origin();
			float upper = Length(pose.World(arm.elbow).Origin() - shoulder);
			float lower = Length(pose.World(arm.wrist).Origin() - pose.World(arm.elbow).Origin());
			float reach = upper + lower;
			float want = Length(hand.wristPosition - shoulder);
			float stretch = std::clamp(want / (reach * 0.98f), 1.0f, 1.2f);
			upper *= stretch;
			lower *= stretch;

			V3 outward = s == Right ? -left : left;
			V3 pole = shoulder + up * -24.0f + outward * 14.0f + forward * -10.0f;

			V3 reached;
			V3 elbow = SolveElbow(shoulder, upper, lower, hand.wristPosition, pole, &reached);

			pose.AimBone(arm.upper, arm.elbow, elbow);
			M34 elbowWorld = pose.World(arm.elbow);
			elbowWorld.SetOrigin(elbow);
			pose.SetWorld(arm.elbow, elbowWorld);
			pose.AimBone(arm.elbow, arm.wrist, reached);

			V3 finger = Normalized(hand.fingerDirection);
			V3 palm = Normalized(Reject(hand.palmNormal, finger), AnyPerpendicular(finger));
			V3 across = Cross(finger, palm) * (s == Right ? 1.0f : -1.0f);
			HandFrame world = { finger, across, Cross(finger, across) };
			M34 wrist = Mul(BasisFromFrame(world), TransposeRotation(BasisFromFrame(localHand[s])));
			wrist.SetOrigin(reached);
			pose.SetWorld(arm.wrist, wrist);

			float twist = ForearmRoll(pose, arm, probe[s]);
			V3 axis = Normalized(reached - elbow);
			static const float share[3] = { 0.1f, 0.5f, 0.9f };
			for (int t = 0; t < 3; ++t)
				if (pose.Valid(arm.forearmTwist[t]))
					pose.RotateBone(arm.forearmTwist[t], AxisAngle(axis, twist * share[t]));

			CurlFingers(pose, arm, hand, (Hand)s);

			report.wristReached[s] = pose.World(arm.wrist).Origin();
			report.handError[s] = Length(report.wristReached[s] - hand.wristPosition);
		}

		return report;
	}

	void CollapseBones(M34* world, const BodyBones& bones)
	{
		if (bones.neck < 0)
			return;
		V3 anchor = world[bones.neck].Origin();
		for (int b : bones.headHide)
		{
			M34& m = world[b];
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					m.m[i][j] *= 0.001f;
			m.SetOrigin(anchor);
		}
	}
}
