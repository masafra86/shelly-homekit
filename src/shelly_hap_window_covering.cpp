/*
 * Copyright (c) 2020 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shelly_hap_window_covering.hpp"

#include "mgos.h"

namespace shelly {
namespace hap {

WindowCovering::WindowCovering(int id, Input *in0, Input *in1, Output *out0,
                               Output *out1, PowerMeter *pm0, PowerMeter *pm1,
                               struct mgos_config_wc *cfg)
    : Component(id),
      Service((SHELLY_HAP_IID_BASE_WINDOW_COVERING +
               (SHELLY_HAP_IID_STEP_WINDOW_COVERING * (id - 1))),
              &kHAPServiceType_WindowCovering,
              kHAPServiceDebugDescription_WindowCovering),
      inputs_({in0, in1}),
      outputs_({out0, out1}),
      pms_({pm0, pm1}),
      cfg_(cfg),
      state_timer_(std::bind(&WindowCovering::RunOnce, this)),
      cur_pos_(cfg_->current_pos),
      move_ms_per_pct_(cfg_->move_time_ms / 100.0) {
}

WindowCovering::~WindowCovering() {
  for (size_t i = 0; i < inputs_.size(); i++) {
    auto *in = inputs_[i];
    if (in == nullptr) continue;
    in->RemoveHandler(input_handlers_[i]);
  }
}

Status WindowCovering::Init() {
  uint16_t iid = svc_.iid + 1;
  // Name
  AddNameChar(iid++, cfg_->name);
  // Target Position
  AddChar(new UInt8Characteristic(
      iid++, &kHAPCharacteristicType_TargetPosition, 0, 100, 1,
      [this](HAPAccessoryServerRef *, const HAPUInt8CharacteristicReadRequest *,
             uint8_t *value) {
        *value = static_cast<uint8_t>(tgt_pos_ >= 0 ? tgt_pos_ : cur_pos_);
        return kHAPError_None;
      },
      true /* supports_notification */,
      [this](HAPAccessoryServerRef *,
             const HAPUInt8CharacteristicWriteRequest *, uint8_t value) {
        SetTgtPos(value);
        return kHAPError_None;
      },
      kHAPCharacteristicDebugDescription_TargetPosition));
  // Current Position
  AddChar(new UInt8Characteristic(
      iid++, &kHAPCharacteristicType_CurrentPosition, 0, 100, 1,
      [this](HAPAccessoryServerRef *, const HAPUInt8CharacteristicReadRequest *,
             uint8_t *value) {
        *value = cur_pos_;
        return kHAPError_None;
      },
      true /* supports_notification */, nullptr /* write_handler */,
      kHAPCharacteristicDebugDescription_CurrentPosition));
  // Position State
  AddChar(new UInt8Characteristic(
      iid++, &kHAPCharacteristicType_PositionState, 0, 2, 1,
      [this](HAPAccessoryServerRef *, const HAPUInt8CharacteristicReadRequest *,
             uint8_t *value) {
        *value = 2; /* Stopped. TODO */
        return kHAPError_None;
      },
      true /* supports_notification */, nullptr /* write_handler */,
      kHAPCharacteristicDebugDescription_PositionState));
  // Hold Position
  AddChar(new BoolCharacteristic(
      iid++, &kHAPCharacteristicType_HoldPosition, nullptr /* read_handler */,
      false /* supports_notification */,
      [this](HAPAccessoryServerRef *, const HAPBoolCharacteristicWriteRequest *,
             bool value) {
        if (value) {
          LOG(LL_INFO, ("%d: Hold position", id()));
          SetState(State::kStop);
        }
        return kHAPError_None;
      },
      kHAPCharacteristicDebugDescription_HoldPosition));
  // Obstruction Detected
  AddChar(new BoolCharacteristic(
      iid++, &kHAPCharacteristicType_ObstructionDetected,
      [this](HAPAccessoryServerRef *, const HAPBoolCharacteristicReadRequest *,
             bool *value) {
        *value = false; /* TODO */
        return kHAPError_None;
      },
      true /* supports_notification */, nullptr /* write_handler */,
      kHAPCharacteristicDebugDescription_ObstructionDetected));

  for (size_t i = 0; i < inputs_.size(); i++) {
    auto *in = inputs_[i];
    if (in != nullptr) {
      auto handler_id = in->AddHandler(
          std::bind(&WindowCovering::HandleInputEvent, this, i, _1, _2));
      input_handlers_.push_back(handler_id);
    } else {
      input_handlers_.push_back(Input::kInvalidHandlerID);
    }
  }
  if (cfg_->calibrated) {
    LOG(LL_INFO, ("%d: mp %.2f, mt_ms %d, cur_pos %.2f", id(), cfg_->move_power,
                  cfg_->move_time_ms, cur_pos_));
  } else {
    LOG(LL_INFO, ("%d: not calibrated", id()));
  }
  state_timer_.Reset(100, MGOS_TIMER_REPEAT);
  return Status::OK();
}

