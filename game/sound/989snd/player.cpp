// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "player.h"

#include <fstream>

#include "sfxblock.h"
#include "blocksound_handler.h"  // FIX 121: instance-limit pre-check reads BlockSoundHandler state

#include "fmt/format.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <combaseapi.h>
#include <windows.h>
#endif
#include "common/log/log.h"

#ifdef __SWITCH__
// FIX 70 (AI-assisted): core pinning for cubeb's audio thread.
#include <array>
#include <atomic>
#include <map>

#include "game/switch/platform.h"
#include "game/switch/run_log.h"
#endif

namespace snd {

#ifdef __SWITCH__
// Audio dropout probe (AI-assisted): why do some sounds (Dark Jak punches) not play?
// Counts per 10 s window, written to gk_run_log.txt by the audio thread. Only touched
// under mTickLock, so plain ints are enough.
namespace {
int s_aud_plays = 0, s_aud_nobank = 0, s_aud_nohandler = 0, s_aud_limit = 0;
// per sound: [0] new play rejected by the instance limit, [1] older instance stopped for it
std::map<std::pair<SoundBank*, u32>, std::array<int, 2>> s_aud_limited;
// FIX 122 (AI-assisted): handlers that finished less than 24 ticks (~0.1 s at 240 Hz)
// after starting. A sound that dies at birth gets re-fired by GOAL every frame -- the
// ctywid retrigger storm measured 2026-11-18 runs one sound at up to 220 plays/s while
// "live" stays at 0-8, and it starves the instance limit so real one-shots (gunshots)
// get dropped. Naming the sound needs a reverse lookup in the bank's (small) name table.
int s_aud_instant = 0;
std::map<std::pair<SoundBank*, u32>, int> s_aud_instant_map;
const char* aud_sound_name(SoundBank* bank, u32 sound_id) {
  auto* block = dynamic_cast<SFXBlock*>(bank);
  if (block) {
    for (const auto& [name, id] : block->Names) {
      if (id == sound_id) {
        return name.c_str();
      }
    }
  }
  return "?";
}
void aud_report_top(int which, const char* what) {
  const std::pair<SoundBank*, u32>* best = nullptr;
  int n = 0;
  for (auto& [k, v] : s_aud_limited) {
    if (v[which] > n) {
      n = v[which];
      best = &k;
    }
  }
  if (best) {
    auto name = best->first->GetName();
    switch_run_logf("[aud]   top %s: bank %.*s sound %u (%s) x%d", what,
                    name ? (int)name->size() : 1, name ? name->data() : "?", best->second,
                    aud_sound_name(best->first, best->second), n);
  }
}
void aud_report_top_instant() {
  // FIX 123 (AI-assisted): top-3, not top-1 -- the f122 log showed several distinct
  // instant-death populations (steam vent, footsteps, ambients) and one line hid all
  // but the biggest.
  for (int rank = 0; rank < 3; rank++) {
    const std::pair<SoundBank*, u32>* best = nullptr;
    int n = 0;
    for (auto& [k, v] : s_aud_instant_map) {
      if (v > n) {
        n = v;
        best = &k;
      }
    }
    if (!best || n == 0) {
      break;
    }
    auto name = best->first->GetName();
    switch_run_logf("[aud]   top%d instant-death: bank %.*s sound %u (%s) x%d", rank + 1,
                    name ? (int)name->size() : 1, name ? name->data() : "?", best->second,
                    aud_sound_name(best->first, best->second), n);
    s_aud_instant_map.erase(*best);
  }
}
}  // namespace
#endif

u8 g_global_excite = 0;

Player::Player() : mVmanager(mSynth) {
  InitCubeb();
}

Player::~Player() {
  DestroyCubeb();
}

void Player::InitCubeb() {
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_coinitialized = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    lg::error("Couldn't initialize COM");
    lg::error("Cubeb init failed");
    return;
  }
#endif

  cubeb_init(&mCtx, "OpenGOAL", nullptr);

  cubeb_stream_params outparam = {};
  outparam.channels = 2;
  outparam.format = CUBEB_SAMPLE_S16LE;
  outparam.rate = 48000;
  outparam.layout = CUBEB_LAYOUT_STEREO;
  outparam.prefs = CUBEB_STREAM_PREF_NONE;

