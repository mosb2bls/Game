#pragma once

#include <string>
#include <vector>
#include <map>

#include "Maths.h"

struct Bone
{
	std::string name;    // Bone identifier (matches mesh/animation channels)
	Matrix offset;       // Inverse bind pose (offset) matrix
	int parentIndex;     // Parent bone index (-1 for root)
};

struct Skeleton
{
	std::vector<Bone> bones;   // Bone hierarchy
	Matrix globalInverse;      // Inverse of model/root transform from importer

	// Linear search bone index by name
	int findBone(std::string name)
	{
		for (int i = 0; i < bones.size(); i++)
		{
			if (bones[i].name == name)
			{
				return i;
			}
		}
		return -1;
	}
};

struct AnimationFrame
{
	std::vector<Vec3> positions;        // Per-bone local position
	std::vector<Quaternion> rotations;  // Per-bone local rotation
	std::vector<Vec3> scales;           // Per-bone local scale
};

struct AnimationSequence
{
	std::vector<AnimationFrame> frames; // Discrete keyframes (uniform tick spacing)
	float ticksPerSecond;               // Playback rate in ticks/sec

	// Linear interpolation for vectors
	Vec3 interpolate(Vec3 p1, Vec3 p2, float t)
	{
		return ((p1 * (1.0f - t)) + (p2 * t));
	}

	// Slerp interpolation for rotations
	Quaternion interpolate(Quaternion q1, Quaternion q2, float t)
	{
		return Quaternion::slerp(q1, q2, t);
	}

	// Total sequence length in seconds
	float duration()
	{
		return ((float)frames.size() / ticksPerSecond);
	}

	// Convert time to base frame index + fractional interpolation
	void calcFrame(float t, int& frame, float& interpolationFact)
	{
		interpolationFact = t * ticksPerSecond;
		frame = (int)floorf(interpolationFact);
		interpolationFact = interpolationFact - (float)frame;
		frame = std::min(frame, (int)(frames.size() - 1));
	}

	// Check if current time is still within available frames
	bool running(float t)
	{
		if ((int)floorf(t * ticksPerSecond) < frames.size())
		{
			return true;
		}
		return false;
	}

	// Clamp next frame index (no looping)
	int nextFrame(int frame)
	{
		return std::min(frame + 1, (int)(frames.size() - 1));
	}

	// Build this bone's global matrix using interpolated local S/R/T and parent global
	Matrix interpolateBoneToGlobal(Matrix* matrices, int baseFrame, float interpolationFact, Skeleton* skeleton, int boneIndex)
	{
		Matrix scale = Matrix::scaling(interpolate(frames[baseFrame].scales[boneIndex], frames[nextFrame(baseFrame)].scales[boneIndex], interpolationFact));
		Matrix rotation = interpolate(frames[baseFrame].rotations[boneIndex], frames[nextFrame(baseFrame)].rotations[boneIndex], interpolationFact).toMatrix();
		Matrix translation = Matrix::translation(interpolate(frames[baseFrame].positions[boneIndex], frames[nextFrame(baseFrame)].positions[boneIndex], interpolationFact));
		Matrix local = scale * rotation * translation;

		if (skeleton->bones[boneIndex].parentIndex > -1)
		{
			Matrix global = local * matrices[skeleton->bones[boneIndex].parentIndex];
			return global;
		}
		return local;
	}
};

class Animation
{
public:
	std::map<std::string, AnimationSequence> animations;  // Named animation clips
	Skeleton skeleton;                                    // Shared skeleton definition

	// Number of bones in the skeleton
	int bonesSize()
	{
		return skeleton.bones.size();
	}

	// Time-to-frame conversion for a given clip
	void calcFrame(std::string name, float t, int& frame, float& interpolationFact)
	{
		animations[name].calcFrame(t, frame, interpolationFact);
	}

	// Bone global matrix at time t (interpolated), using supplied parent globals
	Matrix interpolateBoneToGlobal(std::string name, Matrix* matrices, int baseFrame, float interpolationFact, int boneIndex)
	{
		return animations[name].interpolateBoneToGlobal(matrices, baseFrame, interpolationFact, &skeleton, boneIndex);
	}

	// Convert globals into final skinning matrices used by the shader
	void calcTransforms(Matrix* matrices, Matrix coordTransform)
	{
		for (int i = 0; i < bonesSize(); i++)
		{
			matrices[i] = skeleton.bones[i].offset * matrices[i] * skeleton.globalInverse * coordTransform;
		}
	}

	// Clip existence check
	bool hasAnimation(std::string name)
	{
		if (animations.find(name) == animations.end())
		{
			return false;
		}
		return true;
	}
};

class AnimationInstance
{
public:
	Animation* animation;       // Shared animation data (skeleton + clips)
	std::string usingAnimation; // Current clip name
	float t;                    // Current time in seconds

	Matrix matrices[256];       // Final skinning matrices (shader limit = 256)
	Matrix matricesPose[256];   // Pose globals for querying bone world transforms
	Matrix coordTransform;      // Coordinate-system conversion (importer -> engine)

	// Initialise instance and optional coordinate conversion
	void init(Animation* _animation, int fromYZX)
	{
		animation = _animation;
		if (fromYZX == 1)
		{
			memset(coordTransform.a, 0, 16 * sizeof(float));
			coordTransform.a[0][0] = 1.0f;
			coordTransform.a[2][1] = 1.0f;
			coordTransform.a[1][2] = -1.0f;
			coordTransform.a[3][3] = 1.0f;
		}
	}

	// Advance animation time, evaluate pose, and compute final skinning matrices
	void update(std::string name, float dt)
	{
		if (name == usingAnimation)
		{
			t += dt;
		}
		else
		{
			usingAnimation = name;
			t = 0;
		}

		if (animationFinished() == true)
		{
			return;
		}

		int frame = 0;
		float interpolationFact = 0;
		animation->calcFrame(name, t, frame, interpolationFact);

		for (int i = 0; i < animation->bonesSize(); i++)
		{
			matrices[i] = animation->interpolateBoneToGlobal(name, matrices, frame, interpolationFact, i);
		}

		animation->calcTransforms(matrices, coordTransform);
	}

	// Force restart of the current clip
	void resetAnimationTime()
	{
		t = 0;
	}

	// Non-looping clip end condition
	bool animationFinished()
	{
		if (t > animation->animations[usingAnimation].duration())
		{
			return true;
		}
		return false;
	}

	// Compute current bone world matrix by rebuilding its parent chain pose
	Matrix findWorldMatrix(std::string boneName)
	{
		int boneID = animation->skeleton.findBone(boneName);
		std::vector<int> boneChain;

		int ID = boneID;
		while (ID != -1)
		{
			boneChain.push_back(ID);
			ID = animation->skeleton.bones[ID].parentIndex;
		}

		int frame = 0;
		float interpolationFact = 0;
		animation->calcFrame(usingAnimation, t, frame, interpolationFact);

		for (int i = boneChain.size() - 1; i > -1; i = i - 1)
		{
			matricesPose[boneChain[i]] = animation->interpolateBoneToGlobal(usingAnimation, matricesPose, frame, interpolationFact, boneChain[i]);
		}

		return (matricesPose[boneID] * coordTransform);
	}
};
