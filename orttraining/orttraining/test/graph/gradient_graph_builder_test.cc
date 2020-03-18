// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "gtest/gtest.h"
#include "orttraining/core/optimizer/gist_encode_decode.h"
#include "test/providers/provider_test_utils.h"
#include "core/providers/cpu/cpu_execution_provider.h"
#include "core/session/environment.h"
#include "orttraining/models/runner/training_runner.h"

#ifdef USE_CUDA
#include "bert_toy_fetches.h"
#include "core/providers/cuda/cuda_execution_provider.h"
#endif

using namespace onnxruntime::logging;
using namespace onnxruntime::training;
using namespace google::protobuf::util;

namespace onnxruntime {
namespace test {

namespace {
constexpr auto ORIGINAL_MODEL_PATH = ORT_TSTR("testdata/test_training_model.onnx");
constexpr auto BACKWARD_MODEL_PATH = ORT_TSTR("testdata/temp_backward_model.onnx");

std::unordered_set<std::string> GetModelOutputNames(const InferenceSession& session) {
  const auto outputs_result = session.GetModelOutputs();
  ORT_ENFORCE(outputs_result.first.IsOK(), "Failed to get model outputs: ", outputs_result.first.ErrorMessage());
  std::unordered_set<std::string> output_names{};
  for (const auto* output : *outputs_result.second) {
    output_names.insert(output->Name());
  }
  return output_names;
}
}  // namespace

static TrainingSession::TrainingConfiguration MakeBasicTrainingConfig() {
  TrainingSession::TrainingConfiguration config{};
  config.model_with_training_graph_path = BACKWARD_MODEL_PATH;
  config.loss_function_config = TrainingSession::TrainingConfiguration::LossFunctionConfiguration{};
  config.loss_function_config.value().loss_function_info =
      LossFunctionInfo(OpDef("MeanSquaredError"), "loss", {"predictions", "labels"});

  return config;
}

static Status BuildBackPropGraph(
    const PathString& forward_model_file,
    const TrainingSession::TrainingConfiguration& config,
    PathString& backward_model_file) {
  std::unique_ptr<Environment> env;
  ORT_RETURN_IF_ERROR(Environment::Create(env));

  SessionOptions so{};
  TrainingSession training_session{so};

  std::cout << "Loading source model file = " << ToMBString(forward_model_file) << "\n";

  ORT_RETURN_IF_ERROR(training_session.Load(forward_model_file));

  TrainingSession::TrainingConfigurationResult config_result{};
  ORT_RETURN_IF_ERROR(training_session.ConfigureForTraining(config, config_result));

  backward_model_file = config.model_with_training_graph_path.value();

  return Status::OK();
}

/**
 * Run a training session for this model for 1 epoch, using batch size of 1 and synthetic input data.
 * @param so - SessionOptions for this run.
 * @param backprop_model_file - Mocel file to be run. This should already contain loss function and backward prop nodes.
 * @return TrainingSession for this run.
 */
static std::unique_ptr<TrainingSession> RunTrainingSessionWithChecks(
    const SessionOptions& so, const PathString& backprop_model_file) {
  std::unique_ptr<Environment> env;
  EXPECT_TRUE(Environment::Create(env).IsOK());

  const auto& log_manager = so.session_log_verbosity_level > 0 ? &DefaultLoggingManager() : nullptr;

  std::unique_ptr<TrainingSession> training_session = onnxruntime::make_unique<TrainingSession>(so, log_manager);

  EXPECT_TRUE(training_session->Load(backprop_model_file).IsOK());

  std::pair<common::Status, const ModelMetadata*> res = training_session->GetModelMetadata();
  EXPECT_TRUE(res.first.IsOK());
  EXPECT_TRUE(res.second != nullptr);
  auto model_metadata = res.second;
  std::cout << "Loaded " << model_metadata->graph_name << '\n';

  EXPECT_TRUE(training_session->Initialize().IsOK());

  std::vector<MLValue> gradient_fetches;
  RunOptions run_options;
  run_options.run_log_verbosity_level = so.session_log_verbosity_level;
  run_options.run_tag = so.session_logid;

  // Create dummy feeds
  std::vector<int64_t> image_dims = {1, 784};
  std::vector<int64_t> label_dims = {1, 10};
  std::vector<float> image_value(784, 1);
  std::vector<float> label_value(10, 1);

  MLValue imageMLValue;
  TrainingUtil::CreateCpuMLValue(image_dims, image_value, &imageMLValue);
  MLValue labelMLValue;
  TrainingUtil::CreateCpuMLValue(label_dims, label_value, &labelMLValue);

  auto fw_feeds = std::make_pair<std::vector<std::string>, std::vector<MLValue>>({"X", "labels"}, {imageMLValue, labelMLValue});

  auto output_names_include_gradients = GetModelOutputNames(*training_session);
  std::vector<std::string> training_output_names(output_names_include_gradients.begin(), output_names_include_gradients.end());

  auto start_time = std::chrono::high_resolution_clock::now();

  EXPECT_TRUE(training_session->Run(run_options, fw_feeds.first, fw_feeds.second, training_output_names, &gradient_fetches).IsOK());

  auto end_time = std::chrono::high_resolution_clock::now();
  auto elapsed = TimeDiffMicroSeconds(start_time, end_time);
  std::cout << "Training session run completed in " << elapsed << " microseconds.\n";

  return training_session;
}

TEST(GradientGraphBuilderTest, BuildGradientGraphTest) {
  const auto config = MakeBasicTrainingConfig();
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  std::shared_ptr<Model> pModel;
  EXPECT_TRUE(Model::Load(backprop_model_file, pModel, nullptr, DefaultLoggingManager().DefaultLogger()).IsOK());

  Graph& graph = pModel->MainGraph();
  EXPECT_FALSE(graph.GraphResolveNeeded());
  EXPECT_TRUE(graph.NumberOfNodes() > 0);
  EXPECT_TRUE(graph.MaxNodeIndex() > 0);

  std::cout << "Graph input names = [\n";
  for (const NodeArg* p_node_arg : graph.GetInputs()) {
    std::cout << '\t' << p_node_arg->Name() << '\n';
  }
  std::cout << "]\n";

  std::cout << "Graph output names = [\n";
  for (const NodeArg* p_node_arg : graph.GetOutputs()) {
    std::cout << '\t' << p_node_arg->Name() << '\n';
  }
  std::cout << "]\n";

  for (Node& node : graph.Nodes()) {
    const NodeIndex node_index = node.Index();
    const std::string& node_name = node.Name();
    const std::string& op_type = node.OpType();

    std::cout << "Operation node:"
              << " Index=" << node_index
              << (node.NodeType() == Node::Type::Fused ? "-(FUSED)" : "")
              << " OpType=" << op_type
              << " Name=" << node_name
              << '\n';
  }
}

TEST(GradientGraphBuilderTest, TrainingSession_Basic) {
  const auto config = MakeBasicTrainingConfig();
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  SessionOptions so{};
  RunTrainingSessionWithChecks(so, backprop_model_file);
}

TEST(GradientGraphBuilderTest, TrainingSession_WithGist) {
  auto config = MakeBasicTrainingConfig();
  config.gist_config = TrainingSession::TrainingConfiguration::GistConfiguration{};
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  std::cout << "Loading model file = " << ToMBString(backprop_model_file) << "\n";
  std::shared_ptr<Model> p_model;
  ASSERT_TRUE(onnxruntime::Model::Load(backprop_model_file, p_model, nullptr, DefaultLoggingManager().DefaultLogger()).IsOK());

  const Graph& graph = p_model->MainGraph();
  bool found_encoder = false;
  bool found_decoder = false;
  for (auto& node : graph.Nodes()) {
    const std::string& node_name = node.Name();
    std::cout << "Node name='" << node_name << "' op_type=" << node.OpType() << "\n";
    if (node_name.find(onnxruntime::GistEncodeDecode::GIST_ENCODER_NODE_NAME_BASE) != std::string::npos) {
      found_encoder = true;
      std::cout << "Found encoder node " << node_name << "\n";
    } else if (node_name.find(onnxruntime::GistEncodeDecode::GIST_DECODER_NODE_NAME_BASE) != std::string::npos) {
      found_decoder = true;
      std::cout << "Found decoder node " << node_name << "\n";
    }
  }
  ASSERT_TRUE(found_encoder);
  ASSERT_TRUE(found_decoder);

  SessionOptions so{};
  RunTrainingSessionWithChecks(so, backprop_model_file);
}

TEST(GradientGraphBuilderTest, TrainingSession_WithLogging) {
  const auto& log_manager = DefaultLoggingManager();
  const auto& default_logger = log_manager.DefaultLogger();
  log_manager.SetDefaultLoggerSeverity(Severity::kINFO);

  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kERROR, ::onnxruntime::logging::DataType::USER)) << "ERROR level logging enabled.";
  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kWARNING, ::onnxruntime::logging::DataType::USER)) << "WARNING level logging enabled.";
  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kINFO, ::onnxruntime::logging::DataType::USER)) << "INFO level logging enabled.";

  const auto config = MakeBasicTrainingConfig();
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  SessionOptions so;
  so.session_logid = "training_session_with_logging";
  so.session_log_verbosity_level = 1;  // 1 == detailed logging

  std::unique_ptr<TrainingSession> training_session = RunTrainingSessionWithChecks(so, backprop_model_file);

  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kERROR, ::onnxruntime::logging::DataType::USER)) << "ERROR level logging still enabled.";
  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kWARNING, ::onnxruntime::logging::DataType::USER)) << "WARNING level logging still enabled.";
  EXPECT_TRUE(default_logger.OutputIsEnabled(Severity::kINFO, ::onnxruntime::logging::DataType::USER)) << "INFO level logging still enabled.";

  std::string profile_file = training_session->EndProfiling();

  log_manager.SetDefaultLoggerSeverity(Severity::kWARNING);

  EXPECT_EQ(profile_file, std::string()) << "There should be no profile output file.";
}