  s32 err = 0;
  u32 latency = 0;
  err = cubeb_get_min_latency(mCtx, &outparam, &latency);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }

  err = cubeb_stream_init(mCtx, &mStream, "OpenGOAL", nullptr, nullptr, nullptr, &outparam, latency,
                          &sound_callback, &state_callback, this);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }

  err = cubeb_stream_start(mStream);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }
}

void Player::DestroyCubeb() {
  // FIX 45: guard the teardown. InitCubeb() can bail out early (SDL_InitSubSystem or
  // SDL_OpenAudioDevice failing) leaving mStream/mCtx null, and the old code called
  // cubeb_stream_stop/destroy/destroy unconditionally. The Switch shim tolerates a null stream,
  // but the desktop cubeb does not, so check before touching them.
  if (mStream) {
    cubeb_stream_stop(mStream);
    cubeb_stream_destroy(mStream);
    mStream = nullptr;
  }
  if (mCtx) {
    cubeb_destroy(mCtx);
    mCtx = nullptr;
  }
#ifdef _WIN32
  if (m_coinitialized) {
    CoUninitialize();
    m_coinitialized = false;
  }
#endif
}

long Player::sound_callback([[maybe_unused]] cubeb_stream* stream,
                            void* user,
                            [[maybe_unused]] const void* input,
                            void* output_buffer,
                            long nframes) {
#ifdef __SWITCH__
  // FIX 70 (AI-assisted): cubeb owns this thread and the only handle we ever
  // get to it is right here, inside the callback. Pin it once (first
  // callback) to core 2 with the other I/O workers (PERF_PLAN_NEXT_AGENT.md
  // step 1).
  static std::atomic<bool> s_audio_pinned{false};
  if (!s_audio_pinned.exchange(true)) {
    switch_platform::switch_pin_current_thread("audio", 2);
  }
#endif
  ((Player*)user)->Tick((s16Output*)output_buffer, nframes);
  return nframes;
}

void Player::state_callback([[maybe_unused]] cubeb_stream* stream,
                            [[maybe_unused]] void* user,
                            [[maybe_unused]] cubeb_state state) {}

void Player::Tick(s16Output* stream, int samples) {
  std::scoped_lock lock(mTickLock);
  static int htick = 200;
  static int stick = 48000;
  for (int i = 0; i < samples; i++) {
    // The handlers expect to tick at 240hz
    // 48000/240 = 200
    if (htick == 200) {
      mTick++;

      for (auto it = mHandlers.begin(); it != mHandlers.end();) {
        bool done = it->second->Tick();
        if (done) {
#ifdef __SWITCH__
          // FIX 122 (AI-assisted): instant-death probe. Runs only when a handler
          // finishes (a few times per second), never on the hot path.
          if (auto* bh = dynamic_cast<BlockSoundHandler*>(it->second.get())) {
            if (mTick - bh->m_start_tick < 24) {
              s_aud_instant++;
              s_aud_instant_map[{&bh->Bank(), bh->m_sound_id}]++;
              // FIX 123 (AI-assisted): two full snapshots per sound, then aggregates
              // only. last= is the grain type that ran last (STOP = the sound program
              // branched to its stop grain; a sample/cull decision made by the bank
              // data, not by us), next=/grains= show whether the grain list was
              // burned through with zero delays, voice= whether a VAG voice ever
              // started. vol fields separate "played at ~0 volume" from the rest.
              static std::map<std::pair<SoundBank*, u32>, int> s_aud_detail_n;
              if (++s_aud_detail_n[{&bh->Bank(), bh->m_sound_id}] <= 2) {
                auto bank_name = bh->Bank().GetName();
                switch_run_logf(
                    "[aud] instant %.*s#%u (%s): ticks=%d grains=%u next=%u last=%d "
                    "voice=%d paused=%d appvol=%d curvol=%d pan=%d countdown=%d "
                    "reg=%d,%d,%d,%d",
                    bank_name ? (int)bank_name->size() : 1, bank_name ? bank_name->data() : "?",
                    bh->m_sound_id, aud_sound_name(&bh->Bank(), bh->m_sound_id),
                    mTick - bh->m_start_tick, (unsigned)bh->m_sfx->Grains.size(),
                    bh->m_next_grain, bh->m_last_grain_type, (int)bh->m_had_voice,
                    (int)bh->m_paused, bh->m_app_volume, bh->m_cur_volume, bh->m_cur_pan,
                    bh->m_countdown, bh->m_registers[0], bh->m_registers[1],
                    bh->m_registers[2], bh->m_registers[3]);
              }
            }
          }
#endif
          // fmt::print("erasing handler\n");
          mHandleAllocator.FreeId(it->first);
          it = mHandlers.erase(it);
        } else {
          ++it;
        }
      }

      htick = 0;
    }

    if (stick == 48000) {
      // fmt::print("{} handlers active\n", m_handlers.size());
      stick = 0;
#ifdef __SWITCH__
      static int s_secs = 0;
      if (++s_secs == 10) {
        s_secs = 0;
        switch_run_logf("[aud] 10s plays %d nobank %d nohandler %d limit %d instant %d | live %d",
                        s_aud_plays, s_aud_nobank, s_aud_nohandler, s_aud_limit, s_aud_instant,
                        (int)mHandlers.size());
        aud_report_top(0, "rejected");
        aud_report_top(1, "cut-off");
        aud_report_top_instant();
        s_aud_limited.clear();
        s_aud_instant_map.clear();
        s_aud_plays = s_aud_nobank = s_aud_nohandler = s_aud_limit = s_aud_instant = 0;
      }
#endif
    }

    stick++;
    htick++;
    *stream++ = mSynth.Tick();
  }
}

