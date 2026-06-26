#pragma once

namespace Filmmaker {

struct FollowVec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct FollowAngles {
	double pitch = 0.0;
	double yaw = 0.0;
	double roll = 0.0;
};

double FollowWrapDegrees(double value);
double FollowHalfTimeAlpha(double halfTimeSeconds, double deltaSeconds);
FollowAngles FollowLookAt(const FollowVec3& camera, const FollowVec3& target);
FollowAngles FollowAddAngles(const FollowAngles& base, const FollowAngles& offset);

// Convert an attachment/bone quaternion (x, y, z, w) to engine view angles
// (pitch, yaw, roll in degrees). Used by rigid attach mode so the camera adopts
// the attachment's orientation, not just its position.
FollowAngles FollowQuatToAngles(double x, double y, double z, double w);

// Rotate a LOCAL-space offset (x = forward, y = right, z = up) by the given
// angles into world space. Lets an attach offset stay "behind the muzzle" /
// "above the head" as the target turns, instead of being a fixed world vector.
FollowVec3 FollowRotateVector(const FollowVec3& localOffset, const FollowAngles& angles);
FollowVec3 FollowInverseRotateVector(const FollowVec3& worldOffset, const FollowAngles& angles);
FollowAngles FollowSmoothAngles(
	const FollowAngles& current,
	const FollowAngles& desired,
	double deltaSeconds,
	double halfTimeSeconds,
	double deadzoneDegrees,
	double maxTurnDegreesPerSecond);
FollowVec3 FollowSmoothPosition(
	const FollowVec3& current,
	const FollowVec3& desired,
	double deltaSeconds,
	double halfTimeSeconds);
double FollowDistance(const FollowVec3& a, const FollowVec3& b);

} // namespace Filmmaker
