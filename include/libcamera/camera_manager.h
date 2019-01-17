/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * camera_manager.h - Camera management
 */
#ifndef __LIBCAMERA_CAMERA_MANAGER_H__
#define __LIBCAMERA_CAMERA_MANAGER_H__

#include <memory>
#include <string>
#include <vector>

namespace libcamera {

class Camera;
class DeviceEnumerator;
class EventDispatcher;
class PipelineHandler;

class CameraManager
{
public:
	int start();
	void stop();

	std::vector<std::string> list() const;
	Camera *get(const std::string &name);

	static CameraManager *instance();

	void setEventDispatcher(std::unique_ptr<EventDispatcher> dispatcher);
	EventDispatcher *eventDispatcher();

private:
	CameraManager();
	CameraManager(const CameraManager &) = delete;
	void operator=(const CameraManager &) = delete;
	~CameraManager();

	std::unique_ptr<DeviceEnumerator> enumerator_;
	std::vector<PipelineHandler *> pipes_;

	std::unique_ptr<EventDispatcher> dispatcher_;
};

} /* namespace libcamera */

#endif /* __LIBCAMERA_CAMERA_MANAGER_H__ */
