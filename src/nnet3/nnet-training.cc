// nnet3/nnet-training.cc

// Copyright      2015    Johns Hopkins University (author: Daniel Povey)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "nnet3/nnet-training.h"
#include "nnet3/nnet-utils.h"

namespace kaldi {
namespace nnet3 {

NnetTrainer::NnetTrainer(const NnetTrainerOptions &config,
                         Nnet *nnet):
    config_(config),
    nnet_(nnet),
    compiler_(*nnet, config_.optimize_config),
    num_minibatches_processed_(0) {
  if (config.store_component_stats && config.zero_component_stats)
    ZeroComponentStats(nnet);
}


void NnetTrainer::Train(const NnetExample &eg) {
  bool need_model_derivative = true;
  ComputationRequest request;
  GetComputationRequest(*nnet_, eg, need_model_derivative,
                        config_.store_component_stats,
                        &request);
  const NnetComputation *computation = compiler_.Compile(request);
  NnetComputer computer(config_.compute_config, *computation,
                        *nnet_, nnet_);
  // give the inputs to the computer object.
  computer.AcceptInputs(*nnet_, eg);
  computer.Forward();

  this->ProcessOutputs(eg, &computer);
  computer.Backward();
}

void NnetTrainer::ProcessOutputs(const NnetExample &eg,
                                 NnetComputer *computer) {
  std::vector<NnetIo>::const_iterator iter = eg.io.begin(),
      end = eg.io.end();
  for (; iter != end; ++iter) {
    const NnetIo &io = *iter;
    int32 node_index = nnet_->GetNodeIndex(io.name);
    KALDI_ASSERT(node_index >= 0);
    if (nnet_->IsOutputNode(node_index)) {
      ObjectiveType obj_type = nnet_->GetNode(node_index).u.objective_type;
      BaseFloat tot_weight, tot_objf;
      bool supply_deriv = true;
      ComputeObjectiveFunction(io.features, obj_type, io.name,
                               supply_deriv, computer,
                               &tot_weight, &tot_objf);
      objf_info_[io.name].UpdateStats(io.name, config_.print_interval,
                                      num_minibatches_processed_++,
                                      tot_weight, tot_objf);
    }
  }
}

bool NnetTrainer::PrintTotalStats() const {
  unordered_map<std::string, ObjectiveFunctionInfo>::const_iterator
      iter = objf_info_.begin(),
      end = objf_info_.end();
  bool ans = false;
  for (; iter != end; ++iter) {
    const std::string &name = iter->first;
    const ObjectiveFunctionInfo &info = iter->second;
    ans = ans || info.PrintTotalStats(name);
  }
  return ans;
}

void ObjectiveFunctionInfo::UpdateStats(
    const std::string &output_name,
    int32 minibatches_per_phase,
    int32 minibatch_counter,
    BaseFloat this_minibatch_weight,
    BaseFloat this_minibatch_tot_objf) {
  int32 phase = minibatch_counter / minibatches_per_phase;
  if (phase != current_phase) {
    KALDI_ASSERT(phase == current_phase + 1); // or doesn't really make sense.
    PrintStatsForThisPhase(output_name, minibatches_per_phase);
    current_phase = phase;
    tot_weight_this_phase = 0.0;
    tot_objf_this_phase = 0.0;
  }
  tot_weight_this_phase += this_minibatch_weight;
  tot_objf_this_phase += this_minibatch_tot_objf;
  tot_weight += this_minibatch_weight;
  tot_objf += this_minibatch_tot_objf;
}

void ObjectiveFunctionInfo::PrintStatsForThisPhase(
    const std::string &output_name,
    int32 minibatches_per_phase) const {
  int32 start_minibatch = current_phase * minibatches_per_phase,
      end_minibatch = start_minibatch + minibatches_per_phase - 1;
  KALDI_LOG << "Average objective function for '" << output_name
            << "' for minibatches " << start_minibatch
            << '-' << end_minibatch << " is "
            << (tot_objf_this_phase / tot_weight_this_phase) << " over "
            << tot_weight_this_phase << " frames.";
}

bool ObjectiveFunctionInfo::PrintTotalStats(const std::string &name) const {
  KALDI_LOG << "Overall average objective function for '" << name << "'is "
            << (tot_objf / tot_weight) << " over " << tot_weight << " frames.";
  return (tot_weight != 0.0);
}

void ComputeObjectiveFunction(const GeneralMatrix &supervision,
                              ObjectiveType objective_type,
                              const std::string &output_name,
                              bool supply_deriv,
                              NnetComputer *computer,
                              BaseFloat *tot_weight,
                              BaseFloat *tot_objf) {
  const CuMatrixBase<BaseFloat> &output = computer->GetOutput(output_name);

  if (output.NumCols() != supervision.NumCols())
    KALDI_ERR << "Nnet versus example output dimension (num-classes) "
              << "mismatch for '" << output_name << "': " << output.NumCols()
              << " (nnet) vs. " << supervision.NumCols() << " (egs)\n";
  
  switch (objective_type) {
    case kLinear: {
      // objective is x * y.
      switch (supervision.Type()) {
        case kSparseMatrix: {
          const SparseMatrix<BaseFloat> &post = supervision.GetSparseMatrix();
          CuSparseMatrix<BaseFloat> cu_post(post);
          // The cross-entropy objective is computed by a simple dot product,
          // because after the LogSoftmaxLayer, the output is already in the form
          // of log-likelihoods that are normalized to sum to one.
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatSmat(output, cu_post, kTrans);
          if (supply_deriv) {
            CuMatrix<BaseFloat> output_deriv(output.NumRows(), output.NumCols(),
                                             kUndefined);
            cu_post.CopyToMat(&output_deriv);
            computer->AcceptOutputDeriv(output_name, &output_deriv);
          }
          break;
        }
        case kFullMatrix: {
          // there is a redundant matrix copy in here if we're not using a GPU
          // but we don't anticipate this code branch being used in many cases.
          CuMatrix<BaseFloat> cu_post(supervision.GetFullMatrix());
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptOutputDeriv(output_name, &cu_post);
          break;
        }
        case kCompressedMatrix: {
          Matrix<BaseFloat> post;
          supervision.GetMatrix(&post);
          CuMatrix<BaseFloat> cu_post;
          cu_post.Swap(&post);
          *tot_weight = cu_post.Sum();
          *tot_objf = TraceMatMat(output, cu_post, kTrans);
          if (supply_deriv)
            computer->AcceptOutputDeriv(output_name, &cu_post);
          break;
        }
      }
      break;
    }
    case kQuadratic: {
      // objective is -0.5 (x - y)^2
      CuMatrix<BaseFloat> diff(supervision.NumRows(),
                               supervision.NumCols(),
                               kUndefined);
      diff.CopyFromGeneralMat(supervision);
      diff.AddMat(-1.0, output);
      *tot_weight = diff.NumRows();
      *tot_objf = -0.5 * TraceMatMat(diff, diff, kTrans);
      if (supply_deriv)
        computer->AcceptOutputDeriv(output_name, &diff);
      break;
    }
    default:
      KALDI_ERR << "Objective function type " << objective_type
                << " not handled.";
  }      
}



} // namespace nnet3
} // namespace kaldi