u32 Player::PlaySound(BankHandle bank_id, u32 sound_id, s32 vol, s32 pan, s32 pm, s32 pb) {
  std::scoped_lock lock(mTickLock);
  auto bank = mLoader.GetBankByHandle(bank_id);
#ifdef __SWITCH__
  s_aud_plays++;
#endif
  if (bank == nullptr) {
#ifdef __SWITCH__
    s_aud_nobank++;
#endif
    lg::error("play_sound: Bank {} does not exist", static_cast<void*>(bank_id));
    return 0;
  }

  u32 handle = mHandleAllocator.GetId();

  // FIX 121 (AI-assisted): instance-limit pre-check, before MakeHandler. GOAL retriggers
  // limited sounds at up to hundreds of plays per second (one hardware window measured 4190
  // plays / 4019 rejected), and the old order built a full BlockSoundHandler first -- its
  // constructor even executes immediate grains (voice allocation, blocksound_handler.cpp
  // 104-108) -- just for CheckInstanceLimit to discard it a moment later. All of that ran
  // under mTickLock, which the ~21 ms SDL audio callback also needs; the callback falling
  // behind is what desynced the 240 Hz sound clock from the frame clock (up to +0.7 s per
  // 10 s of busy gameplay, measured between the [aud] 10s lines in gk_run_log.txt). This
  // replicates BlockSoundHandler::CheckInstanceLimit's reject decision without constructing
  // anything: a doomed play now costs one cheap map scan instead of a handler build+destroy
  // under the audio lock. The steal branch (tick/vol flags) still goes through the original
  // MakeHandler + CheckInstanceLimit path, unchanged.
  if (auto* block = dynamic_cast<SFXBlock*>(bank)) {
    if (sound_id < block->Sounds.size() && !block->Sounds[sound_id].Grains.empty()) {
      const auto& def = block->Sounds[sound_id];
      if (def.InstanceLimit && def.Flags.has_instlimit()) {
        int inst = 0;
        s32 min_app_volume = 0;  // valid as soon as inst > 0
        for (const auto& [id, handler_ptr] : mHandlers) {
          auto* handler = dynamic_cast<BlockSoundHandler*>(handler_ptr.get());
          if (handler && (handler->m_orig_sfx == &def || handler->m_sfx == &def)) {
            if (inst == 0 || handler->m_app_volume < min_app_volume) {
              min_app_volume = handler->m_app_volume;
            }
            inst++;
          }
        }
        // CheckInstanceLimit rejects the new play when inst >= limit and it may not steal:
        // the tick flag always steals the oldest instead, and with the vol flag a louder
        // new play still steals from the quietest existing one.
        if (def.InstanceLimit - 1 < inst && !def.Flags.instlimit_tick() &&
            (!def.Flags.instlimit_vol() || min_app_volume >= vol)) {
#ifdef __SWITCH__
          s_aud_limit++;
          s_aud_limited[{bank, sound_id}][0]++;
#endif
          return 0;
        }
      }
    }
  }

  auto handler = bank->MakeHandler(mVmanager, sound_id, vol, pan, pm, pb, GetTick(), handle);
  if (!handler.has_value()) {
#ifdef __SWITCH__
    s_aud_nohandler++;
#endif
    return 0;
  }

  auto handler_to_stop = handler.value()->CheckInstanceLimit(mHandlers, vol, true);
  if (handler_to_stop) {
#ifdef __SWITCH__
    s_aud_limit++;
    s_aud_limited[{bank, sound_id}][handler_to_stop == handler.value().get() ? 0 : 1]++;
#endif
    handler_to_stop->Stop();
    if (handler_to_stop == handler.value().get()) {
      return 0;
    }
  }

  handler.value()->m_sound_handle = handle;
  mHandlers.emplace(handle, std::move(handler.value()));
  // fmt::print("play_sound {}:{} - {}\n", bank_id, sound_id, handle);

  return handle;
}

