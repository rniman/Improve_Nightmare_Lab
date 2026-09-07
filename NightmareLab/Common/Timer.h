#pragma once

#include <array>
#include <cstdint>

class Timer
{
public:
	Timer();
	virtual ~Timer() = default;

	/// @brief 경과 시간을 샘플 평균으로 갱신한다. lockFps가 양수이면 busy wait로 제한한다.
	void Tick(float lockFps = 0.0f);
	void Start();
	void Stop();
	/// @brief 기준 시각을 재설정한다. 기존 샘플과 누적 정지 시간은 유지한다.
	void Reset();

	unsigned long GetFrameRate(wchar_t* text = nullptr, int characterCount = 0) const;
	float GetTimeElapsed() const;
	float GetTotalTime() const;

private:
	double mTimeScale = 0.0;
	float mElapsedTime = 0.0f;
	std::int64_t mBasePerformanceCounter = 0;
	std::int64_t mPausedPerformanceCounter = 0;
	std::int64_t mStopPerformanceCounter = 0;
	std::int64_t mCurrentPerformanceCounter = 0;
	std::int64_t mLastPerformanceCounter = 0;

	std::array<float, 50> mFrameTimes = {};
	unsigned long mSampleCount = 0;
	unsigned long mCurrentFrameRate = 0;
	unsigned long mFramesPerSecond = 0;
	float mFpsTimeElapsed = 0.0f;
	bool mStopped = false;
};
