#pragma once
#include <cmath>
#include <vector>

namespace rig
{
	struct V3
	{
		float x = 0, y = 0, z = 0;
		V3() = default;
		V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
		V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		V3 operator*(float s) const { return { x * s, y * s, z * s }; }
		V3 operator-() const { return { -x, -y, -z }; }
		V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
		V3& operator-=(const V3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
	};

	inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float Length(const V3& a) { return std::sqrt(Dot(a, a)); }
	inline V3 Normalized(const V3& a, const V3& fallback = { 1, 0, 0 })
	{
		float l = Length(a);
		return l > 1e-6f ? a * (1.0f / l) : fallback;
	}
	inline V3 Lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
	inline V3 Reject(const V3& v, const V3& n) { return v - n * Dot(v, n); }

	struct M34
	{
		float m[3][4];

		V3 Axis(int i) const { return { m[0][i], m[1][i], m[2][i] }; }
		V3 Origin() const { return { m[0][3], m[1][3], m[2][3] }; }
		void SetAxis(int i, const V3& v) { m[0][i] = v.x; m[1][i] = v.y; m[2][i] = v.z; }
		void SetOrigin(const V3& v) { m[0][3] = v.x; m[1][3] = v.y; m[2][3] = v.z; }

		static M34 Identity()
		{
			M34 r{};
			r.m[0][0] = r.m[1][1] = r.m[2][2] = 1;
			return r;
		}

		static M34 FromAxes(const V3& x, const V3& y, const V3& z, const V3& o)
		{
			M34 r{};
			r.SetAxis(0, x); r.SetAxis(1, y); r.SetAxis(2, z); r.SetOrigin(o);
			return r;
		}

		V3 TransformPoint(const V3& p) const
		{
			return {
				m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
				m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
				m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] };
		}

		V3 Rotate(const V3& v) const
		{
			return {
				m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
				m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
				m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
		}

		V3 InverseRotate(const V3& v) const
		{
			return {
				m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
				m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
				m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z };
		}
	};

	M34 Mul(const M34& a, const M34& b);
	M34 InverseRigid(const M34& a);
	M34 AxisAngle(const V3& axis, float radians);
	M34 RotationBetween(const V3& from, const V3& to);
	M34 RotateAbout(const M34& bone, const M34& rotation, const V3& pivot);
	M34 Orthonormalized(const M34& a);
	M34 Slerp(const M34& a, const M34& b, float t);

	V3 SolveElbow(const V3& shoulder, float upperLength, float lowerLength, const V3& target, const V3& pole, V3* reachedTarget);

	class Pose
	{
	public:
		void Reset(const M34* world, const int* parents, int count);
		int Count() const { return (int)m_World.size(); }
		const M34& World(int i) const { return m_World[i]; }
		void SetWorld(int bone, const M34& world);
		void RotateBone(int bone, const M34& rotation);
		void TranslateAll(const V3& delta);
		void AimBone(int bone, int child, const V3& targetChildPosition);
		void CopyTo(M34* out) const;
		bool Valid(int bone) const { return bone >= 0 && bone < Count(); }
		int Parent(int bone) const { return m_Parent[bone]; }

	private:
		void PropagateFrom(int bone);

		std::vector<M34> m_World;
		std::vector<M34> m_Local;
		std::vector<int> m_Parent;
	};

	enum Hand { Left = 0, Right = 1 };
	enum Finger { Thumb = 0, Index, Middle, Ring, Pinky, FingerCount };

	struct ArmBones
	{
		int clavicle = -1, upper = -1, upperTwist = -1, elbow = -1, wrist = -1;
		int forearmTwist[3] = { -1, -1, -1 };
		int finger[FingerCount][3] = {
			{ -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 } };
	};

	struct LegBones
	{
		int thigh = -1, knee = -1, ankle = -1;
	};

	struct BodyBones
	{
		int root = -1, neck = -1, head = -1;
		ArmBones arm[2];
		LegBones leg[2];
		std::vector<int> headHide;
	};

	struct HandTarget
	{
		bool valid = false;
		V3 wristPosition;
		V3 fingerDirection;
		V3 palmNormal;
		float curl[FingerCount] = {};
		float splay = 0;
	};

	struct BodyTarget
	{
		V3 neckPosition;
		V3 bodyForward;
		V3 bodyUp = { 0, 0, 1 };
		float maxCrouch = 40.0f;
		float maxLean = 24.0f;
		HandTarget hand[2];
	};

	struct SolveReport
	{
		V3 wristReached[2];
		float handError[2] = {};
	};

	SolveReport SolveBody(Pose& pose, const BodyBones& bones, const BodyTarget& target);
	void CurlFingers(Pose& pose, const ArmBones& arm, const HandTarget& hand, Hand side);
	void CollapseBones(M34* world, const BodyBones& bones);
}
