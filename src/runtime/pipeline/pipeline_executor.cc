/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file pipeline_executor.cc
 */
#include "pipeline_executor.h"
namespace tvm {
namespace runtime {
/*!
 * \brief Give frontends an access to packed functions.
 * \param name The name of the function.
 * \param sptr_to_self The pointer to the module node.
 * \return The corresponding packed function.
 */
PackedFunc PipelineExecutor::GetFunction(const std::string& name,
                                         const ObjectPtr<Object>& sptr_to_self) {
  if (name == "get_num_outputs") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->NumOutputs(); });
  } else if (name == "get_input_pipeline_map") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0])) {
        *rv = this->GetInputPipeplineMap(args[0].operator String());
      } else {
        LOG(FATAL) << "Function only support the input name value in the form of string";
      }
    });
  } else if (name == "get_params_group_pipeline_map") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0])) {
        *rv = this->GetParamsGroupPipelineMap(args[0].operator String());
      } else {
        LOG(FATAL) << "Function only support the input name value in the form of string";
      }
    });
  } else if (name == "set_param") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0]) && String::CanConvertFrom(args[1])) {
        this->SetParam(args[0].operator String(), args[1].operator String(), args[2]);
      } else {
        LOG(FATAL) << "Function only support the parameter name and the key in the form of string";
      }
    });
  } else {
    LOG(FATAL) << "Unknown packed function: " << name;
    return PackedFunc();
  }
  return nullptr;
}

/*!
 * \brief Using the global input name to get the index, and also get the input interface name
   of corresponding subgraph from the input connection configuration.
 * \param The global input name.
 * \return Returning the index and the input interface name of corresponding subgraph.
 */
Array<String> PipelineExecutor::GetInputPipeplineMap(std::string input_name) {
  std::pair<int, std::string> map = input_connection_config[input_name];
  return {std::to_string(map.first), map.second};
}

/*!
 * \brief Return the module index for the parameters group name.
 * \param name The parameters group name.
 * \return int The module index.
 */
int PipelineExecutor::GetParamsGroupPipelineMap(const std::string& name) {
  return param_connection_config[name];
}

/*!
 * \brief Use the mod_config information to create a graph runtime list.
 * \param mod_config The config information that generates by the export library function call.
 */
std::vector<Module> PipelineExecutor::CreateGraphModules(const ModuleConfig& mod_config) {
  const PackedFunc* graph_executor_create = Registry::Get("tvm.graph_executor.create");
  std::vector<Module> ret;
  ret.resize(mod_config.size());
  for (auto config : mod_config) {
    // Load library.
    auto lib = Module::LoadFromFile(config.second.lib_name.c_str());

    // Read json.
    std::ifstream ifJson(config.second.json_name.c_str());
    if (ifJson.fail()) {
      LOG(FATAL) << "json file not found: " << config.second.json_name;
    }
    const std::string json((std::istreambuf_iterator<char>(ifJson)),
                           std::istreambuf_iterator<char>());

    // Create a graph executor.
    std::istringstream istr(config.second.dev);
    std::string str;
    int device_type = 1, device_id = 0;
    while (getline(istr, str, ';')) {
      std::istringstream istr_dev(str);
      std::string str_temp;
      if (getline(istr_dev, str_temp)) {
        device_type = stoi(str_temp);
      }
      if (getline(istr_dev, str_temp)) {
        device_id = stoi(str_temp);
      }
    }
    Module graph_module = (*graph_executor_create)(json, lib, device_type, device_id);

    // Load parameters.
    TVMByteArray params_arr;
    const char* params_file_name = config.second.params_name.c_str();
    std::ifstream if_param(params_file_name);
    if (if_param.fail()) {
      LOG(FATAL) << "params file not found: " << params_file_name;
    }
    const std::string params((std::istreambuf_iterator<char>(if_param)),
                             std::istreambuf_iterator<char>());
    params_arr.data = params.c_str();
    params_arr.size = params.length();
    auto load_params = graph_module.GetFunction("load_params");
    load_params(params_arr);

    // Put a graph executor module into the vector.
    ret[config.first] = graph_module;
  }
  return ret;
}
/*!
 * \brief Set a parameter into a graph module.
 * \param param_group_name The parameters group name.
 * \param param_key_name The parameter key name.
 * \param data_in The parameter data.
 */
void PipelineExecutor::SetParam(std::string param_group_name, std::string param_key_name,
                                DLTensor* data_in) {
  // Get the module index from the param name.
  int module_index = this->GetParamsGroupPipelineMap(param_group_name);
  // TODO(huajsj): set the parameters into runtime module.
}
/*!
 * \brief Initialize the pipeline executor with a list of modules to be pipelined
 *  and config in JSON format.
 * \param modules The module list used for building the pipeline.
 * \param pipeline_json The configuration of modules dependencies.
 */
void PipelineExecutor::Init(const std::vector<Module>& modules, const std::string& pipeline_json) {
  ICHECK(!modules.empty()) << "The graph executor module list is empty.";
  // Use JSONReader to load pipeline configuration.
  std::istringstream is(pipeline_json);
  dmlc::JSONReader reader(&is);
  this->LoadConfig(&reader);
  ICHECK(!pipeline_config_.Empty()) << "The pipeline config information is empty.";
  // Initialize the pipeline function class used for pipeline thread pool management
  // and schedule etc. This function returns the number of output.
  num_outputs_ = pipeline_scheduler_.PipelineInit(modules, pipeline_config_);
  return;
}

Module PipelineExecutorCreate(const Array<Module>& m, const std::string& pipeline_json) {
  ICHECK(!m.empty()) << "The module list is empty.";
  auto exec = make_object<PipelineExecutor>();
  std::vector<Module> graph_modules;
  for (auto mod : m) {
    graph_modules.push_back(mod);
  }
  exec->Init(graph_modules, pipeline_json);
  return Module(exec);
}

Module PipelineExecutorLoad(const std::string& load_json, const std::string& pipeline_json) {
  auto exec = make_object<PipelineExecutor>();
  std::istringstream is(load_json);
  dmlc::JSONReader reader(&is);
  ModuleConfig& mod_config = exec->LoadModuleConfig(&reader);
  ICHECK(!mod_config.empty()) << "The module config is empty.";
  std::vector<Module> modules = exec->CreateGraphModules(mod_config);
  exec->Init(modules, pipeline_json);
  return Module(exec);
}

TVM_REGISTER_GLOBAL("tvm.pipeline_executor.create").set_body([](TVMArgs args, TVMRetValue* rv) {
  *rv = PipelineExecutorCreate(args[0], args[1]);
});

TVM_REGISTER_GLOBAL("tvm.pipeline_executor.load").set_body([](TVMArgs args, TVMRetValue* rv) {
  *rv = PipelineExecutorLoad(args[0], args[1]);
});
}  // namespace runtime
}  // namespace tvm