TEST(GradientGraphBuilderTest, TrainingSession_WithProfiler) {
  const auto config = MakeBasicTrainingConfig();
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  SessionOptions so;
  so.enable_profiling = true;
  so.profile_file_prefix = ORT_TSTR("onnx_training_profiler_test");

  std::unique_ptr<TrainingSession> training_session = RunTrainingSessionWithChecks(so, backprop_model_file);

  std::string profile_file = training_session->EndProfiling();

  std::cout << "Profile output file = " << profile_file << '\n';

  std::ifstream profile(profile_file);
  ASSERT_TRUE(profile);

  std::vector<std::string> core_trace_fields = {"pid", "dur", "ts", "ph", "X", "name", "args"};
  std::vector<std::string> fiddle_profile_data_fields = {"dur", "activation_size", "parameter_size", "output_size"};

  int count = 0;
  std::string line;
  while (std::getline(profile, line)) {
    if (count == 0) {
      ASSERT_TRUE(line.find('[') != std::string::npos)
          << "Missing opening array marker in first trace record: " << line;
      // Opening array marker found.
    } else if (line.find(']') != std::string::npos) {
      // Closing array marker found.
      break;
    } else if (count >= 1) {
      if (count == 1) {
        auto s = "model_loading_uri";
        ASSERT_TRUE(line.find(s) != std::string::npos)
            << "Missing field '" << s << "' in trace record: " << line;
      }

      // Check we have the core fields in each trace record.
      for (auto& s : core_trace_fields) {
        ASSERT_TRUE(line.find(s) != std::string::npos)
            << "Missing core trace field '" << s << "' in trace record: " << line;
      }

      // Check we have the data profile fields output for each kernel operation.
      if (line.find("_kernel_time") != std::string::npos) {
        for (auto& s : fiddle_profile_data_fields) {
          ASSERT_TRUE(line.find(s) != std::string::npos)
              << "Missing data profile field '" << s << "' in trace record: " << line;
        }
      }
    }

    count++;
  }
  ASSERT_TRUE(count > 1);
}

