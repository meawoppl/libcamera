/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2021, Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>
 *
 * Python bindings
 */

#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <libcamera/libcamera.h>

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

namespace py = pybind11;

using namespace std;
using namespace libcamera;

template<typename T>
static py::object ValueOrTuple(const ControlValue &cv)
{
	if (cv.isArray()) {
		const T *v = reinterpret_cast<const T *>(cv.data().data());
		auto t = py::tuple(cv.numElements());

		for (size_t i = 0; i < cv.numElements(); ++i)
			t[i] = v[i];

		return std::move(t);
	}

	return py::cast(cv.get<T>());
}

static py::object ControlValueToPy(const ControlValue &cv)
{
	switch (cv.type()) {
	case ControlTypeBool:
		return ValueOrTuple<bool>(cv);
	case ControlTypeByte:
		return ValueOrTuple<uint8_t>(cv);
	case ControlTypeInteger32:
		return ValueOrTuple<int32_t>(cv);
	case ControlTypeInteger64:
		return ValueOrTuple<int64_t>(cv);
	case ControlTypeFloat:
		return ValueOrTuple<float>(cv);
	case ControlTypeString:
		return py::cast(cv.get<string>());
	case ControlTypeRectangle: {
		const Rectangle *v = reinterpret_cast<const Rectangle *>(cv.data().data());
		return py::make_tuple(v->x, v->y, v->width, v->height);
	}
	case ControlTypeSize: {
		const Size *v = reinterpret_cast<const Size *>(cv.data().data());
		return py::make_tuple(v->width, v->height);
	}
	case ControlTypeNone:
	default:
		throw runtime_error("Unsupported ControlValue type");
	}
}

template<typename T>
static ControlValue ControlValueMaybeArray(const py::object &ob)
{
	try {
		std::vector<T> vec = ob.cast<std::vector<T>>();
		return ControlValue(Span<const T>(vec));
	} catch (py::cast_error const &e) {
	}
	return ControlValue(ob.cast<T>());
}

static ControlValue PyToControlValue(const py::object &ob, ControlType type)
{
	switch (type) {
	case ControlTypeBool:
		return ControlValue(ob.cast<bool>());
	case ControlTypeByte:
		return ControlValueMaybeArray<uint8_t>(ob);
	case ControlTypeInteger32:
		return ControlValueMaybeArray<int32_t>(ob);
	case ControlTypeInteger64:
		return ControlValueMaybeArray<int64_t>(ob);
	case ControlTypeFloat:
		return ControlValueMaybeArray<float>(ob);
	case ControlTypeString:
		return ControlValue(ob.cast<string>());
	case ControlTypeRectangle: {
		std::array<int32_t, 4> array = ob.cast<std::array<int32_t, 4>>();
		return ControlValue(Rectangle(array[0], array[1], array[2], array[3]));
	}
	case ControlTypeSize: {
		std::array<int32_t, 2> array = ob.cast<std::array<int32_t, 2>>();
		return ControlValue(Size(array[0], array[1]));
	}
	case ControlTypeNone:
	default:
		throw runtime_error("Control type not implemented");
	}
}

static weak_ptr<CameraManager> g_camera_manager;
static int g_eventfd;
static mutex g_reqlist_mutex;
static vector<Request *> g_reqlist;

static void handleRequestCompleted(Request *req)
{
	{
		lock_guard guard(g_reqlist_mutex);
		g_reqlist.push_back(req);
	}

	uint64_t v = 1;
	write(g_eventfd, &v, 8);
}