void Player::DebugPrintAllSoundsInBank(BankHandle bank_id) {
  std::scoped_lock lock(mTickLock);
  auto* bank = mLoader.GetBankByHandle(bank_id);
  if (!bank) {
    lg::error("DebugPrintAllSoundsInBank: invalid bank");
    return;
  }
  bank->DebugPrintAllSounds();
}

u32 Player::PlaySoundByName(BankHandle bank_id,
                            char* bank_name,
                            char* sound_name,
                            s32 vol,
                            s32 pan,
                            s32 pm,
                            s32 pb) {
  std::scoped_lock lock(mTickLock);
  SoundBank* bank = nullptr;
  if (bank_id == 0 && bank_name != nullptr) {
    bank = mLoader.GetBankByName(bank_name);
  } else if (bank_id != 0) {
    bank = mLoader.GetBankByHandle(bank_id);
  } else {
    bank = mLoader.GetBankWithSound(sound_name);
  }

  if (bank == nullptr) {
    // lg::error("play_sound_by_name: failed to find bank for sound {}", sound_name);
    return 0;
  }

  auto sound = bank->GetSoundByName(sound_name);
  if (sound.has_value()) {
    return PlaySound(bank, sound.value(), vol, pan, pm, pb);
  }

  // lg::error("play_sound_by_name: failed to find sound {}", sound_name);

  return 0;
}

void Player::StopSound(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Stop();

  // m_handle_allocator.free_id(sound_id);
  // m_handlers.erase(sound_id);
}

u32 Player::GetSoundID(u32 sound_handle) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_handle);
  if (handler == mHandlers.end())
    return -1;
  return handler->second->SoundID();
}

void Player::SetSoundReg(u32 sound_id, u8 reg, u8 value) {
  std::scoped_lock lock(mTickLock);
  if (mHandlers.find(sound_id) == mHandlers.end()) {
    // fmt::print("set_midi_reg: Handler {} does not exist\n", sound_id);
    return;
  }

  auto* handler = mHandlers.at(sound_id).get();
  handler->SetRegister(reg, value);
}

u8 Player::GetSoundGroup(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end()) {
    return 0;
  }
  return handler->second->Group();
}

bool Player::SoundStillActive(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return false;

  // fmt::print("sound_still_active {}\n", sound_id);
  return true;
}

void Player::SetMasterVolume(u32 group, s32 volume) {
  std::scoped_lock lock(mTickLock);
  if (volume > 0x400)
    volume = 0x400;

  if (volume < 0)
    volume = 0;

  if (group == 15)
    return;

  mVmanager.SetMasterVol(group, volume);

  // Master volume
  if (group == 16) {
    mSynth.SetMasterVol(0x3ffff * volume / 0x400);
  }
}

BankHandle Player::LoadBank(std::span<u8> bank) {
  // FIX 121 (AI-assisted): parse the bank OUTSIDE mTickLock. The old scoped_lock wrapped
  // Loader::BankLoad's whole multi-MB sample memcpy + grain parse, and GOAL swaps district
  // banks constantly while roaming (see the [aud] bank unload lines); each swap held the
  // audio callback's lock for far longer than its ~21 ms deadline -> underruns -> the 240 Hz
  // sound clock fell behind the frame clock (the measured audio desync). mBanks is only ever
  // touched from the GOAL thread -- the audio Tick never reads it -- so the parse needs no
  // lock at all and only the cheap AddBank insert keeps one, for ordering with the other
  // GOAL-side Player calls that hold it.
  auto parsed = mLoader.ParseBank(bank);
  if (!parsed) {
    return nullptr;
  }
  std::scoped_lock lock(mTickLock);
  return mLoader.AddBank(std::move(parsed));
}