#ifdef USE_CUDA
static void RunBertTrainingWithChecks(
    const SessionOptions& so,
    const PathString& backprop_model_file) {
  std::unique_ptr<Environment> env;
  EXPECT_TRUE(Environment::Create(env).IsOK());

  const auto& log_manager = so.session_log_verbosity_level > 0 ? &DefaultLoggingManager() : nullptr;

  std::unique_ptr<TrainingSession> training_session = onnxruntime::make_unique<TrainingSession>(so, log_manager);

  EXPECT_TRUE(training_session->Load(backprop_model_file).IsOK());

  std::pair<common::Status, const ModelMetadata*> res = training_session->GetModelMetadata();
  EXPECT_TRUE(res.first.IsOK());
  ASSERT_TRUE(res.second != nullptr);
  auto model_metadata = res.second;
  std::cout << "Loaded " << model_metadata->graph_name << '\n';

  CUDAExecutionProviderInfo xp_info;
  ASSERT_TRUE(training_session->RegisterExecutionProvider(onnxruntime::make_unique<CUDAExecutionProvider>(xp_info)).IsOK());

  ASSERT_TRUE(training_session->Initialize().IsOK());

  RunOptions run_options;
  run_options.run_log_verbosity_level = so.session_log_verbosity_level;
  run_options.run_tag = so.session_logid;

  // Creating feeds
  int batch_size = 13;
  int max_seq_len_in_batch = 7;
  std::vector<std::string> feed_names = {
      "input_ids",
      "token_type_ids",
      "input_mask",
      "masked_lm_ids",
      "next_sentence_labels",
      "masked_lm_positions",
      "masked_lm_weights",
  };
  std::vector<TensorShape> tensor_shapes = {
      {batch_size, max_seq_len_in_batch},
      {batch_size, max_seq_len_in_batch},
      {batch_size, max_seq_len_in_batch},
      {batch_size, max_seq_len_in_batch},
      {batch_size},
      {batch_size, max_seq_len_in_batch},
      {batch_size, max_seq_len_in_batch}};

  std::vector<std::vector<int64_t>> tensor_values = {
      /*input_ids*/
      {49, 97, 53, 5, 33, 65, 62,
       51, 38, 61, 45, 74, 27, 64,
       17, 36, 17, 96, 12, 79, 32,
       68, 90, 77, 18, 39, 12, 93,
       9, 87, 42, 60, 71, 12, 45,
       55, 40, 78, 81, 26, 70, 61,
       56, 66, 33, 7, 70, 1, 11,
       92, 51, 90, 85, 80, 0, 78,
       63, 42, 31, 93, 41, 90, 8,
       24, 72, 28, 30, 18, 69, 57,
       11, 10, 40, 65, 62, 13, 38,
       70, 37, 90, 15, 70, 42, 69,
       26, 77, 70, 75, 36, 56, 11},
      /*token_type_ids*/
      {12, 13, 1, 8, 15, 12, 9,
       15, 11, 6, 4, 9, 4, 3,
       8, 4, 9, 3, 2, 10, 15,
       3, 11, 13, 10, 6, 15, 14,
       8, 1, 0, 2, 12, 0, 15,
       10, 7, 10, 2, 6, 7, 7,
       4, 14, 2, 2, 10, 15, 3,
       9, 9, 3, 10, 6, 9, 14,
       2, 12, 10, 7, 9, 5, 6,
       5, 1, 8, 15, 2, 2, 4,
       4, 1, 2, 12, 8, 7, 6,
       13, 8, 14, 15, 11, 2, 10,
       3, 15, 10, 6, 7, 0, 8},
      /*input_mask*/
      {1, 1, 0, 1, 1, 1, 1,
       1, 1, 0, 0, 1, 0, 0,
       1, 0, 1, 0, 0, 1, 1,
       0, 1, 1, 1, 0, 1, 1,
       1, 0, 0, 0, 1, 0, 1,
       1, 0, 1, 0, 0, 0, 0,
       0, 1, 0, 0, 1, 1, 0,
       1, 1, 0, 1, 0, 1, 1,
       0, 1, 1, 0, 1, 0, 0,
       0, 0, 1, 1, 0, 0, 0,
       0, 0, 0, 1, 1, 0, 0,
       1, 1, 1, 1, 1, 0, 1,
       0, 1, 1, 0, 0, 0, 1},
      /*masked_lm_ids*/
      {1, 1, 0, 1, 2, 1, 1,
       1, 1, 1, 2, 0, 2, 0,
       1, 0, 0, 2, 1, 2, 2,
       2, 0, 1, 0, 2, 0, 2,
       1, 1, 2, 0, 1, 1, 1,
       2, 2, 0, 2, 1, 1, 2,
       1, 0, 2, 0, 0, 2, 1,
       2, 2, 2, 0, 2, 1, 1,
       0, 2, 1, 2, 0, 0, 2,
       0, 0, 0, 2, 1, 0, 0,
       1, 2, 1, 0, 1, 2, 1,
       2, 0, 2, 1, 2, 0, 2,
       2, 2, 1, 1, 0, 2, 1},
      /*next_sentence_labels*/
      {1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0},
      /*masked_lm_positions*/
      {0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6,
       0, 1, 2, 3, 4, 5, 6}};
  std::vector<float> masked_lm_weights(13 * 7, 1.0f);

  std::vector<OrtValue> feeds(feed_names.size());
  for (size_t i = 0; i < 6; ++i) {
    TrainingUtil::CreateCpuMLValue(tensor_shapes[i].GetDims(), tensor_values[i], &feeds[i]);
  }
  TrainingUtil::CreateCpuMLValue(tensor_shapes[6].GetDims(), masked_lm_weights, &feeds[6]);

  auto output_names_include_gradients = GetModelOutputNames(*training_session);
  std::vector<std::string> fetch_names(output_names_include_gradients.begin(), output_names_include_gradients.end());

  std::vector<OrtValue> fetches;

  EXPECT_TRUE(training_session->Run(run_options, feed_names, feeds, fetch_names, &fetches).IsOK());

  for (size_t i = 0; i < fetch_names.size(); ++i) {
    if (!fetches[i].IsAllocated() || !!fetches[i].IsTensor())
      continue;

    const Tensor& tensor = fetches[i].Get<Tensor>();
    if (DataTypeImpl::GetType<float>() != tensor.DataType()) {
      continue;
    }

    const std::string& name = fetch_names[i];
    if (BERT_TOY_FETCHES.find(name) == BERT_TOY_FETCHES.end()) {
      continue;
    }

    auto gradient_ref = BERT_TOY_FETCHES.at(name);
    EXPECT_TRUE(static_cast<size_t>(tensor.Shape().Size()) == gradient_ref.size());

    float max_diff = 0;
    float max_percent_diff = 0;
    const float* data = tensor.template Data<float>();
    for (size_t idx = 0; idx < gradient_ref.size(); ++idx) {
      float diff = std::fabs(static_cast<float>(gradient_ref[idx]) - data[idx]);
      max_diff = std::fmax(max_diff, diff);
      max_percent_diff = std::fmax(max_percent_diff, diff / data[idx]);
    }
    EXPECT_TRUE(max_diff < 1e-5) << name << " is incorrect: max_diff is " << max_diff;
    if (max_diff > 1e-10) {
      EXPECT_TRUE(max_percent_diff < 0.01f) << name << " is incorrect: max_percent_diff is "
                                            << max_percent_diff;
    }
  }
}
#endif
TEST(GradientGraphBuilderTest, DISABLED_TrainingSession_BertToy) {
  const auto model_path = ORT_TSTR("testdata/bert_toy_optimized.onnx");

  TrainingSession::TrainingConfiguration config{};
  config.model_with_training_graph_path = ORT_TSTR("testdata/bert_toy_optimized_bw.onnx");
  config.loss_function_config = TrainingSession::TrainingConfiguration::LossFunctionConfiguration{};
  config.loss_function_config.value().loss_function_info =
      LossFunctionInfo(OpDef("BertLoss", kOnnxDomain),
                       "total_loss",
                       {/*prediction_masked_lm*/ "prediction_scores",
                        /*prediction_next_sentence*/ "seq_relationship_score",
                        /*masked_lm_positions*/ "masked_lm_positions",
                        /*masked_lm_ids*/ "masked_lm_ids",
                        /*masked_lm_weights*/ "masked_lm_weights",
                        /*next_sentence_labels*/ "next_sentence_labels",
                        /*mlm_loss*/ "mlm_loss",
                        /*nsp_loss*/ "nsp_loss"});
  config.weight_names_to_not_train = {
      "position_01",            // Slice's dat input
      "op_min_ends_expand_10",  //op_min_ends_expand_10
  };
  config.immutable_weights = {
      {"Div", {{1, 8.0f}, {1, 1.4142135381698608f}}},
      {"Add", {{1, 1.0f}, {1, 9.999999960041972e-13f}}},
      {"Mul", {{1, 0.5f}, {1, -10000.0f}}},
      {"Sub", {{0, 1.0f}}}};

  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(model_path, config, backprop_model_file));

