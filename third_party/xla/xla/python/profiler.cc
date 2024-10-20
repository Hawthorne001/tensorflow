/* Copyright 2020 The OpenXLA Authors.

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

#include "xla/python/profiler.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "nanobind/nanobind.h"
#include "nanobind/stl/string.h"       // IWYU pragma: keep
#include "nanobind/stl/string_view.h"  // IWYU pragma: keep
#include "nanobind/stl/unique_ptr.h"   // IWYU pragma: keep
#include "absl/strings/str_cat.h"
#include "xla/backends/profiler/plugin/plugin_tracer.h"
#include "xla/backends/profiler/plugin/profiler_c_api.h"
#include "xla/pjrt/c/pjrt_c_api.h"
#include "xla/pjrt/c/pjrt_c_api_profiler_extension.h"
#include "xla/pjrt/exceptions.h"
#include "xla/pjrt/status_casters.h"
#include "xla/python/xplane_to_profile_instructions.h"
#include "tsl/platform/macros.h"
#include "tsl/profiler/lib/profiler_factory.h"
#include "tsl/profiler/lib/profiler_interface.h"
#include "tsl/profiler/lib/profiler_session.h"
#include "tsl/profiler/lib/traceme.h"
#include "tsl/profiler/rpc/client/capture_profile.h"
#include "tsl/profiler/rpc/profiler_server.h"

namespace xla {

namespace nb = nanobind;

namespace {

// Wraps TraceMe with an interface that takes python types.
class TraceMeWrapper {
 public:
  // nb::str and nb::kwargs are taken by const reference to avoid
  // python reference-counting overhead.
  TraceMeWrapper(const nb::str& name, const nb::kwargs& kwargs)
      : traceme_(
            [&]() {
              std::string name_and_metadata = nb::cast<std::string>(name);
              if (kwargs.size() > 0) {
                AppendMetadata(&name_and_metadata, kwargs);
              }
              return name_and_metadata;
            },
            /*level=*/1) {}

  // nb::kwargs is taken by const reference to avoid python
  // reference-counting overhead.
  void SetMetadata(const nb::kwargs& kwargs) {
    if (TF_PREDICT_FALSE(kwargs.size() > 0)) {
      traceme_.AppendMetadata([&]() {
        std::string metadata;
        AppendMetadata(&metadata, kwargs);
        return metadata;
      });
    }
  }

  void Stop() { traceme_.Stop(); }

  static bool IsEnabled() { return tsl::profiler::TraceMe::Active(); }

 private:
  // Converts kwargs to strings and appends them to name encoded as TraceMe
  // metadata.
  static void AppendMetadata(std::string* name, const nb::kwargs& kwargs) {
    name->push_back('#');
    for (const auto& kv : kwargs) {
      absl::StrAppend(name, nb::cast<std::string_view>(kv.first), "=",
                      EncodePyObject(kv.second), ",");
    }
    name->back() = '#';
  }

  static std::string EncodePyObject(nb::handle handle) {
    if (nb::isinstance<nb::bool_>(handle)) {
      return nb::cast<bool>(handle) ? "1" : "0";
    }
    return nb::cast<std::string>(nb::str(handle));
  }

  tsl::profiler::TraceMe traceme_;
};

tensorflow::ProfileOptions DefaultPythonProfileOptions() {
  tensorflow::ProfileOptions options = tsl::ProfilerSession::DefaultOptions();
  options.set_python_tracer_level(1);
  options.set_enable_hlo_proto(true);
  return options;
}

const PLUGIN_Profiler_Api* FindProfilerApi(const PJRT_Api* pjrt_api) {
  const PJRT_Extension_Base* next =
      reinterpret_cast<const PJRT_Extension_Base*>(pjrt_api->extension_start);
  while (next != nullptr &&
         next->type != PJRT_Extension_Type::PJRT_Extension_Type_Profiler) {
    next = next->next;
  }
  if (next == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<const PJRT_Profiler_Extension*>(next)->profiler_api;
}
}  // namespace

// nanobind requires in-place construction of types, but tsl::ProfilerSession
// can only be created by its factory function. No matter, we just box it
// ourselves.
struct ProfilerSessionWrapper {
  explicit ProfilerSessionWrapper(std::unique_ptr<tsl::ProfilerSession> session)
      : session(std::move(session)) {}

  std::unique_ptr<tsl::ProfilerSession> session;
};

