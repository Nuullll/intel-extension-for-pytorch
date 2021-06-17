#include <runtime/Exception.h>
#include <runtime/Device.h>

using namespace at;

namespace xpu {
namespace dpcpp {

DeviceIndex device_count() noexcept {
  int count;
  int err = dpcppGetDeviceCount(&count);
  return (err == DPCPP_SUCCESS) ? static_cast<DeviceIndex>(count) : 0;
}

DeviceIndex current_device() {
  DeviceIndex cur_device;
  AT_DPCPP_CHECK(dpcppGetDevice(&cur_device));
  return static_cast<DeviceIndex>(cur_device);
}

void set_device(DeviceIndex device) {
  AT_DPCPP_CHECK(dpcppSetDevice(static_cast<int>(device)));
}

DeviceIndex get_devie_index_from_ptr(void *ptr) {
  DeviceIndex device_index;
  AT_DPCPP_CHECK(dpcppGetDeviceIdFromPtr(&device_index, ptr));
  return device_index;
}

} // namespace dpcpp
} // namespace xpu