void Player::UnloadBank(BankHandle bank_handle) {
  std::scoped_lock lock(mTickLock);
  auto* bank = mLoader.GetBankByHandle(bank_handle);
  if (bank == nullptr)
    return;

  // FIX 96 (AI-assisted): stop the outgoing bank's handlers before dropping
  // them. The old code erased the handler entries without keying their voices
  // off, so a bank swap mid-loop (jak3 swaps the district bank when crossing
  // Haven City boundaries - "Unload soundbank ctyslmbh / Load ctyslmah" in the
  // data log) silently killed long-lived loops like the traffic-vehicle engine
  // - the "zoomer sound goes away while riding" report - and left voice grains
  // owned by a handler that was destroyed a moment later. Stop() is the same
  // path Player::StopSound uses.
  u32 stopped_live = 0;
  for (auto it = mHandlers.begin(); it != mHandlers.end();) {
    if (&it->second->Bank() == bank_handle) {
      it->second->Stop();
      stopped_live++;
      mHandleAllocator.FreeId(it->first);
      it = mHandlers.erase(it);
    } else {
      ++it;
    }
  }
  if (stopped_live) {
    fmt::print("[aud] FIX 96 bank unload stopped {} live sound handler(s)\n", stopped_live);
#ifdef __SWITCH__
    switch_run_logf("[aud] bank unload stopped %u live sound(s)", stopped_live);
#endif
  }
#ifdef __SWITCH__
  s_aud_limited.clear();  // its keys hold bank pointers
  s_aud_instant_map.clear();  // FIX 122: same reason -- bank pointers in the keys
#endif

  mLoader.UnloadBank(bank_handle);
}

void Player::SetPanTable(VolPair* pantable) {
  std::scoped_lock lock(mTickLock);
  mVmanager.SetPanTable(pantable);
}

void Player::SetPlaybackMode(s32 mode) {
  std::scoped_lock lock(mTickLock);
  mVmanager.SetPlaybackMode(mode);
}

void Player::PauseSound(s32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Pause();
}

void Player::ContinueSound(s32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Unpause();
}

void Player::PauseAllSoundsInGroup(u8 group) {
  std::scoped_lock lock(mTickLock);

  for (auto& h : mHandlers) {
    if ((1 << h.second->Group()) & group) {
      h.second->Pause();
    }
  }
}

void Player::ContinueAllSoundsInGroup(u8 group) {
  std::scoped_lock lock(mTickLock);

  for (auto& h : mHandlers) {
    if ((1 << h.second->Group()) & group) {
      h.second->Unpause();
    }
  }
}

void Player::SetSoundVolPan(s32 sound_id, s32 vol, s32 pan) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->SetVolPan(vol, pan);
}

void Player::SetSoundPmod(s32 sound_handle, s32 mod) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_handle);
  if (handler == mHandlers.end())
    return;

  handler->second->SetPMod(mod);
}

void Player::StopAllSounds() {
  std::scoped_lock lock(mTickLock);
  for (auto it = mHandlers.begin(); it != mHandlers.end();) {
    mHandleAllocator.FreeId(it->first);
    it = mHandlers.erase(it);
  }
}

s32 Player::GetSoundUserData(BankHandle block_handle,
                             char* block_name,
                             s32 sound_id,
                             char* sound_name,
                             SFXUserData* dst) {
  std::scoped_lock lock(mTickLock);
  SoundBank* bank = nullptr;
  if (block_handle == nullptr && block_name != nullptr) {
    bank = mLoader.GetBankByName(block_name);
  } else if (block_handle != nullptr) {
    bank = mLoader.GetBankByHandle(block_handle);
  } else {
    bank = mLoader.GetBankWithSound(sound_name);
  }

  if (bank == nullptr) {
    return 0;
  }

  if (sound_id == -1) {
    auto sound = bank->GetSoundByName(sound_name);
    if (sound.has_value()) {
      sound_id = sound.value();
    } else {
      return 0;
    }
  }

  auto ud = bank->GetSoundUserData(sound_id);
  if (ud.has_value()) {
    dst->data[0] = ud.value()->data[0];
    dst->data[1] = ud.value()->data[1];
    dst->data[2] = ud.value()->data[2];
    dst->data[3] = ud.value()->data[3];
    return 1;
  } else {
    return 0;
  }

  return 0;
}

}  // namespace snd
