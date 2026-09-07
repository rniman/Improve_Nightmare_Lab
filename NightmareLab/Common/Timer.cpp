#include "Timer.h"

#include <Windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace
{
	std::int64_t ReadPerformanceCounter()
	{
		LARGE_INTEGER counter;
		::QueryPerformanceCounter(&counter);
		return counter.QuadPart;
	}
}

Timer::Timer()
{
	LARGE_INTEGER frequency;
	::QueryPerformanceFrequency(&frequency);
	mLastPerformanceCounter = ReadPerformanceCounter();
	mTimeScale = 1.0 / static_cast<double>(frequency.QuadPart);
	mBasePerformanceCounter = mLastPerformanceCounter;
}

void Timer::Tick(float lockFps)
{
	if (mStopped)
	{
		mElapsedTime = 0.0f;
		return;
	}

	mCurrentPerformanceCounter = ReadPerformanceCounter();
	float elapsedTime = static_cast<float>((mCurrentPerformanceCounter - mLastPerformanceCounter) * mTimeScale);
	if (lockFps > 0.0f)
	{
		while (elapsedTime < (1.0f / lockFps))
		{
			mCurrentPerformanceCounter = ReadPerformanceCounter();
			elapsedTime = static_cast<float>((mCurrentPerformanceCounter - mLastPerformanceCounter) * mTimeScale);
		}
	}
	mLastPerformanceCounter = mCurrentPerformanceCounter;

	if (std::fabs(elapsedTime - mElapsedTime) < 1.0f)
	{
		std::memmove(mFrameTimes.data() + 1, mFrameTimes.data(), (mFrameTimes.size() - 1) * sizeof(float));
		mFrameTimes[0] = elapsedTime;
		if (mSampleCount < mFrameTimes.size())
		{
			mSampleCount++;
		}
	}

	mFramesPerSecond++;
	mFpsTimeElapsed += elapsedTime;
	if (mFpsTimeElapsed > 1.0f)
	{
		mCurrentFrameRate = mFramesPerSecond;
		mFramesPerSecond = 0;
		mFpsTimeElapsed = 0.0f;
	}

	mElapsedTime = 0.0f;
	for (unsigned long i = 0; i < mSampleCount; i++)
	{
		mElapsedTime += mFrameTimes[i];
	}
	if (mSampleCount > 0)
	{
		mElapsedTime /= mSampleCount;
	}
}

void Timer::Start()
{
	const std::int64_t counter = ReadPerformanceCounter();
	if (mStopped)
	{
		mPausedPerformanceCounter += counter - mStopPerformanceCounter;
		mLastPerformanceCounter = counter;
		mStopPerformanceCounter = 0;
		mStopped = false;
	}
}

void Timer::Stop()
{
	if (!mStopped)
	{
		mStopPerformanceCounter = ReadPerformanceCounter();
		mStopped = true;
	}
}

void Timer::Reset()
{
	const std::int64_t counter = ReadPerformanceCounter();
	mBasePerformanceCounter = counter;
	mLastPerformanceCounter = counter;
	mStopPerformanceCounter = 0;
	mStopped = false;
}

unsigned long Timer::GetFrameRate(wchar_t* text, int characterCount) const
{
	if (text)
	{
		_itow_s(mCurrentFrameRate, text, characterCount, 10);
		wcscat_s(text, characterCount, L" FPS)");
	}
	return mCurrentFrameRate;
}

float Timer::GetTimeElapsed() const
{
	return mElapsedTime;
}

float Timer::GetTotalTime() const
{
	const std::int64_t counter = mStopped ? mStopPerformanceCounter : mCurrentPerformanceCounter;
	return static_cast<float>(((counter - mPausedPerformanceCounter) - mBasePerformanceCounter) * mTimeScale);
}
