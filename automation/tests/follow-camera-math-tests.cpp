#include "../../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h"
#include "../../AfxHookSource2/Filmmaker/Movie/ThirdPersonCameraMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-6) {
	return std::fabs(a - b) <= epsilon;
}
}

int main() {
	Check(Near(FollowWrapDegrees(190.0), -170.0), "wrap +190");
	Check(Near(FollowWrapDegrees(-190.0), 170.0), "wrap -190");
	Check(Near(ThirdPersonWrapYaw(540.0), 180.0), "thirdperson yaw wrap");
	Check(Near(ThirdPersonWrapYaw(-181.0), 179.0), "thirdperson negative yaw wrap");
	Check(Near(ThirdPersonClampPitch(100.0), 85.0), "thirdperson pitch max");
	Check(Near(ThirdPersonClampPitch(-100.0), -85.0), "thirdperson pitch min");
	Check(Near(ThirdPersonClampDistance(10.0), 30.0), "thirdperson distance min");
	Check(Near(ThirdPersonClampDistance(250.0), 200.0), "thirdperson distance max");
	double presetYaw = 0.0;
	Check(ThirdPersonPresetYaw("left", presetYaw) && Near(presetYaw, -90.0), "thirdperson left preset");

	// Orbit pose solver: east-facing pivot (yaw 0), camera behind at 100u, level pitch.
	const double orbitEye[3] = {100.0, 50.0, 64.0};
	const ThirdPersonPose behind = ThirdPersonSolvePose(orbitEye, 0.0, 0.0, 100.0);
	Check(Near(behind.x, 0.0) && Near(behind.y, 50.0) && Near(behind.z, 64.0)
		&& Near(behind.yaw, 0.0) && Near(behind.pitch, 0.0), "orbit camera sits behind pivot");
	// World yaw 90 (north): camera south of the pivot, looking north at it.
	const ThirdPersonPose side = ThirdPersonSolvePose(orbitEye, 90.0, 0.0, 100.0);
	Check(Near(side.x, 100.0, 1e-5) && Near(side.y, -50.0, 1e-5) && Near(side.yaw, 90.0),
		"orbit camera yaw 90 sits south of pivot");
	// Positive pitch = camera raised above the pivot, looking down at it.
	const ThirdPersonPose raised = ThirdPersonSolvePose(orbitEye, 0.0, 30.0, 100.0);
	Check(raised.z > orbitEye[2] && Near(raised.pitch, 30.0), "orbit positive pitch raises camera");
	// Yaw ease: tau 0 snaps; otherwise the approach takes the shortest wrapped arc.
	Check(Near(ThirdPersonApproachYaw(170.0, -170.0, 1.0, 0.0), -170.0), "orbit yaw snap with zero tau");
	const double eased = ThirdPersonApproachYaw(170.0, -170.0, 0.12, 0.12);
	const double easedArc = ThirdPersonWrapYaw(eased - 170.0);
	Check(easedArc > 0.0 && easedArc < 20.0, "orbit yaw ease crosses the wrap the short way");

	const FollowAngles east = FollowLookAt({0, 0, 0}, {100, 0, 0});
	Check(Near(east.pitch, 0.0) && Near(east.yaw, 0.0), "look east");
	const FollowAngles north = FollowLookAt({0, 0, 0}, {0, 100, 0});
	Check(Near(north.yaw, 90.0), "look north");
	const FollowAngles up = FollowLookAt({0, 0, 0}, {0, 0, 100});
	Check(Near(up.pitch, -90.0), "look up");
	const FollowAngles mounted = FollowAddAngles({10, 170, -175}, {5, 25, -20});
	Check(Near(mounted.pitch, 15.0) && Near(mounted.yaw, -165.0) && Near(mounted.roll, 165.0),
		"rigid attach angle trim wraps");
	const FollowVec3 nearVerticalCamera{100, 20, 68};
	const FollowVec3 nearVerticalTarget{100.001, 20, 60};
	const FollowAngles singularLook = FollowLookAt(nearVerticalCamera, nearVerticalTarget);
	const FollowAngles rigidNearVertical = FollowAddAngles({12, 45, 3}, {0, 10, 0});
	Check(std::fabs(singularLook.pitch) > 85.0 && Near(rigidNearVertical.yaw, 55.0),
		"rigid attach does not derive yaw from near-vertical look-at");

	Check(Near(FollowHalfTimeAlpha(1.0, 1.0), 0.5), "half-time alpha");
	const FollowAngles halfway = FollowSmoothAngles({0, 0, 0}, {0, 90, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(halfway.yaw, 45.0), "half-time angle smoothing");

	const FollowAngles wrapped = FollowSmoothAngles({0, 170, 0}, {0, -170, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(wrapped.yaw, 180.0), "shortest wrapped arc");

	const FollowAngles deadzone = FollowSmoothAngles({2, 3, 0}, {2.2, 3.2, 0}, 1.0, 0.1, 1.0, 0.0);
	Check(Near(deadzone.pitch, 2.0) && Near(deadzone.yaw, 3.0), "deadzone");

	const FollowAngles limited = FollowSmoothAngles({0, 0, 0}, {0, 180, 0}, 0.1, 0.0, 0.0, 30.0);
	Check(Near(std::fabs(limited.yaw), 3.0), "max turn speed");

	const FollowVec3 p = FollowSmoothPosition({0, 0, 0}, {10, 20, 30}, 1.0, 1.0);
	Check(Near(p.x, 5.0) && Near(p.y, 10.0) && Near(p.z, 15.0), "position smoothing");
	Check(Near(FollowDistance({0, 0, 0}, {3, 4, 0}), 5.0), "distance");

	const FollowVec3 front = FollowRotateVector({72, 0, 8}, {0, 0, 0});
	Check(Near(front.x, 72.0) && Near(front.y, 0.0) && Near(front.z, 8.0), "local front offset");
	const FollowVec3 yawedFront = FollowRotateVector({72, 0, 0}, {0, 90, 0});
	Check(Near(yawedFront.x, 0.0, 1e-5) && Near(yawedFront.y, 72.0, 1e-5), "local offset follows yaw");
	const FollowVec3 localMount{32, -12, 7};
	const FollowAngles mountAngles{15, 70, -20};
	const FollowVec3 worldMount = FollowRotateVector(localMount, mountAngles);
	const FollowVec3 recoveredMount = FollowInverseRotateVector(worldMount, mountAngles);
	Check(Near(recoveredMount.x, localMount.x, 1e-5)
		&& Near(recoveredMount.y, localMount.y, 1e-5)
		&& Near(recoveredMount.z, localMount.z, 1e-5),
		"local attach offset round-trips through inverse rotate");
	const FollowVec3 steppedOffset = FollowRotateVector({40, 0, 0}, {0, 0, 0});
	const FollowVec3 localRebase = FollowInverseRotateVector(steppedOffset, {0, 0, 0});
	const FollowVec3 freshOffset = FollowRotateVector(localRebase, {0, 90, 0});
	Check(Near(freshOffset.x, 0.0, 1e-5) && Near(freshOffset.y, 40.0, 1e-5),
		"render-time attach rebase rotates local offset with fresh mount orientation");
	const FollowVec3 target{100, 20, 60};
	const FollowVec3 camera{172, 20, 68};
	const FollowAngles backAtTarget = FollowLookAt(camera, target);
	Check(Near(backAtTarget.yaw, 180.0) && backAtTarget.pitch > 0.0, "front camera looks back at target");

	const FollowVec3 frozen = FollowSmoothPosition({10, 20, 30}, {100, 200, 300}, 0.0, 0.25);
	Check(Near(frozen.x, 10.0) && Near(frozen.y, 20.0) && Near(frozen.z, 30.0), "zero-dt position hold");
	const FollowAngles frozenAngles = FollowSmoothAngles({4, 5, 6}, {40, 50, 60}, 0.0, 0.25, 0.0, 0.0);
	Check(Near(frozenAngles.pitch, 4.0) && Near(frozenAngles.yaw, 5.0), "zero-dt angle hold");

	std::cout << "FollowCameraMathTests: all checks passed\n";
	return 0;
}