StatusOr<std::string> WindowCovering::GetInfo() const {
  int cal_state = (cfg_->calibrated ? 1 : 0);
  return mgos::JSONPrintStringf(
      "{id: %d, type: %d, name: %Q, state: %d, state_str: %Q, "
      "cal_state: %d, cur_pos: %d, tgt_pos: %d}",
      id(), type(), cfg_->name, (int) state_, StateStr(state_), cal_state,
      (int) cur_pos_, (int) tgt_pos_);
}

Status WindowCovering::SetConfig(const std::string &config_json,
                                 bool *restart_required) {
  struct mgos_config_wc cfg = *cfg_;
  cfg.name = nullptr;
  int state = -1, cal_result = -1, tgt_pos = -1;
  json_scanf(config_json.c_str(), config_json.size(),
             "{name: %Q, state: %d, cal_result: %d, tgt_pos: %d}", &cfg.name,
             &state, &cal_result, &tgt_pos);
  mgos::ScopedCPtr name_owner((void *) cfg.name);
  if (state >= 0) {
    SetState(static_cast<State>(state));
    return Status::OK();
  }
  if (cal_result == 0 || cal_result == 100) {
    if (state_ == State::kPostCal0) {
      LOG(LL_INFO, ("open_output_idx = %d", cal_result));
      cfg_->open_output_idx = cal_result;
      SetCurPos(cal_result == 0 ? kFullyClosed : kFullyOpen);
      SetState(State::kPreCal1);
    }
  }
  if (tgt_pos >= 0) {
    auto st = SetTgtPos(tgt_pos);
    if (!st.ok()) {
      return mgos::Annotatef(st, "failed to set target position");
    }
  }
  return Status::OK();
}

// static
const char *WindowCovering::StateStr(State state) {
  switch (state) {
    case State::kIdle:
      return "idle";
    case State::kPreCal0:
      return "precal0";
    case State::kCal0:
      return "cal0";
    case State::kPostCal0:
      return "postcal0";
    case State::kPreCal1:
      return "precal1";
    case State::kCal1:
      return "cal1";
    case State::kPostCal1:
      return "postcal1";
    case State::kMove:
      return "move";
    case State::kMoving:
      return "moving";
    case State::kStop:
      return "stop";
    case State::kStopping:
      return "stopping";
    case State::kError:
      return "error";
  }
  return "???";
}

void WindowCovering::SaveState() {
  mgos_sys_config_save(&mgos_sys_config, false /* try_once */, NULL /* msg */);
}

void WindowCovering::SetState(State new_state) {
  if (state_ == new_state) return;
  LOG(LL_INFO,
      ("%d: State transition: %s -> %s (%d -> %d)", id(), StateStr(state_),
       StateStr(new_state), (int) state_, (int) new_state));
  state_ = new_state;
}

