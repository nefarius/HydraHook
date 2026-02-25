/*
MIT License

Copyright (c) 2018-2026 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "Perception.h"
#include <HydraHook/Engine/HydraHookCore.h>

#include <opencv2/features2d.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>

void RunPerceptionPipeline(cv::Mat& frame, PerceptionResults& out)
{
	const int minFeatures = 8;
	const int maxPoseTrailLen = 100;

	out.valid = false;

	if (frame.empty() || !frame.isContinuous())
	{
		HydraHookEngineLogError("HydraHook-OpenCV: RunPerceptionPipeline received empty or non-continuous frame");
		return;
	}

	static cv::Mat prevGray, currGray;
	static std::vector<cv::Point2f> prevPts, currPts;
	static cv::Ptr<cv::ORB> orb = cv::ORB::create(500);
	static std::vector<cv::Vec3f> poseTrail;
	static bool needReinit = true;

	cv::Mat gray;
	try
	{
		if (frame.channels() == 3)
			cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
		else
			gray = frame;
	}
	catch (const cv::Exception& ex)
	{
		HydraHookEngineLogError("HydraHook-OpenCV: cvtColor failed: %s (ch=%d)", ex.what(), frame.channels());
		return;
	}

	currGray = gray;

	if (needReinit || prevPts.size() < (size_t)minFeatures)
	{
		std::vector<cv::KeyPoint> kps;
		cv::Mat desc;
		orb->detectAndCompute(currGray, cv::noArray(), kps, desc);
		prevPts.clear();
		for (const auto& kp : kps)
			prevPts.push_back(kp.pt);
		prevGray = currGray.clone();
		needReinit = false;
		out.prevPts = prevPts;
		out.currPts = prevPts;
		out.featureCount = (int)prevPts.size();
		out.valid = prevPts.size() >= (size_t)minFeatures;
	}
	else
	{
		std::vector<uchar> status;
		std::vector<float> err;
		try
		{
			cv::calcOpticalFlowPyrLK(prevGray, currGray, prevPts, currPts, status, err,
				cv::Size(21, 21), 3,
				cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 30, 0.01));
		}
		catch (const cv::Exception& ex)
		{
			HydraHookEngineLogError("HydraHook-OpenCV: calcOpticalFlowPyrLK failed: %s", ex.what());
			needReinit = true;
			out.prevPts = prevPts;
			out.currPts = currPts;
			out.featureCount = (int)prevPts.size();
			out.valid = false;
			return;
		}

		std::vector<cv::Point2f> goodPrev, goodCurr;
		const size_t n = (std::min)(status.size(), (std::min)(prevPts.size(), currPts.size()));
		for (size_t i = 0; i < n; i++)
		{
			if (status[i])
			{
				goodPrev.push_back(prevPts[i]);
				goodCurr.push_back(currPts[i]);
			}
		}

		if (goodPrev.size() < (size_t)minFeatures)
		{
			needReinit = true;
			out.prevPts = prevPts;
			out.currPts = currPts;
			out.featureCount = (int)currPts.size();
			out.valid = false;
		}
		else
		{
			const int w = frame.cols;
			const int h = frame.rows;
			if (w <= 0 || h <= 0)
			{
				HydraHookEngineLogError("HydraHook-OpenCV: Invalid frame size %dx%d, skipping perception", w, h);
				needReinit = true;
				out.prevPts = prevPts;
				out.currPts = currPts;
				out.featureCount = (int)currPts.size();
				out.valid = false;
			}
			else if (goodPrev.size() != goodCurr.size())
			{
				HydraHookEngineLogError("HydraHook-OpenCV: Point count mismatch prev=%zu curr=%zu", goodPrev.size(), goodCurr.size());
				needReinit = true;
				out.prevPts = prevPts;
				out.currPts = currPts;
				out.featureCount = (int)currPts.size();
				out.valid = false;
			}
			else
			{
				double fx = (double)w;
				double fy = (double)w;
				double cx = w / 2.0;
				double cy = h / 2.0;
				cv::Mat K = (cv::Mat_<double>(3, 3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);

				cv::Mat E, R, t;
				std::vector<int> inlierMask;
				try
				{
					E = cv::findEssentialMat(goodPrev, goodCurr, K, cv::RANSAC, 0.999, 1.0, inlierMask);
				}
				catch (const cv::Exception& ex)
				{
					HydraHookEngineLogError("HydraHook-OpenCV: findEssentialMat failed: %s (prev=%zu curr=%zu)", ex.what(), goodPrev.size(), goodCurr.size());
					needReinit = true;
					out.prevPts = goodPrev;
					out.currPts = goodCurr;
					out.featureCount = (int)goodCurr.size();
					out.valid = true;
					goto skip_essential;
				}

				if (E.empty())
				{
					HydraHookEngineLogError("HydraHook-OpenCV: findEssentialMat returned empty matrix");
				}
				else
				{
					int inliers = 0;
					for (int m : inlierMask)
						if (m) inliers++;

					if (inliers >= minFeatures)
					{
						std::vector<cv::Point2f> inlierPrev, inlierCurr;
						for (size_t i = 0; i < inlierMask.size(); i++)
						{
							if (inlierMask[i])
							{
								inlierPrev.push_back(goodPrev[i]);
								inlierCurr.push_back(goodCurr[i]);
							}
						}
						try
						{
							int recovered = cv::recoverPose(E, inlierPrev, inlierCurr, K, R, t);
							if (recovered > 0 && !t.empty() && t.rows >= 3 && t.cols >= 1)
							{
								poseTrail.push_back(cv::Vec3f((float)t.at<double>(0), (float)t.at<double>(1), (float)t.at<double>(2)));
								if (poseTrail.size() > (size_t)maxPoseTrailLen)
									poseTrail.erase(poseTrail.begin());
								out.R = R;
								out.t = t;
								out.poseTrail = poseTrail;
								out.inliers = inliers;
							}
						}
						catch (const cv::Exception& ex)
						{
							HydraHookEngineLogError("HydraHook-OpenCV: recoverPose failed: %s", ex.what());
						}
					}
				}

			skip_essential:
				prevPts = currPts;
				prevGray = currGray.clone();
				out.prevPts = goodPrev;
				out.currPts = goodCurr;
				out.featureCount = (int)goodCurr.size();
				out.valid = true;
			}
		}
	}
}