#ifdef USE_CUDA
  SessionOptions so;
  RunBertTrainingWithChecks(so, backprop_model_file);
#endif
}

class PipelineSplitter {
 public:
  struct UnidirectionCutInfo {
    // nodes are identified by its output[0]
    std::vector<std::string> nodes;
    // inputs for sync between sub models
    std::vector<std::string> sync_inputs;
    // outputs for sync between sub models
    // note there might be some graph ouputs do not need to sync
    std::vector<std::string> sync_outputs;
    // dependencies for maintaining topological order
    std::vector<std::string> wait_depends;
    std::vector<std::string> record_depends;
  };

  struct CutInfo {
    UnidirectionCutInfo fw;
    UnidirectionCutInfo bw;
  };

  PipelineSplitter() = default;

  void Split(
      PathString backprop_model_file,
      const std::vector<PathString>& sub_model_files,
      const std::vector<CutInfo>& cuts) {
    const int num_subs = (int)cuts.size();

    ONNX_NAMESPACE::ModelProto mp;
    ASSERT_STATUS_OK(Model::Load(backprop_model_file, mp));

    const auto& main_gp = mp.graph();
    ONNX_NAMESPACE::ModelProto sub_mps[3];
    for (int i = 0; i < num_subs; ++i) {
      auto& sub = sub_mps[i];
      sub.CopyFrom(mp);
      sub.clear_graph();
      FillInputWait(sub.mutable_graph(), main_gp, cuts[i].fw.sync_inputs, cuts[i].fw.wait_depends, i, /*bw*/ false);
    }

    for (const auto& n : main_gp.node()) {
      // check which sub_model the node should be in
      int sub_id = -1;
      for (int i = 0; i < num_subs; ++i) {
        const auto& cut = cuts[i];
        if (std::count(cut.fw.nodes.cbegin(), cut.fw.nodes.cend(), n.output()[0])) {
          sub_id = i;
          break;
        }
        if (std::count(cut.bw.nodes.cbegin(), cut.bw.nodes.cend(), n.output()[0])) {
          sub_id = i;
          break;
        }
      }
      EXPECT_TRUE(sub_id != -1);
      auto* sub_gp = sub_mps[sub_id].mutable_graph();
      const auto& cut = cuts[sub_id];

      // add WaitEvent node at the beginning of bw
      if (!cut.bw.nodes.empty() && n.output()[0] == cut.bw.nodes.front()) {
        FillInputWait(sub_gp,
                      main_gp,
                      cut.bw.sync_inputs,
                      cut.bw.wait_depends,
                      sub_id,
                      /*bw*/ true);
      }

      // copy node to sub model
      sub_gp->mutable_node()->Add()->CopyFrom(n);
      for (auto i = n.input().cbegin(); i != n.input().cend(); ++i) {
        AddItemByName(sub_gp->mutable_initializer(), main_gp.initializer(), *i, *i);
        if (0 == std::count(cut.fw.sync_inputs.cbegin(), cut.fw.sync_inputs.cend(), *i) &&
            0 == std::count(cut.bw.sync_inputs.cbegin(), cut.bw.sync_inputs.cend(), *i)) {
          // carry over original graph's input, if not in sync_inputs
          AddItemByName(sub_gp->mutable_input(), main_gp.input(), *i, *i);
        }
      };
      for (auto i = n.output().cbegin(); i != n.output().cend(); ++i) {
        if (std::count(cut.fw.sync_outputs.cbegin(), cut.fw.sync_outputs.cend(), *i) ||
            std::count(cut.bw.sync_outputs.cbegin(), cut.bw.sync_outputs.cend(), *i))
          continue;  // sync_ouputs already handled, skip

        // add graph output
        if (!AddItemByName(sub_gp->mutable_output(), main_gp.output(), *i, *i)) {
          // for non-output, add shape info
          AddItemByName(sub_gp->mutable_value_info(), main_gp.value_info(), *i, *i);
        }
      };

      // add RecordEvent node at the end of fw and bw
      if ((!cut.fw.nodes.empty() && n.output()[0] == cut.fw.nodes.back()) ||
          (!cut.bw.nodes.empty() && n.output()[0] == cut.bw.nodes.back())) {
        bool bw = (n.output()[0] == cut.bw.nodes.back());
        const auto& sync_outputs = (bw ? cut.bw.sync_outputs : cut.fw.sync_outputs);
        const auto& dependencies = (bw ? cut.bw.record_depends : cut.fw.record_depends);

        FillOutputRecord(sub_gp,
                         main_gp,
                         sync_outputs,
                         dependencies,
                         sub_id,
                         bw);
      }
    }

    // save sub models
    for (int sub_id = 0; sub_id < num_subs; ++sub_id) {
      std::ofstream ofs(sub_model_files[sub_id], std::ofstream::binary);
      sub_mps[sub_id].SerializeToOstream(&ofs);
      ofs.close();
    }
  }