void WindowCovering::SetCurPos(float new_cur_pos) {
  if (new_cur_pos == cur_pos_) return;
  LOG(LL_INFO, ("%d: Current pos %.2f -> %.2f", id(), cur_pos_, new_cur_pos));
  cur_pos_ = new_cur_pos;
  cfg_->current_pos = cur_pos_;
  if (new_cur_pos == kFullyOpen || new_cur_pos == kFullyClosed ||
      (std::abs(new_cur_pos - last_notify_pos_) > 2.0)) {
    SaveState();
    chars_[2]->RaiseEvent();
    last_notify_pos_ = new_cur_pos;
  }
}

Status WindowCovering::SetTgtPos(float new_tgt_pos) {
  if (new_tgt_pos < kFullyClosed || new_tgt_pos > kFullyOpen) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT,
                        "%d: Invalid target position %f", id(), new_tgt_pos);
  }
  LOG(LL_INFO, ("%d: Target pos %.2f -> %.2f", id(), tgt_pos_, new_tgt_pos));
  tgt_pos_ = new_tgt_pos;
  chars_[1]->RaiseEvent();
  return Status::OK();
}

WindowCovering::Direction WindowCovering::GetDesiredMoveDirection() {
  if (tgt_pos_ == kNotSet) return Direction::kNone;
  float pos_diff = tgt_pos_ - cur_pos_;
  if (!cfg_->calibrated || std::abs(pos_diff) < 2) {
    return Direction::kNone;
  }
  return (pos_diff > 0 ? Direction::kOpen : Direction::kClose);
}

void WindowCovering::Move(Direction dir) {
  const char *ss = StateStr(state_);
  bool os[2] = {false, false};
  if (cfg_->calibrated) {
    switch (dir) {
      case Direction::kNone:
        break;
      case Direction::kOpen:
        os[cfg_->open_output_idx] = true;
        break;
      case Direction::kClose:
        os[!cfg_->open_output_idx] = true;
        break;
    }
  }
  outputs_[0]->SetState(os[0], ss);
  outputs_[1]->SetState(os[1], ss);
  move_dir_ = dir;
}