PYBIND11_MODULE(_libcamera, m)
{
	m.def("logSetLevel", &logSetLevel);

	py::class_<CameraManager, std::shared_ptr<CameraManager>>(m, "CameraManager")
		.def_static("singleton", []() {
			shared_ptr<CameraManager> cm = g_camera_manager.lock();
			if (cm)
				return cm;

			int fd = eventfd(0, 0);
			if (fd == -1)
				throw std::system_error(errno, std::generic_category(), "Failed to create eventfd");

			cm = shared_ptr<CameraManager>(new CameraManager, [](auto p) {
				close(g_eventfd);
				g_eventfd = -1;
				delete p;
			});

			g_eventfd = fd;
			g_camera_manager = cm;

			int ret = cm->start();
			if (ret)
				throw std::system_error(-ret, std::generic_category(), "Failed to start CameraManager");

			return cm;
		})

		.def_property_readonly("version", &CameraManager::version)

		.def_property_readonly("efd", [](CameraManager &) {
			return g_eventfd;
		})

		.def("getReadyRequests", [](CameraManager &) {
			vector<Request *> v;

			{
				lock_guard guard(g_reqlist_mutex);
				swap(v, g_reqlist);
			}

			vector<py::object> ret;

			for (Request *req : v) {
				py::object o = py::cast(req);
				/* decrease the ref increased in Camera::queueRequest() */
				o.dec_ref();
				ret.push_back(o);
			}

			return ret;
		})

		.def("get", py::overload_cast<const string &>(&CameraManager::get), py::keep_alive<0, 1>())

		.def("find", [](CameraManager &self, string str) {
			std::transform(str.begin(), str.end(), str.begin(), ::tolower);

			for (auto c : self.cameras()) {
				string id = c->id();

				std::transform(id.begin(), id.end(), id.begin(), ::tolower);

				if (id.find(str) != string::npos)
					return c;
			}

			return shared_ptr<Camera>();
		}, py::keep_alive<0, 1>())

		/* Create a list of Cameras, where each camera has a keep-alive to CameraManager */
		.def_property_readonly("cameras", [](CameraManager &self) {
			py::list l;

			for (auto &c : self.cameras()) {
				py::object py_cm = py::cast(self);
				py::object py_cam = py::cast(c);
				py::detail::keep_alive_impl(py_cam, py_cm);
				l.append(py_cam);
			}

			return l;
		});

	py::class_<Camera, shared_ptr<Camera>>(m, "Camera")
		.def_property_readonly("id", &Camera::id)
		.def("acquire", &Camera::acquire)
		.def("release", &Camera::release)
		.def("start", [](shared_ptr<Camera> &self, py::dict controls) {
			self->requestCompleted.connect(handleRequestCompleted);

			const ControlInfoMap &controlMap = self->controls();
			ControlList controlList(controlMap);
			for (std::pair<py::handle, py::handle> item : controls) {
				auto key = item.first.cast<std::string>();

				auto it = find_if(controlMap.begin(), controlMap.end(),
						  [&key](const auto &kvp) {
							  return kvp.first->name() == key; });

				if (it == controlMap.end())
					throw runtime_error("Control " + key + " not found");

				const auto &id = it->first;
				auto obj = py::cast<py::object>(item.second);

				controlList.set(id->id(), PyToControlValue(obj, id->type()));
			}

			int ret = self->start(&controlList);
			if (ret)
				self->requestCompleted.disconnect(handleRequestCompleted);

			return ret;
		},
		py::arg("controls") = py::dict())

		.def("stop", [](shared_ptr<Camera> &self) {
			int ret = self->stop();
			if (!ret)
				self->requestCompleted.disconnect(handleRequestCompleted);

			return ret;
		})

		.def("__repr__", [](shared_ptr<Camera> &self) {
			return "<libcamera.Camera '" + self->id() + "'>";
		})

		/* Keep the camera alive, as StreamConfiguration contains a Stream* */
		.def("generateConfiguration", &Camera::generateConfiguration, py::keep_alive<0, 1>())
		.def("configure", &Camera::configure)

		.def("createRequest", &Camera::createRequest, py::arg("cookie") = 0)

		.def("queueRequest", [](Camera &self, Request *req) {
			py::object py_req = py::cast(req);

			py_req.inc_ref();

			int ret = self.queueRequest(req);
			if (ret)
				py_req.dec_ref();

			return ret;
		})

		.def_property_readonly("streams", [](Camera &self) {
			py::set set;
			for (auto &s : self.streams()) {
				py::object py_self = py::cast(self);
				py::object py_s = py::cast(s);
				py::detail::keep_alive_impl(py_s, py_self);
				set.add(py_s);
			}
			return set;
		})

		.def_property_readonly("controls", [](Camera &self) {
			py::dict ret;

			for (const auto &[id, ci] : self.controls()) {
				ret[id->name().c_str()] = make_tuple<py::object>(ControlValueToPy(ci.min()),
										 ControlValueToPy(ci.max()),
										 ControlValueToPy(ci.def()));
			}

			return ret;
		})

		.def_property_readonly("properties", [](Camera &self) {
			py::dict ret;

			for (const auto &[key, cv] : self.properties()) {
				const ControlId *id = properties::properties.at(key);
				py::object ob = ControlValueToPy(cv);

				ret[id->name().c_str()] = ob;
			}

			return ret;
		});

	py::enum_<CameraConfiguration::Status>(m, "ConfigurationStatus")
		.value("Valid", CameraConfiguration::Valid)
		.value("Adjusted", CameraConfiguration::Adjusted)
		.value("Invalid", CameraConfiguration::Invalid);

	py::class_<CameraConfiguration>(m, "CameraConfiguration")
		.def("__iter__", [](CameraConfiguration &self) {
			return py::make_iterator<py::return_value_policy::reference_internal>(self);
		}, py::keep_alive<0, 1>())
		.def("__len__", [](CameraConfiguration &self) {
			return self.size();
		})
		.def("validate", &CameraConfiguration::validate)
		.def("at", py::overload_cast<unsigned int>(&CameraConfiguration::at), py::return_value_policy::reference_internal)
		.def_property_readonly("size", &CameraConfiguration::size)
		.def_property_readonly("empty", &CameraConfiguration::empty)
		.def_readwrite("transform", &CameraConfiguration::transform);

	py::class_<StreamConfiguration>(m, "StreamConfiguration")
		.def("toString", &StreamConfiguration::toString)
		.def_property_readonly("stream", &StreamConfiguration::stream, py::return_value_policy::reference_internal)
		.def_property(
			"size",
			[](StreamConfiguration &self) { return make_tuple(self.size.width, self.size.height); },
			[](StreamConfiguration &self, tuple<uint32_t, uint32_t> size) { self.size.width = get<0>(size); self.size.height = get<1>(size); })
		.def_property(
			"pixelFormat",
			[](StreamConfiguration &self) { return self.pixelFormat.toString(); },
			[](StreamConfiguration &self, string fmt) { self.pixelFormat = PixelFormat::fromString(fmt); })
		.def_readwrite("stride", &StreamConfiguration::stride)
		.def_readwrite("frameSize", &StreamConfiguration::frameSize)
		.def_readwrite("bufferCount", &StreamConfiguration::bufferCount)
		.def_property_readonly("formats", &StreamConfiguration::formats, py::return_value_policy::reference_internal)
		.def_property("colorSpace",
			      [](StreamConfiguration &self) {
				      py::object ret;
				      if (self.colorSpace)
					      ret = py::cast(*self.colorSpace);
				      else
					      ret = py::none();
				      return ret;
			      },
			      [](StreamConfiguration &self, py::object colorSpace) {
				      if (colorSpace.is(py::none()))
					      self.colorSpace.reset();
				      else
					      self.colorSpace = colorSpace.cast<ColorSpace>();
			      });
	;

	py::class_<StreamFormats>(m, "StreamFormats")
		.def_property_readonly("pixelFormats", [](StreamFormats &self) {
			vector<string> fmts;
			for (auto &fmt : self.pixelformats())
				fmts.push_back(fmt.toString());
			return fmts;
		})
		.def("sizes", [](StreamFormats &self, const string &pixelFormat) {
			auto fmt = PixelFormat::fromString(pixelFormat);
			vector<tuple<uint32_t, uint32_t>> fmts;
			for (const auto &s : self.sizes(fmt))
				fmts.push_back(make_tuple(s.width, s.height));
			return fmts;
		})
		.def("range", [](StreamFormats &self, const string &pixelFormat) {
			auto fmt = PixelFormat::fromString(pixelFormat);
			const auto &range = self.range(fmt);
			return make_tuple(make_tuple(range.hStep, range.vStep),
					  make_tuple(range.min.width, range.min.height),
					  make_tuple(range.max.width, range.max.height));
		});

	py::enum_<StreamRole>(m, "StreamRole")
		.value("StillCapture", StreamRole::StillCapture)
		.value("Raw", StreamRole::Raw)
		.value("VideoRecording", StreamRole::VideoRecording)
		.value("Viewfinder", StreamRole::Viewfinder);

	py::class_<FrameBufferAllocator>(m, "FrameBufferAllocator")
		.def(py::init<shared_ptr<Camera>>(), py::keep_alive<1, 2>())
		.def("allocate", &FrameBufferAllocator::allocate)
		.def_property_readonly("allocated", &FrameBufferAllocator::allocated)
		/* Create a list of FrameBuffers, where each FrameBuffer has a keep-alive to FrameBufferAllocator */
		.def("buffers", [](FrameBufferAllocator &self, Stream *stream) {
			py::object py_self = py::cast(self);
			py::list l;
			for (auto &ub : self.buffers(stream)) {
				py::object py_buf = py::cast(ub.get(), py::return_value_policy::reference_internal, py_self);
				l.append(py_buf);
			}
			return l;
		});

	py::class_<FrameBuffer>(m, "FrameBuffer")
		/* TODO: implement FrameBuffer::Plane properly */
		.def(py::init([](vector<tuple<int, unsigned int>> planes, unsigned int cookie) {
			vector<FrameBuffer::Plane> v;
			for (const auto &t : planes)
				v.push_back({ SharedFD(get<0>(t)), FrameBuffer::Plane::kInvalidOffset, get<1>(t) });
			return new FrameBuffer(v, cookie);
		}))
		.def_property_readonly("metadata", &FrameBuffer::metadata, py::return_value_policy::reference_internal)
		.def("length", [](FrameBuffer &self, uint32_t idx) {
			const FrameBuffer::Plane &plane = self.planes()[idx];
			return plane.length;
		})
		.def("fd", [](FrameBuffer &self, uint32_t idx) {
			const FrameBuffer::Plane &plane = self.planes()[idx];
			return plane.fd.get();
		})
		.def_property("cookie", &FrameBuffer::cookie, &FrameBuffer::setCookie);

	py::class_<Stream>(m, "Stream")
		.def_property_readonly("configuration", &Stream::configuration);

	py::enum_<Request::ReuseFlag>(m, "ReuseFlag")
		.value("Default", Request::ReuseFlag::Default)
		.value("ReuseBuffers", Request::ReuseFlag::ReuseBuffers);

	py::class_<Request>(m, "Request")
		/* Fence is not supported, so we cannot expose addBuffer() directly */
		.def("addBuffer", [](Request &self, const Stream *stream, FrameBuffer *buffer) {
			return self.addBuffer(stream, buffer);
		}, py::keep_alive<1, 3>()) /* Request keeps Framebuffer alive */
		.def_property_readonly("status", &Request::status)
		.def_property_readonly("buffers", &Request::buffers)
		.def_property_readonly("cookie", &Request::cookie)
		.def_property_readonly("hasPendingBuffers", &Request::hasPendingBuffers)
		.def("set_control", [](Request &self, string &control, py::object value) {
			const auto &controls = self.camera()->controls();

			auto it = find_if(controls.begin(), controls.end(),
					  [&control](const auto &kvp) { return kvp.first->name() == control; });

			if (it == controls.end())
				throw runtime_error("Control not found");

			const auto &id = it->first;

			self.controls().set(id->id(), PyToControlValue(value, id->type()));
		})
		.def_property_readonly("metadata", [](Request &self) {
			py::dict ret;

			for (const auto &[key, cv] : self.metadata()) {
				const ControlId *id = controls::controls.at(key);
				py::object ob = ControlValueToPy(cv);

				ret[id->name().c_str()] = ob;
			}

			return ret;
		})
		/* As we add a keep_alive to the fb in addBuffers(), we can only allow reuse with ReuseBuffers. */
		.def("reuse", [](Request &self) { self.reuse(Request::ReuseFlag::ReuseBuffers); });

	py::enum_<Request::Status>(m, "RequestStatus")
		.value("Pending", Request::RequestPending)
		.value("Complete", Request::RequestComplete)
		.value("Cancelled", Request::RequestCancelled);

	py::enum_<FrameMetadata::Status>(m, "FrameMetadataStatus")
		.value("Success", FrameMetadata::FrameSuccess)
		.value("Error", FrameMetadata::FrameError)
		.value("Cancelled", FrameMetadata::FrameCancelled);

	py::class_<FrameMetadata>(m, "FrameMetadata")
		.def_readonly("status", &FrameMetadata::status)
		.def_readonly("sequence", &FrameMetadata::sequence)
		.def_readonly("timestamp", &FrameMetadata::timestamp)
		/* temporary helper, to be removed */
		.def_property_readonly("bytesused", [](FrameMetadata &self) {
			vector<unsigned int> v;
			v.resize(self.planes().size());
			transform(self.planes().begin(), self.planes().end(), v.begin(), [](const auto &p) { return p.bytesused; });
			return v;
		});

	py::class_<Transform>(m, "Transform")
		.def(py::init([](int rotation, int hflip, int vflip, int transpose) {
			Transform t = transformFromRotation(rotation);
			if (hflip)
				t ^= Transform::HFlip;
			if (vflip)
				t ^= Transform::VFlip;
			if (transpose)
				t ^= Transform::Transpose;
			return t;
		}),
		py::arg("rotation") = 0,
		py::arg("hflip") = 0,
		py::arg("vflip") = 0,
		py::arg("transpose") = 0)
		.def(py::init([](Transform &other) { return other; }))
		.def("__repr__", [](Transform &self) {
			return "<libcamera.Transform '" + std::string(transformToString(self)) + "'>";
		})
		.def_property("hflip",
			      [](Transform &self) {
				      return !!(self & Transform::HFlip);
			      },
			      [](Transform &self, int hflip) {
				      if (hflip)
					      self |= Transform::HFlip;
				      else
					      self &= ~Transform::HFlip;
			      })
		.def_property("vflip",
			      [](Transform &self) {
				      return !!(self & Transform::VFlip);
			      },
			      [](Transform &self, int vflip) {
				      if (vflip)
					      self |= Transform::VFlip;
				      else
					      self &= ~Transform::VFlip;
			      })
		.def_property("transpose",
			      [](Transform &self) {
				      return !!(self & Transform::Transpose);
			      },
			      [](Transform &self, int transpose) {
				      if (transpose)
					      self |= Transform::Transpose;
				      else
					      self &= ~Transform::Transpose;
			      })
		.def("inverse", [](Transform &self) { return -self; })
		.def("invert", [](Transform &self) {
			self = -self;
			return py::none();
		})
		.def("compose", [](Transform &self, Transform &other) {
			self = self * other;
			return py::none();
		});

	py::enum_<ColorSpace::Primaries>(m, "Primaries")
		.value("Raw", ColorSpace::Primaries::Raw)
		.value("Smpte170m", ColorSpace::Primaries::Smpte170m)
		.value("Rec709", ColorSpace::Primaries::Rec709)
		.value("Rec2020", ColorSpace::Primaries::Rec2020);

	py::enum_<ColorSpace::TransferFunction>(m, "TransferFunction")
		.value("Linear", ColorSpace::TransferFunction::Linear)
		.value("Srgb", ColorSpace::TransferFunction::Srgb)
		.value("Rec709", ColorSpace::TransferFunction::Rec709);

	py::enum_<ColorSpace::YcbcrEncoding>(m, "YcbcrEncoding")
		.value("None", ColorSpace::YcbcrEncoding::None)
		.value("Rec601", ColorSpace::YcbcrEncoding::Rec601)
		.value("Rec709", ColorSpace::YcbcrEncoding::Rec709)
		.value("Rec2020", ColorSpace::YcbcrEncoding::Rec2020);

	py::enum_<ColorSpace::Range>(m, "Range")
		.value("Full", ColorSpace::Range::Full)
		.value("Limited", ColorSpace::Range::Limited);

	py::class_<ColorSpace>(m, "ColorSpace")
		.def(py::init([](ColorSpace::Primaries primaries,
				 ColorSpace::TransferFunction transferFunction,
				 ColorSpace::YcbcrEncoding ycbcrEncoding,
				 ColorSpace::Range range) {
			return ColorSpace(primaries, transferFunction, ycbcrEncoding, range);
		}),
		py::arg("primaries"),
		py::arg("transferFunction"),
		py::arg("ycbcrEncoding"),
		py::arg("range"))
		.def(py::init([](ColorSpace &other) { return other; }))
		.def("__repr__", [](ColorSpace &self) {
			return "<libcamera.ColorSpace '" + self.toString() + "'>";
		})
		.def_property("primaries",
			      [](ColorSpace &self) { return self.primaries; },
			      [](ColorSpace &self, ColorSpace::Primaries primaries) {
				      self.primaries = primaries;
			      })
		.def_property("transferFunction",
			      [](ColorSpace &self) { return self.transferFunction; },
			      [](ColorSpace &self, ColorSpace::TransferFunction transferFunction) {
				      self.transferFunction = transferFunction;
			      })
		.def_property("ycbcrEncoding",
			      [](ColorSpace &self) { return self.ycbcrEncoding; },
			      [](ColorSpace &self, ColorSpace::YcbcrEncoding ycbcrEncoding) {
				      self.ycbcrEncoding = ycbcrEncoding;
			      })
		.def_property("range",
			      [](ColorSpace &self) { return self.range; },
			      [](ColorSpace &self, ColorSpace::Range range) {
				      self.range = range;
			      })
		.def_static("Raw", []() { return ColorSpace::Raw; })
		.def_static("Jpeg", []() { return ColorSpace::Jpeg; })
		.def_static("Srgb", []() { return ColorSpace::Srgb; })
		.def_static("Smpte170m", []() { return ColorSpace::Smpte170m; })
		.def_static("Rec709", []() { return ColorSpace::Rec709; })
		.def_static("Rec2020", []() { return ColorSpace::Rec2020; });

	py::enum_<libcamera::controls::AeMeteringModeEnum>(m, "AeMeteringMode")
		.value("CentreWeighted", libcamera::controls::MeteringCentreWeighted)
		.value("Spot", libcamera::controls::MeteringSpot)
		.value("Matrix", libcamera::controls::MeteringMatrix)
		.value("Custom", libcamera::controls::MeteringCustom);

	py::enum_<libcamera::controls::AeConstraintModeEnum>(m, "AeConstraintMode")
		.value("Normal", libcamera::controls::ConstraintNormal)
		.value("Highlight", libcamera::controls::ConstraintHighlight)
		.value("Shadows", libcamera::controls::ConstraintShadows)
		.value("Custom", libcamera::controls::ConstraintCustom);

	py::enum_<libcamera::controls::AeExposureModeEnum>(m, "AeExposureMode")
		.value("Normal", libcamera::controls::ExposureNormal)
		.value("Short", libcamera::controls::ExposureShort)
		.value("Long", libcamera::controls::ExposureLong)
		.value("Custom", libcamera::controls::ExposureCustom);

	py::enum_<libcamera::controls::AwbModeEnum>(m, "AwbMode")
		.value("Auto", libcamera::controls::AwbAuto)
		.value("Incandescent", libcamera::controls::AwbIncandescent)
		.value("Tungsten", libcamera::controls::AwbTungsten)
		.value("Fluorescent", libcamera::controls::AwbFluorescent)
		.value("Indoor", libcamera::controls::AwbIndoor)
		.value("Daylight", libcamera::controls::AwbDaylight)
		.value("Cloudy", libcamera::controls::AwbCloudy)
		.value("Custom", libcamera::controls::AwbCustom);

	py::enum_<libcamera::controls::draft::AePrecaptureTriggerEnum>(m, "AePrecaptureTrigger")
		.value("Idle", libcamera::controls::draft::AePrecaptureTriggerIdle)
		.value("Start", libcamera::controls::draft::AePrecaptureTriggerStart)
		.value("Cancel", libcamera::controls::draft::AePrecaptureTriggerCancel);

	py::enum_<libcamera::controls::draft::AfTriggerEnum>(m, "AfTrigger")
		.value("Idle", libcamera::controls::draft::AfTriggerIdle)
		.value("Start", libcamera::controls::draft::AfTriggerStart)
		.value("Cancel", libcamera::controls::draft::AfTriggerCancel);

	py::enum_<libcamera::controls::draft::NoiseReductionModeEnum>(m, "NoiseReductionMode")
		.value("Off", libcamera::controls::draft::NoiseReductionModeOff)
		.value("Fast", libcamera::controls::draft::NoiseReductionModeFast)
		.value("HighQuality", libcamera::controls::draft::NoiseReductionModeHighQuality)
		.value("Minimal", libcamera::controls::draft::NoiseReductionModeMinimal)
		.value("ZSL", libcamera::controls::draft::NoiseReductionModeZSL);

	py::enum_<libcamera::controls::draft::ColorCorrectionAberrationModeEnum>(m, "ColorCorrectionAberrationMode")
		.value("Off", libcamera::controls::draft::ColorCorrectionAberrationOff)
		.value("Fast", libcamera::controls::draft::ColorCorrectionAberrationFast)
		.value("HighQuality", libcamera::controls::draft::ColorCorrectionAberrationHighQuality);

	py::enum_<libcamera::controls::draft::AeStateEnum>(m, "AeState")
		.value("Inactive", libcamera::controls::draft::AeStateInactive)
		.value("Searching", libcamera::controls::draft::AeStateSearching)
		.value("Converged", libcamera::controls::draft::AeStateConverged)
		.value("Locked", libcamera::controls::draft::AeStateLocked)
		.value("FlashRequired", libcamera::controls::draft::AeStateFlashRequired)
		.value("Precapture", libcamera::controls::draft::AeStatePrecapture);

	py::enum_<libcamera::controls::draft::AfStateEnum>(m, "AfState")
		.value("Inactive", libcamera::controls::draft::AfStateInactive)
		.value("PassiveScan", libcamera::controls::draft::AfStatePassiveScan)
		.value("PassiveFocused", libcamera::controls::draft::AfStatePassiveFocused)
		.value("ActiveScan", libcamera::controls::draft::AfStateActiveScan)
		.value("FocusedLock", libcamera::controls::draft::AfStateFocusedLock)
		.value("NotFocusedLock", libcamera::controls::draft::AfStateNotFocusedLock)
		.value("PassiveUnfocused", libcamera::controls::draft::AfStatePassiveUnfocused);

	py::enum_<libcamera::controls::draft::AwbStateEnum>(m, "AwbState")
		.value("StateInactive", libcamera::controls::draft::AwbStateInactive)
		.value("StateSearching", libcamera::controls::draft::AwbStateSearching)
		.value("Converged", libcamera::controls::draft::AwbConverged)
		.value("Locked", libcamera::controls::draft::AwbLocked);

	py::enum_<libcamera::controls::draft::LensShadingMapModeEnum>(m, "LensShadingMapMode")
		.value("Off", libcamera::controls::draft::LensShadingMapModeOff)
		.value("On", libcamera::controls::draft::LensShadingMapModeOn);

	py::enum_<libcamera::controls::draft::SceneFlickerEnum>(m, "SceneFlicker")
		.value("Off", libcamera::controls::draft::SceneFickerOff)
		.value("50Hz", libcamera::controls::draft::SceneFicker50Hz)
		.value("60Hz", libcamera::controls::draft::SceneFicker60Hz);

	py::enum_<libcamera::controls::draft::TestPatternModeEnum>(m, "TestPatternMode")
		.value("Off", libcamera::controls::draft::TestPatternModeOff)
		.value("SolidColor", libcamera::controls::draft::TestPatternModeSolidColor)
		.value("ColorBars", libcamera::controls::draft::TestPatternModeColorBars)
		.value("ColorBarsFadeToGray", libcamera::controls::draft::TestPatternModeColorBarsFadeToGray)
		.value("Pn9", libcamera::controls::draft::TestPatternModePn9)
		.value("Custom1", libcamera::controls::draft::TestPatternModeCustom1);
}