/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ExtensibleRTTI.h"
#include "xla/python/ifrt/array_spec.h"
#include "xla/python/ifrt/array_spec.pb.h"
#include "xla/python/ifrt/device.h"
#include "xla/python/ifrt/io_callable_program.h"
#include "xla/python/ifrt/io_callable_program.pb.h"
#include "xla/python/ifrt/program_serdes.h"
#include "xla/python/ifrt/serdes.h"
#include "xla/python/ifrt/sharding.pb.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace ifrt {

namespace {

// Serialization/deserialization for `IoCallableProgram`.
class IoCallableProgramSerDes
    : public llvm::RTTIExtends<IoCallableProgramSerDes, SerDes> {
 public:
  absl::string_view type_name() const override {
    return "xla::ifrt::IoCallableProgram";
  }

  absl::StatusOr<std::string> Serialize(Serializable& serializable) override {
    const IoCallableProgram& program =
        llvm::cast<IoCallableProgram>(serializable);
    IoCallableProgramProto proto;
    ;
    proto.set_type(program.type);
    proto.set_name(program.name);
    proto.set_serialized_program_text(program.serialized_program_text);
    *proto.mutable_devices() = program.devices.ToProto();
    for (const ArraySpec& spec : program.input_specs) {
      TF_ASSIGN_OR_RETURN(*proto.add_input_specs(), spec.ToProto());
    }
    for (const ArraySpec& spec : program.output_specs) {
      TF_ASSIGN_OR_RETURN(*proto.add_output_specs(), spec.ToProto());
    }
    return proto.SerializeAsString();
  }

  absl::StatusOr<std::unique_ptr<Serializable>> Deserialize(
      const std::string& serialized,
      std::unique_ptr<DeserializeOptions> options) override {
    const auto* deserialize_program_options =
        llvm::cast<DeserializeProgramOptions>(options.get());

    IoCallableProgramProto proto;
    if (!proto.ParseFromString(serialized)) {
      return absl::InvalidArgumentError(
          "Failed to parse serialized IoCallableProgramProto");
    }
    TF_ASSIGN_OR_RETURN(
        DeviceList devices,
        DeviceList::FromProto(deserialize_program_options->lookup_device,
                              proto.devices()));
    std::vector<ArraySpec> input_specs;
    input_specs.reserve(proto.input_specs_size());
    for (const ArraySpecProto& spec_proto : proto.input_specs()) {
      TF_ASSIGN_OR_RETURN(
          ArraySpec spec,
          ArraySpec::FromProto(deserialize_program_options->lookup_device,
                               spec_proto));
      input_specs.push_back(std::move(spec));
    }
    std::vector<ArraySpec> output_specs;
    output_specs.reserve(proto.output_specs_size());
    for (const ArraySpecProto& spec_proto : proto.output_specs()) {
      TF_ASSIGN_OR_RETURN(
          ArraySpec spec,
          ArraySpec::FromProto(deserialize_program_options->lookup_device,
                               spec_proto));
      output_specs.push_back(std::move(spec));
    }

    return std::make_unique<IoCallableProgram>(
        /*type=*/proto.type(), /*name=*/proto.name(),
        /*serialized_program_text=*/
        std::move(*proto.mutable_serialized_program_text()),
        /*devices=*/std::move(devices),
        /*input_specs=*/std::move(input_specs),
        /*output_specs=*/std::move(output_specs));
  }

  static char ID;  // NOLINT
};

// Serialization/deserialization for `IoCallableCompileOptions`.
class IoCallableCompileOptionsSerDes
    : public llvm::RTTIExtends<IoCallableCompileOptionsSerDes, SerDes> {
 public:
  absl::string_view type_name() const override {
    return "xla::ifrt::IoCallableCompileOptions";
  }

  absl::StatusOr<std::string> Serialize(Serializable& serializable) override {
    return "";
  }

  absl::StatusOr<std::unique_ptr<Serializable>> Deserialize(
      const std::string& serialized,
      std::unique_ptr<DeserializeOptions> options) override {
    if (!serialized.empty()) {
      return absl::InvalidArgumentError(
          "Invalid serialized IoCallableCompileOptions; a serialized "
          "IoCallableCompileOptions is expected to be an empty string");
    }
    return std::make_unique<IoCallableCompileOptions>();
  }

  static char ID;  // NOLINT
};

[[maybe_unused]] char IoCallableProgramSerDes::ID = 0;         // NOLINT
[[maybe_unused]] char IoCallableCompileOptionsSerDes::ID = 0;  // NOLINT

// clang-format off
bool register_io_callable_program_serdes = ([]{
  RegisterSerDes<IoCallableProgram>(
      std::make_unique<IoCallableProgramSerDes>());
}(), true);

bool register_io_callable_compile_options_serdes = ([]{
  RegisterSerDes<IoCallableCompileOptions>(
      std::make_unique<IoCallableCompileOptionsSerDes>());
}(), true);
// clang-format on

}  // namespace

}  // namespace ifrt
}  // namespace xla