 private:
  // add RepeatedField item by name from another RepeatedFields
  // return true if the name exists in dst
  template <typename TD, typename TS>
  bool AddItemByName(TD* dst, const TS& src, const std::string& name, const std::string& new_name) {
    for (auto iter = dst->cbegin(); iter != dst->cend(); ++iter) {
      if (iter->name() == new_name) {
        return true;
      }
    }
    for (auto iter = src.cbegin(); iter != src.cend(); ++iter) {
      if (iter->name() == name) {
        auto* p = dst->Add();
        p->CopyFrom(*iter);
        *p->mutable_name() = new_name;
        return true;
      }
    }
    return false;
  }

  void FillInputWait(
      ONNX_NAMESPACE::GraphProto* sub_gp,
      const ONNX_NAMESPACE::GraphProto& main_gp,
      const std::vector<std::string>& sync_inputs,
      const std::vector<std::string>& dependencies,
      int sub_id,
      bool bw) {
    // input/output with Wait/RecordEvent
    // Note data is gated by Wait/RecordEvent, so name with postfix "_sync"
    // In distributed training, the pattern is:
    //   wait_data -> recv -> wait_pipeline -> fw/bw -> record_pipeline -> send -> record_data
    // Here wait_data/record_data is to ensure execution order due to data dependency (same batch across pipelines),
    // while wait_pipeline/recorde_pipeline is to ensure pipeline execution order.
    // This test simplifies the graph to omit send/recv,
    // but we still need to have double wait and record to sync data and pipeline separately
    ONNX_NAMESPACE::NodeProto* wait_data_np = nullptr;
    ONNX_NAMESPACE::NodeProto* wait_pipeline_np = nullptr;
    std::string wait_data_id = "wait_data_" + std::to_string(sub_id) + (bw ? "_bw" : "_fw");
    std::string wait_pipeline_id = "wait_pipeline_" + std::to_string(sub_id) + (bw ? "_bw" : "_fw");
    bool is_first = (sub_id == 0 && !bw);
    if (sync_inputs.size() + dependencies.size() > 0) {
      if (!is_first) {
        wait_data_np = sub_gp->add_node();
        *wait_data_np->mutable_op_type() = "WaitEvent";
        *wait_data_np->mutable_domain() = kMSDomain;
        *wait_data_np->mutable_input()->Add() = wait_data_id;
      }
      wait_pipeline_np = sub_gp->add_node();
      *wait_pipeline_np->mutable_op_type() = "WaitEvent";
      *wait_pipeline_np->mutable_domain() = kMSDomain;
      *wait_pipeline_np->mutable_input()->Add() = wait_pipeline_id;
    }
    for (const auto& name : sync_inputs) {
      std::string input_name = name + "_sync";
      std::string recv_name = name + "_recv";
      if (wait_data_np) {
        *wait_data_np->mutable_input()->Add() = input_name;
        *wait_data_np->mutable_output()->Add() = recv_name;
        *wait_pipeline_np->mutable_input()->Add() = recv_name;
      } else {
        *wait_pipeline_np->mutable_input()->Add() = input_name;
      }
      *wait_pipeline_np->mutable_output()->Add() = name;
      // some input comes graph input
      if (AddItemByName(sub_gp->mutable_input(),
                        main_gp.input(),
                        name,
                        input_name)) {
        ASSERT_TRUE(is_first);
        // add shape info
        EXPECT_TRUE(AddItemByName(sub_gp->mutable_value_info(),
                                  main_gp.input(),
                                  name,
                                  name));
      } else {
        // some input comes from the middle of the graph
        AddItemByName(sub_gp->mutable_input(),
                      main_gp.value_info(),
                      name,
                      input_name);
        // add shape info
        AddItemByName(sub_gp->mutable_value_info(),
                      main_gp.value_info(),
                      name,
                      recv_name);
        AddItemByName(sub_gp->mutable_value_info(),
                      main_gp.value_info(),
                      name,
                      name);
      }
    }

    if (wait_pipeline_np) {
      //add dependencies on the first wait
      auto* wait_np = wait_data_np ? wait_data_np : wait_pipeline_np;
      for (const auto& dep : dependencies) {
        *wait_np->mutable_input()->Add() = dep;
      }

      // add input for event ids
      if (wait_data_np) {
        auto* p = sub_gp->mutable_input()->Add();
        *p->mutable_name() = wait_data_id;
        p->mutable_type()->mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
      }
      auto* p = sub_gp->mutable_input()->Add();
      *p->mutable_name() = wait_pipeline_id;
      p->mutable_type()->mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
    }
  }