void BuildProfilerSubmodule(nb::module_& m) {
  nb::module_ profiler =
      m.def_submodule("profiler", "TensorFlow profiler integration");
  nb::class_<tsl::profiler::ProfilerServer> profiler_server_class(
      profiler, "ProfilerServer");
  profiler.def(
      "start_server",
      [](int port) -> std::unique_ptr<tsl::profiler::ProfilerServer> {
        auto server = std::make_unique<tsl::profiler::ProfilerServer>();
        server->StartProfilerServer(port);
        return server;
      },
      nb::arg("port"));
  profiler.def("register_plugin_profiler", [](nb::capsule c_api) -> void {
    if (std::string_view(c_api.name()) != "pjrt_c_api") {
      throw xla::XlaRuntimeError(
          "Argument to register_plugin_profiler was not a pjrt_c_api capsule.");
    }
    const PLUGIN_Profiler_Api* profiler_api =
        FindProfilerApi(static_cast<const PJRT_Api*>(c_api.data()));
    std::function<std::unique_ptr<tsl::profiler::ProfilerInterface>(
        const tensorflow::ProfileOptions&)>
        create_func = [profiler_api = profiler_api](
                          const tensorflow::ProfileOptions& options) mutable {
          return std::make_unique<xla::profiler::PluginTracer>(profiler_api,
                                                               options);
        };
    tsl::profiler::RegisterProfilerFactory(std::move(create_func));
  });

  nb::class_<ProfilerSessionWrapper> profiler_session_class(profiler,
                                                            "ProfilerSession");
  profiler_session_class
      .def("__init__",
           [](ProfilerSessionWrapper* wrapper) {
             new (wrapper) ProfilerSessionWrapper(
                 tsl::ProfilerSession::Create(DefaultPythonProfileOptions()));
           })
      .def("__init__",
           [](ProfilerSessionWrapper* wrapper,
              const tensorflow::ProfileOptions& options) {
             new (wrapper)
                 ProfilerSessionWrapper(tsl::ProfilerSession::Create(options));
           })
      .def("stop_and_export",
           [](ProfilerSessionWrapper* sess,
              const std::string& tensorboard_dir) -> void {
             tensorflow::profiler::XSpace xspace;
             // Disables the ProfilerSession
             xla::ThrowIfError(sess->session->CollectData(&xspace));
             xla::ThrowIfError(tsl::profiler::ExportToTensorBoard(
                 xspace, tensorboard_dir, /* also_export_trace_json= */ true));
           })
      .def("stop",
           [](ProfilerSessionWrapper* sess) -> nb::bytes {
             tensorflow::profiler::XSpace xspace;
             // Disables the ProfilerSession
             xla::ThrowIfError(sess->session->CollectData(&xspace));
             std::string xspace_str = xspace.SerializeAsString();
             return nb::bytes(xspace_str.data(), xspace_str.size());
           })
      .def("export",
           [](ProfilerSessionWrapper* sess, nb::bytes xspace,
              const std::string& tensorboard_dir) -> void {
             tensorflow::profiler::XSpace xspace_proto;
             // TODO(phawkins): change to std::string_view when protobuf is
             // updated in XLA.
             xspace_proto.ParseFromString(
                 std::string(xspace.c_str(), xspace.size()));
             xla::ThrowIfError(tsl::profiler::ExportToTensorBoard(
                 xspace_proto, tensorboard_dir,
                 /* also_export_trace_json= */ true));
           });

  nb::class_<tensorflow::ProfileOptions> profile_options_class(
      profiler, "ProfileOptions");
  profile_options_class
      .def("__init__",
           [](tensorflow::ProfileOptions* options) {
             new (options)
                 tensorflow::ProfileOptions(DefaultPythonProfileOptions());
           })
      .def_prop_rw("include_dataset_ops",
                   &tensorflow::ProfileOptions::include_dataset_ops,
                   &tensorflow::ProfileOptions::set_include_dataset_ops)
      .def_prop_rw("host_tracer_level",
                   &tensorflow::ProfileOptions::host_tracer_level,
                   &tensorflow::ProfileOptions::set_host_tracer_level)
      .def_prop_rw("python_tracer_level",
                   &tensorflow::ProfileOptions::python_tracer_level,
                   &tensorflow::ProfileOptions::set_python_tracer_level)
      .def_prop_rw("enable_hlo_proto",
                   &tensorflow::ProfileOptions::enable_hlo_proto,
                   &tensorflow::ProfileOptions::set_enable_hlo_proto)
      .def_prop_rw("start_timestamp_ns",
                   &tensorflow::ProfileOptions::start_timestamp_ns,
                   &tensorflow::ProfileOptions::set_start_timestamp_ns)
      .def_prop_rw("duration_ms", &tensorflow::ProfileOptions::duration_ms,
                   &tensorflow::ProfileOptions::set_duration_ms)
      .def_prop_rw(
          "repository_path", &tensorflow::ProfileOptions::repository_path,
          [](tensorflow::ProfileOptions* options, const std::string& path) {
            options->set_repository_path(path);
          });

  nb::class_<TraceMeWrapper> traceme_class(profiler, "TraceMe");
  traceme_class.def(nb::init<nb::str, nb::kwargs>())
      .def("__enter__", [](nb::object self) -> nb::object { return self; })
      .def(
          "__exit__",
          [](nb::object self, const nb::object& ex_type,
             const nb::object& ex_value,
             const nb::object& traceback) -> nb::object {
            nb::cast<TraceMeWrapper*>(self)->Stop();
            return nb::none();
          },
          nb::arg("ex_type").none(), nb::arg("ex_value").none(),
          nb::arg("traceback").none())
      .def("set_metadata", &TraceMeWrapper::SetMetadata)
      .def_static("is_enabled", &TraceMeWrapper::IsEnabled);

  profiler.def(
      "get_profiled_instructions_proto",
      [](std::string tensorboard_dir) -> nb::bytes {
        tensorflow::profiler::ProfiledInstructionsProto profile_proto;
        xla::ThrowIfError(
            xla::ConvertXplaneUnderLogdirToProfiledInstructionsProto(
                tensorboard_dir, &profile_proto));
        std::string profile_proto_str = profile_proto.SerializeAsString();
        return nb::bytes(profile_proto_str.data(), profile_proto_str.size());
      },
      nb::arg("tensorboard_dir"));

  profiler.def("get_fdo_profile", [](nb::bytes xspace) -> nb::bytes {
    tensorflow::profiler::XSpace xspace_proto;
    // TODO(phawkins): change to std::string_view when protobuf is
    // updated in XLA.
    xspace_proto.ParseFromString(std::string(xspace.c_str(), xspace.size()));
    tensorflow::profiler::ProfiledInstructionsProto fdo_profile;
    xla::ThrowIfError(xla::ConvertXplaneToProfiledInstructionsProto(
        {xspace_proto}, &fdo_profile));
    std::string fdo_profile_str = fdo_profile.SerializeAsString();
    return nb::bytes(fdo_profile_str.data(), fdo_profile_str.size());
  });
}

}  // namespace xla
