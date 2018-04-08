//
// Copyright (c) 2017 XiaoMi All rights reserved.
//

#include <vector>

#include "gtest/gtest.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/core/tensor.h"
#include "mace/core/workspace.h"
#include "mace/kernels/opencl/helper.h"
#include "mace/utils/tuner.h"
#include "mace/utils/utils.h"

namespace mace {
namespace kernels {
namespace {

const bool BufferToImageOpImpl(Tensor *buffer,
                               Tensor *image,
                               std::vector<size_t> &image_shape) {
  std::unique_ptr<BufferBase> kernel_error;
  uint32_t gws[2] = {static_cast<uint32_t>(image_shape[0]),
                     static_cast<uint32_t>(image_shape[1])};

  auto runtime = OpenCLRuntime::Global();

  std::string kernel_name = "in_out_buffer_to_image";
  std::string obfuscated_kernel_name = MACE_OBFUSCATE_SYMBOL(kernel_name);
  std::set<std::string> built_options;
  std::stringstream kernel_name_ss;
  kernel_name_ss << "-D" << kernel_name << "=" << obfuscated_kernel_name;
  built_options.emplace(kernel_name_ss.str());
  if (runtime->IsNonUniformWorkgroupsSupported()) {
    built_options.emplace("-DNON_UNIFORM_WORK_GROUP");
  }
  if (buffer->dtype() == image->dtype()) {
    built_options.emplace("-DDATA_TYPE=" + DtToCLDt(DataTypeToEnum<float>::value));
    built_options.emplace("-DCMD_DATA_TYPE=" +
                          DtToCLCMDDt(DataTypeToEnum<float>::value));
  } else {
    built_options.emplace("-DDATA_TYPE=" +
                          DtToUpstreamCLDt(DataTypeToEnum<float>::value));
    built_options.emplace("-DCMD_DATA_TYPE=" +
                          DtToUpstreamCLCMDDt(DataTypeToEnum<float>::value));
  }
  if (runtime->IsOutOfRangeCheckEnabled()) {
    built_options.emplace("-DOUT_OF_RANGE_CHECK");
    kernel_error = std::move(std::unique_ptr<Buffer>(
          new Buffer(GetDeviceAllocator(DeviceType::OPENCL), 1)));
    kernel_error->Map(nullptr);
    *(kernel_error->mutable_data<char>()) = '0';
    kernel_error->UnMap();
  }

  auto b2f_kernel = runtime->BuildKernel("buffer_to_image",
                                         obfuscated_kernel_name, built_options);

  uint32_t idx = 0;
  if (runtime->IsOutOfRangeCheckEnabled()) {
    b2f_kernel.setArg(idx++,
        *(static_cast<cl::Buffer *>(kernel_error->buffer())));
  }
  if (!runtime->IsNonUniformWorkgroupsSupported()) {
    b2f_kernel.setArg(idx++, gws[0]);
    b2f_kernel.setArg(idx++, gws[1]);
  }
  b2f_kernel.setArg(idx++, *(buffer->opencl_buffer()));
  MACE_CHECK(buffer->buffer_offset() % GetEnumTypeSize(buffer->dtype()) == 0,
             "buffer offset not aligned");
  b2f_kernel.setArg(idx++,
                    static_cast<uint32_t>(buffer->buffer_offset() /
                                          GetEnumTypeSize(buffer->dtype())));
  b2f_kernel.setArg(idx++, static_cast<uint32_t>(buffer->dim(1)));
  b2f_kernel.setArg(idx++, static_cast<uint32_t>(buffer->dim(2)));
  b2f_kernel.setArg(idx++, static_cast<uint32_t>(buffer->dim(3)));
  b2f_kernel.setArg(idx++, *(image->opencl_image()));

  const uint32_t kwg_size =
      static_cast<uint32_t>(runtime->GetKernelMaxWorkGroupSize(b2f_kernel));
  const std::vector<uint32_t> lws = {16, kwg_size / 16};

  cl::Event event;
  cl_int error;
  if (runtime->IsNonUniformWorkgroupsSupported()) {
    error = runtime->command_queue().enqueueNDRangeKernel(
        b2f_kernel, cl::NullRange, cl::NDRange(gws[0], gws[1]),
        cl::NDRange(lws[0], lws[1]), nullptr, &event);
  } else {
    std::vector<uint32_t> roundup_gws(lws.size());
    for (size_t i = 0; i < lws.size(); ++i) {
      roundup_gws[i] = RoundUp(gws[i], lws[i]);
    }

    error = runtime->command_queue().enqueueNDRangeKernel(
        b2f_kernel, cl::NullRange, cl::NDRange(roundup_gws[0], roundup_gws[1]),
        cl::NDRange(lws[0], lws[1]), nullptr, &event);
  }
  MACE_CHECK_CL_SUCCESS(error);

  runtime->command_queue().finish();
  bool is_out_of_range = false;
  if (runtime->IsOutOfRangeCheckEnabled()) {
    kernel_error->Map(nullptr);
    is_out_of_range = *(kernel_error->mutable_data<char>()) == '1' ? true : false;
    kernel_error->UnMap();
  }
  return is_out_of_range;
}

}  // namespace

class OutOfRangeCheckTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    setenv("MACE_OUT_OF_RANGE_CHECK", "1", 1);
  }
};

TEST(OutOfRangeCheckTest, RandomTest) {
  static unsigned int seed = time(NULL);
  index_t batch = 11 + rand_r(&seed) % 10;
  index_t height = 12 + rand_r(&seed) % 100;
  index_t width = 13 + rand_r(&seed) % 100;
  index_t channels = 14 + rand_r(&seed) % 50;

  std::vector<index_t> buffer_shape = {batch, height, width, channels};
  Workspace ws;
  Tensor *buffer = ws.CreateTensor("Buffer",
                                   GetDeviceAllocator(DeviceType::OPENCL),
                                   DataTypeToEnum<float>::v());
  buffer->Resize(buffer_shape);

  std::vector<size_t> image_shape;
  Tensor *image = ws.CreateTensor("Image",
                                   GetDeviceAllocator(DeviceType::OPENCL),
                                   DataTypeToEnum<float>::v());
  CalImage2DShape(buffer->shape(), IN_OUT_CHANNEL, &image_shape);
  image->ResizeImage(buffer->shape(), image_shape);
  ASSERT_FALSE(BufferToImageOpImpl(buffer, image, image_shape));

  std::vector<size_t> overflow_image_shape = image_shape;
  for (int i = 0; i < overflow_image_shape.size(); ++i) {
    overflow_image_shape[i] += 1;
  }
  ASSERT_TRUE(BufferToImageOpImpl(buffer, image, overflow_image_shape));
}

}  // namespace kernels
}  // namespace mace