  void FillOutputRecord(
      ONNX_NAMESPACE::GraphProto* sub_gp,
      const ONNX_NAMESPACE::GraphProto& main_gp,
      const std::vector<std::string>& sync_outputs,
      const std::vector<std::string>& dependencies,
      int sub_id,
      bool bw) {
    ONNX_NAMESPACE::NodeProto* record_pipeline_np = nullptr;
    ONNX_NAMESPACE::NodeProto* record_data_np = nullptr;
    std::string record_pipeline_id = "record_pipeline_" + std::to_string(sub_id) + (bw ? "_bw" : "_fw");
    std::string record_data_id = "record_data_" + std::to_string(sub_id) + (bw ? "_bw" : "_fw");
    bool is_last = (sub_id == 0 && bw);
    if (sync_outputs.size() + dependencies.size() > 0) {
      record_pipeline_np = sub_gp->add_node();
      *record_pipeline_np->mutable_op_type() = "RecordEvent";
      *record_pipeline_np->mutable_domain() = kMSDomain;
      *record_pipeline_np->mutable_input()->Add() = record_pipeline_id;

      if (!is_last) {
        record_data_np = sub_gp->add_node();
        *record_data_np->mutable_op_type() = "RecordEvent";
        *record_data_np->mutable_domain() = kMSDomain;
        *record_data_np->mutable_input()->Add() = record_data_id;
      }
    }

    if (sync_outputs.size() > 0) {
      for (const auto& name : sync_outputs) {
        *record_pipeline_np->mutable_input()->Add() = name;
        if (record_data_np) {
          *record_pipeline_np->mutable_output()->Add() = name + "_send";
          *record_data_np->mutable_input()->Add() = name + "_send";
          *record_data_np->mutable_output()->Add() = name + "_sync";
        } else {
          *record_pipeline_np->mutable_output()->Add() = name + "_sync";
        }
      }
    }

    if (record_pipeline_np) {
      for (const auto& name : dependencies)
        *record_pipeline_np->mutable_input()->Add() = name;

      // add input for event_id
      auto* p = sub_gp->mutable_input()->Add();
      *p->mutable_name() = record_pipeline_id;
      p->mutable_type()->mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);

      if (record_data_np) {
        p = sub_gp->mutable_input()->Add();
        *p->mutable_name() = record_data_id;
        p->mutable_type()->mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_INT64);
      }
    }

    // add graph output and shape info
    for (const auto& name : sync_outputs) {
      AddItemByName(sub_gp->mutable_output(),
                    main_gp.value_info(),
                    name,
                    name + "_sync");
      if (!is_last) {
        AddItemByName(sub_gp->mutable_value_info(),
                      main_gp.value_info(),
                      name,
                      name + "_send");
      }
      AddItemByName(sub_gp->mutable_value_info(),
                    main_gp.value_info(),
                    name,
                    name);
    }
  }
};