void WindowCovering::RunOnce() {
  const char *ss = StateStr(state_);
  if (state_ != State::kIdle) {
    LOG(LL_DEBUG, ("%d: %s md %d pos %.2f -> %.2f", id(), ss, (int) move_dir_,
                   cur_pos_, tgt_pos_));
  }
  switch (state_) {
    case State::kIdle: {
      Move(Direction::kNone);
      if (GetDesiredMoveDirection() != Direction::kNone) {
        SetState(State::kMove);
      }
      break;
    }
    case State::kPreCal0: {
      LOG(LL_INFO, ("Begin calibration"));
      cfg_->calibrated = false;
      SaveState();
      outputs_[0]->SetState(true, ss);
      outputs_[1]->SetState(false, ss);
      SetState(State::kCal0);
      break;
    }
    case State::kCal0: {
      auto p0v = pms_[0]->GetPowerW();
      if (!p0v.ok()) {
        LOG(LL_ERROR, ("PM error"));
        SetState(State::kError);
        break;
      }
      const float p0 = p0v.ValueOrDie();
      LOG(LL_INFO, ("P0 = %.3f", p0));
      if (p0 < 1) {
        outputs_[0]->SetState(false, ss);
        SetState(State::kPostCal0);
      }
      break;
    }
    case State::kPostCal0: {
      outputs_[0]->SetState(false, ss);
      break;
    }
    case State::kPreCal1: {
      outputs_[1]->SetState(true, ss);
      begin_ = mgos_uptime_micros();
      p_sum_ = 0;
      p_num_ = 0;
      SetState(State::kCal1);
      break;
    }
    case State::kCal1: {
      auto p1v = pms_[1]->GetPowerW();
      if (!p1v.ok()) {
        LOG(LL_ERROR, ("PM error"));
        SetState(State::kError);
        break;
      }
      const float p1 = p1v.ValueOrDie();
      LOG(LL_INFO, ("%d: P1 = %.3f", id(), p1));
      if (p_num_ > 0 && p1 < 1) {
        end_ = mgos_uptime_micros();
        outputs_[1]->SetState(false, StateStr(state_));
        int move_time_ms = (end_ - begin_) / 1000;
        float move_power = p_sum_ / p_num_;
        LOG(LL_INFO, ("%d: calibration done, move_time %d, move_power %.3f",
                      id(), move_time_ms, move_power));
        cfg_->move_time_ms = move_time_ms;
        cfg_->move_power = move_power;
        move_ms_per_pct_ = cfg_->move_time_ms / 100.0;
        SetState(State::kPostCal1);
      } else {
        p_sum_ += p1;
        p_num_++;
      }
      break;
    }
    case State::kPostCal1: {
      cfg_->calibrated = true;
      SetCurPos(cfg_->open_output_idx == 0 ? kFullyClosed : kFullyOpen);
      SaveState();
      SetTgtPos(50);
      SetState(State::kIdle);
      break;
    }
    case State::kMove: {
      Direction dir = GetDesiredMoveDirection();
      if (dir == Direction::kNone) {
        SetState(State::kStop);
        break;
      }
      move_start_pos_ = cur_pos_;
      begin_ = mgos_uptime_micros();
      Move(dir);
      SetState(State::kMoving);
      break;
    }
    case State::kMoving: {
      int moving_time_ms = (mgos_uptime_micros() - begin_) / 1000;
      float pos_diff = moving_time_ms / move_ms_per_pct_;
      float new_cur_pos =
          (move_dir_ == Direction::kOpen ? move_start_pos_ + pos_diff
                                         : move_start_pos_ - pos_diff);
      SetCurPos(new_cur_pos);
      int pm_idx = (move_dir_ == Direction::kOpen ? cfg_->open_output_idx
                                                  : !cfg_->open_output_idx);
      auto pmv = pms_[pm_idx]->GetPowerW();
      float pm = 0;
      if (pmv.ok()) {
        pm = pmv.ValueOrDie();
      } else {
        LOG(LL_ERROR, ("PM error"));
        SetState(State::kError);
        break;
      }
      // If moving to one of the limit positions, keep moving
      if ((tgt_pos_ == kFullyOpen && move_dir_ == Direction::kOpen) ||
          (tgt_pos_ == kFullyClosed && move_dir_ == Direction::kClose)) {
        LOG(LL_DEBUG, ("Moving to %d, pm %.2f", (int) tgt_pos_, pm));
        if (pm > 1) {
          // Still moving.
          break;
        } else {
          SetCurPos(move_dir_ == Direction::kOpen ? kFullyOpen : kFullyClosed);
        }
      } else {
        if (GetDesiredMoveDirection() == move_dir_) {
          // Still moving.
          break;
        }
      }
      Move(Direction::kNone);  // Stop moving immediately to minimize error.
      LOG(LL_INFO, ("Finished moving"));
      SetState(State::kStop);
      break;
    }
    case State::kStop: {
      Move(Direction::kNone);
      SetState(State::kStopping);
      break;
    }
    case State::kStopping: {
      float p0 = 0, p1 = 0;
      auto p0v = pms_[0]->GetPowerW();
      if (p0v.ok()) p0 = p0v.ValueOrDie();
      auto p1v = pms_[1]->GetPowerW();
      if (p1v.ok()) p1 = p1v.ValueOrDie();
      if (p0 < 1 && p1 < 1) {
        SetState(State::kIdle);
      }
      break;
    }
    case State::kError: {
      Move(Direction::kNone);
      break;
    }
  }
}

void WindowCovering::HandleInputEvent(int index, Input::Event ev, bool state) {
}

}  // namespace hap
}  // namespace shelly