TEST(GradientGraphBuilderTest, TrainingSession_WithPipeline) {
  auto config = MakeBasicTrainingConfig();
  //config.set_gradients_as_graph_outputs = true;
  PathString backprop_model_file;
  ASSERT_STATUS_OK(BuildBackPropGraph(ORIGINAL_MODEL_PATH, config, backprop_model_file));

  // cut the model using outputs
  const std::vector<PipelineSplitter::CutInfo> cuts = {
      //sub model 0
      {{{"T1", "T2", "T3"},
        {"X"},
        {"T3"},
        {},
        {}},
       {{"T2_grad", "T1_grad", "B1_grad", "W1_grad"},
        {"T3_grad"},
        {},
        {"T3_sync"},
        {"B1_grad", "W1_grad"}}},
      // sub model 1
      {{{"T4", "T5", "T6"},
        {"T3"},
        {"T6"},
        {},
        {}},
       {{"T5_grad", "T4_grad", "T3_grad", "B2_grad", "W2_grad"},
        {"T6_grad"},
        {"T3_grad"},
        {"T6_sync"},
        {"B2_grad", "W2_grad"}}},
      // sub model 2
      {{{"T7", "MeanSquaredError_diff", "MeanSquaredError_diff_square", "loss", "predictions"},
        {"T6"},
        {},
        {},
        {}},
       {{"MeanSquaredError_reduce_mean_Grad/Unqueezed_Grad", "MeanSquaredError_reduce_mean_Grad/Tiled_Grad", "MeanSquaredError_diff_square_grad", "MeanSquaredError_diff_grad", "predictions_grad", "B3_grad", "T7_grad", "W3_grad", "T6_grad"},
        {},
        {"T6_grad"},
        {},
        {"loss", "predictions", "B3_grad", "W3_grad"}}}};

  const int num_subs = (int)cuts.size();

  std::vector<PathString> sub_model_files(num_subs);
  for (int sub_id = 0; sub_id < num_subs; ++sub_id) {
#ifdef _WIN32
    auto sub_id_str = std::to_wstring(sub_id);
#else
    auto sub_id_str = std::to_string(sub_id);
#endif
    sub_model_files[sub_id] = ORT_TSTR("sub_") + sub_id_str + ORT_TSTR(".onnx");
  }

  PipelineSplitter splitter;
  splitter.Split(backprop_model_file, sub_model_files, cuts);

  // create training sessions
  std::unique_ptr<Environment> env;
  EXPECT_TRUE(Environment::Create(env).IsOK());

  struct SubSession {
    std::unique_ptr<TrainingSession> sess;
    SessionOptions so;
    RunOptions run_options;
  };

  std::vector<SubSession> subs(num_subs);
  for (int sub_id = 0; sub_id < num_subs; ++sub_id) {
    auto& sub_sess = subs[sub_id];
    sub_sess.so.enable_profiling = true;
#ifdef _WIN32
    auto sub_id_str = std::to_wstring(sub_id);
#else
    auto sub_id_str = std::to_string(sub_id);
#endif
    sub_sess.so.profile_file_prefix = ORT_TSTR("pipeline") + sub_id_str;
    const auto& log_manager = sub_sess.so.session_log_verbosity_level > 0 ? &DefaultLoggingManager() : nullptr;
    sub_sess.run_options.run_log_verbosity_level = sub_sess.so.session_log_verbosity_level;
    sub_sess.run_options.run_tag = sub_sess.so.session_logid;

    sub_sess.sess = onnxruntime::make_unique<TrainingSession>(sub_sess.so, log_manager);
    EXPECT_TRUE(sub_sess.sess->Load(sub_model_files[sub_id]).IsOK());
    EXPECT_TRUE(sub_sess.sess->Initialize().IsOK());
  }

  // pipeline inputs for each batch
  struct PipelineFeed {
    MLValue x_value;
    MLValue label_value;
    std::vector<MLValue> record_data_values;
    std::vector<std::pair<MLValue, MLValue>> wait_record_pipeline_values;

    void SetInputs(const std::vector<float>& x, const std::vector<float>& label) {
      // dummy data for model inputs
      std::vector<int64_t> x_dims = {1, 784};
      std::vector<int64_t> label_dims = {1, 10};
      TrainingUtil::CreateCpuMLValue<float>(x_dims, x, &x_value);
      TrainingUtil::CreateCpuMLValue<float>(label_dims, label, &label_value);
    }

    void SetEvents(const std::vector<int64_t>& record_data,
                   const std::vector<std::pair<int64_t, int64_t>>& wait_record_pipeline) {
      record_data_values.resize(record_data.size());
      for (size_t i = 0; i < record_data.size(); ++i) {
        TrainingUtil::CreateCpuMLValue<int64_t>({}, {record_data[i]}, &record_data_values[i]);
      }
      wait_record_pipeline_values.resize(wait_record_pipeline.size());
      for (size_t i = 0; i < wait_record_pipeline.size(); ++i) {
        TrainingUtil::CreateCpuMLValue<int64_t>(
            {}, {wait_record_pipeline[i].first},
            &wait_record_pipeline_values[i].first);
        TrainingUtil::CreateCpuMLValue<int64_t>(
            {}, {wait_record_pipeline[i].second},
            &wait_record_pipeline_values[i].second);
      }
    }
  };

  // pipeline data for each batch
  struct PipelineData : public PipelineFeed {
    MLValue t3_value;
    MLValue t3_grad_value;
    MLValue t6_value;
    MLValue t6_grad_value;

    PipelineData() {
      std::vector<int64_t> t3_dims = {1, 128};
      std::vector<int64_t> t6_dims = {1, 32};
      std::vector<float> t3_data(128);
      std::vector<float> t6_data(32);
      TrainingUtil::CreateCpuMLValue<float>(t3_dims, t3_data, &t3_value);
      TrainingUtil::CreateCpuMLValue<float>(t3_dims, t3_data, &t3_grad_value);
      TrainingUtil::CreateCpuMLValue<float>(t6_dims, t6_data, &t6_value);
      TrainingUtil::CreateCpuMLValue<float>(t6_dims, t6_data, &t6_grad_value);
    };
  };

  auto worker = [&subs](int sub_id, PipelineData& data) {
    std::vector<std::string> input_names;
    std::vector<MLValue> input_values;
    std::vector<std::string> output_names;
    std::vector<MLValue> output_values;
    switch (sub_id) {
      case 0:
        input_names = {
            "X_sync", "T3_grad_sync",
            "wait_pipeline_0_fw",
            "record_pipeline_0_fw", "record_data_0_fw",
            "wait_data_0_bw", "wait_pipeline_0_bw",
            "record_pipeline_0_bw"};
        input_values = {
            data.x_value, data.t3_grad_value,
            data.wait_record_pipeline_values[0].first,
            data.wait_record_pipeline_values[0].second,
            data.record_data_values[0],
            data.record_data_values[3],
            data.wait_record_pipeline_values[4].first,
            data.wait_record_pipeline_values[4].second};
        output_names = {"T3_sync"};
        output_values = {data.t3_value};
        break;
      case 1:
        input_names = {
            "T3_sync", "T6_grad_sync",
            "wait_data_1_fw", "wait_pipeline_1_fw",
            "record_pipeline_1_fw", "record_data_1_fw",
            "wait_data_1_bw", "wait_pipeline_1_bw",
            "record_pipeline_1_bw", "record_data_1_bw"};
        input_values = {
            data.t3_value, data.t6_grad_value,
            data.record_data_values[0],
            data.wait_record_pipeline_values[1].first,
            data.wait_record_pipeline_values[1].second,
            data.record_data_values[1],
            data.record_data_values[2],
            data.wait_record_pipeline_values[3].first,
            data.wait_record_pipeline_values[3].second,
            data.record_data_values[3]};
        output_names = {"T6_sync", "T3_grad_sync"};
        output_values = {data.t6_value, data.t3_grad_value};
        break;
      case 2:
        input_names = {
            "T6_sync", "labels",
            "wait_data_2_fw", "wait_pipeline_2_fw",
            "record_pipeline_2_bw", "record_data_2_bw"};
        input_values = {
            data.t6_value, data.label_value,
            data.record_data_values[1],
            data.wait_record_pipeline_values[2].first,
            data.wait_record_pipeline_values[2].second,
            data.record_data_values[2]};
        output_names = {"T6_grad_sync"};
        output_values = {data.t6_grad_value};
        break;
      default:
        ASSERT_TRUE(false);
    }
    EXPECT_TRUE(subs[sub_id].sess->Run(subs[sub_id].run_options, input_names, input_values, output_names, &output_values).IsOK());
  };

  struct EventsPerBatch {
    std::vector<int64_t> record_data;
    std::vector<std::pair<int64_t, int64_t>> wait_record_pipeline;
  };

  // TODO: create this from generator
  // sub 0: F0 F1 F2 F3 F4 B0 F5 B1    B2    B3    B4    B5
  // sub 1:    F0 F1 F2 B0 F3 B1 F4 B2 F5 B3    B4    B5
  // sub 2:       F0 B0 F1 B1 F2 B2 F3 B3 F4 B4 F5 B5
  // Note that in distributed training, event id would be local to each pipeline
  // We use different ranges for event ids for pipelines here:
  // 0 -> 99: data dependencies for record_data
  // 100 -> 199: sub 0
  // 200 -> 299: sub 1
  // 300 -> 399: sub 2

  const std::vector<EventsPerBatch> events = {
      // batch 0
      {{0, 1, 2, 3},
       {{-1, 100}, {-1, 200}, {-1, 300}, {202, 203}, {104, 105}}},
      // batch 1
      {{4, 5, 6, 7},
       {{100, 101}, {200, 201}, {300, 301}, {204, 205}, {106, 107}}},
      // batch 2
      {{8, 9, 10, 11},
       {{101, 102}, {201, 202}, {301, 302}, {206, 207}, {107, 108}}},
      // batch 3
      {{12, 13, 14, 15},
       {{102, 103}, {203, 204}, {302, 303}, {208, 209}, {108, 109}}},
      //batch 4
      {{16, 17, 18, 19},
       {{103, 104}, {205, 206}, {303, 304}, {209, 210}, {109, 110}}},
      // batch 5
      {{20, 21, 22, 23},
       {{105, 106}, {207, 208}, {304, 305}, {210, 211}, {110, 111}}},
  };

  const int num_batches = (int)events.size();

  std::vector<std::thread> workers;
  //TODO: recycle pipeline data during batch loop
  std::vector<PipelineData> pd;
  pd.resize(num_batches);
  for (int batch = 0; batch < num_batches; ++batch) {
    std::vector<float> x(784);
    std::vector<float> label(10);
    pd[batch].SetInputs(x, label);
    pd[batch].SetEvents(events[batch].record_data, events[batch].wait_record_pipeline);

    for (int sub_id = 0; sub_id < num_subs; ++sub_id) {
      workers.emplace_back([&worker, &pd, batch, sub_id]() {
        worker(sub_id, pd[batch]);
      });
    }
  }

  //TODO: join workers during batch loop
  for (auto& w : workers)
    w.join();

  // finish profiler
  for (int sub_id = 0; sub_id < num_subs; ++sub_id) {
    subs[sub_id].sess->EndProfiling();
  }
}

}  // namespace test
}  // namespace onnxruntime